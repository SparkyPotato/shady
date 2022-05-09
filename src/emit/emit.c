#include "list.h"
#include "dict.h"

#include "../log.h"
#include "../portability.h"
#include "../type.h"
#include "../analysis/scope.h"

#include "spirv_builder.h"

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

typedef struct SpvFileBuilder* FileBuilder;
typedef struct SpvFnBuilder* FnBuilder;
typedef struct SpvBasicBlockBuilder* BBBuilder;

typedef struct Emitter_ {
    IrArena* arena;
    CompilerConfig* configuration;
    FileBuilder file_builder;
    SpvId void_t;
    struct Dict* node_ids;
} Emitter;

SpvStorageClass emit_addr_space(AddressSpace address_space) {
    switch(address_space) {
        case AsGlobalLogical:   return SpvStorageClassStorageBuffer;
        case AsSharedLogical:   return SpvStorageClassCrossWorkgroup;
        case AsPrivateLogical:  return SpvStorageClassPrivate;
        case AsFunctionLogical: return SpvStorageClassFunction;

        case AsGeneric: error("not implemented");
        case AsGlobalPhysical: return SpvStorageClassPhysicalStorageBuffer;
        case AsSharedPhysical:
        case AsSubgroupPhysical:
        case AsPrivatePhysical: error("This should have been lowered before");

        case AsInput: return SpvStorageClassInput;
        case AsOutput: return SpvStorageClassOutput;

        // TODO: depending on platform, use push constants/ubos/ssbos here
        case AsExternal: return SpvStorageClassStorageBuffer;
        default: SHADY_NOT_IMPLEM;
    }
}

SpvId emit_type(Emitter* emitter, const Type* type);
SpvId emit_value(Emitter* emitter, const Node* node, const SpvId* use_id);

static void register_result(Emitter* emitter, const Node* variable, SpvId id) {
    spvb_name(emitter->file_builder, id, variable->payload.var.name);
    insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, variable, id);
}

static void emit_primop(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, PrimOp prim_op, Nodes variables) {
    Nodes args = prim_op.operands;
    LARRAY(SpvId, arr, args.count);
    for (size_t i = 0; i < args.count; i++)
        arr[i] = args.nodes[i] ? emit_value(emitter, args.nodes[i], NULL) : 0;

    SpvId i32_t = emit_type(emitter, int_type(emitter->arena));

    switch (prim_op.op) {
        case add_op: register_result(emitter, variables.nodes[0], spvb_binop(bb_builder, SpvOpIAdd, i32_t, arr[0], arr[1])); break;
        case sub_op: register_result(emitter, variables.nodes[0], spvb_binop(bb_builder, SpvOpISub, i32_t, arr[0], arr[1])); break;
        case mul_op: register_result(emitter, variables.nodes[0], spvb_binop(bb_builder, SpvOpIMul, i32_t, arr[0], arr[1])); break;
        case div_op: register_result(emitter, variables.nodes[0], spvb_binop(bb_builder, SpvOpSDiv, i32_t, arr[0], arr[1])); break;
        case mod_op: register_result(emitter, variables.nodes[0], spvb_binop(bb_builder, SpvOpSMod, i32_t, arr[0], arr[1])); break;
        case load_op: {
            assert(without_qualifier(args.nodes[0]->type)->tag == PtrType_TAG);
            const Type* elem_type = without_qualifier(args.nodes[0]->type)->payload.ptr_type.pointed_type;
            SpvId eptr = emit_value(emitter, args.nodes[0], NULL);
            SpvId result = spvb_load(bb_builder, emit_type(emitter, elem_type), eptr, 0, NULL);
            register_result(emitter, variables.nodes[0], result);
            break;
        }
        case store_op: {
            assert(without_qualifier(args.nodes[0]->type)->tag == PtrType_TAG);
            SpvId eptr = emit_value(emitter, args.nodes[0], NULL);
            SpvId eval = emit_value(emitter, args.nodes[1], NULL);
            spvb_store(bb_builder, eval, eptr, 0, NULL);
            break;
        }
        case alloca_op: {
            const Type* elem_type = args.nodes[0];
            SpvId result = spvb_local_variable(fn_builder, emit_type(emitter, ptr_type(emitter->arena, (PtrType) {
                .address_space = AsFunctionLogical,
                .pointed_type = elem_type
            })), SpvStorageClassFunction);
            register_result(emitter, variables.nodes[0], result);
            break;
        }
        case lea_op: {
            if (arr[1]) {
                error("TODO: OpPtrAccessChain")
            } else {
                const Type* target_type = typecheck_primop(emitter->arena, prim_op).nodes[0];
                SpvId result = spvb_access_chain(bb_builder, emit_type(emitter, target_type), arr[0], args.count - 2, &arr[2]);
                register_result(emitter, variables.nodes[0], result);
            }
            break;
        }
        default: error("TODO: unhandled op");
    }
}

static BBBuilder emit_if(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, If if_instr, Nodes variables) {
    SpvId next_id = spvb_fresh_id(emitter->file_builder);
    BBBuilder next = spvb_begin_bb(fn_builder, next_id);

    SpvId true_id = spvb_fresh_id(emitter->file_builder);
    SpvId false_id = if_instr.if_false ? spvb_fresh_id(emitter->file_builder) : next_id;

    spvb_selection_merge(bb_builder, next_id, 0);
    SpvId condition = emit_value(emitter, if_instr.condition, NULL);
    spvb_branch_conditional(bb_builder, condition, true_id, false_id);

    // emit_bb
    error("TODO");

    return next;
}

static BBBuilder emit_loop(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, Loop loop_instr, Nodes variables) {
    SpvId next_id = spvb_fresh_id(emitter->file_builder);
    BBBuilder next = spvb_begin_bb(fn_builder, next_id);
    error("TODO");
    return next;
}

BBBuilder emit_let(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, const Node* let_node) {
    Nodes variables = let_node->payload.let.variables;
    const Node* instruction = let_node->payload.let.instruction;
    switch (instruction->tag) {
        case PrimOp_TAG: {
            emit_primop(emitter, fn_builder, bb_builder, instruction->payload.prim_op, variables);
            return bb_builder;
        }
        case Call_TAG: error("TODO emit calls")
        case If_TAG: emit_if(emitter, fn_builder, bb_builder, instruction->payload.if_instr, variables);
        case Loop_TAG: emit_loop(emitter, fn_builder, bb_builder, instruction->payload.loop_instr, variables);
        default: error("Unrecognised instruction %s", node_tags[instruction->tag]);
    }
    SHADY_UNREACHABLE;
}

static SpvId find_reserved_id(Emitter* emitter, const Node* node) {
    SpvId* found = find_value_dict(const Node*, SpvId, emitter->node_ids, node);
    assert(found);
    return *found;
}

void emit_terminator(Emitter* emitter, FnBuilder fn_builder, BBBuilder basic_block_builder, const Node* terminator) {
    switch (terminator->tag) {
        case Return_TAG: {
            const Nodes* ret_values = &terminator->payload.fn_ret.values;
            switch (ret_values->count) {
                case 0: spvb_return_void(basic_block_builder); return;
                case 1: spvb_return_value(basic_block_builder, emit_value(emitter, ret_values->nodes[0], NULL)); return;
                default: {
                    LARRAY(SpvId, arr, ret_values->count);
                    for (size_t i = 0; i < ret_values->count; i++)
                        arr[i] = emit_value(emitter, ret_values->nodes[i], NULL);
                    SpvId return_that = spvb_composite(basic_block_builder, fn_ret_type_id(fn_builder), ret_values->count, arr);
                    spvb_return_value(basic_block_builder, return_that);
                    return;
                }
            }
        }
        case Jump_TAG: {
            assert(terminator->payload.jump.args.count == 0 && "TODO: implement bb params");
            spvb_branch(basic_block_builder, find_reserved_id(emitter, terminator->payload.jump.target));
            return;
        }
        case Branch_TAG: {
            assert(terminator->payload.branch.args.count == 0 && "TODO: implement bb params");

            SpvId condition = emit_value(emitter, terminator->payload.branch.condition, NULL);
            spvb_branch_conditional(basic_block_builder, condition, find_reserved_id(emitter, terminator->payload.branch.true_target), find_reserved_id(emitter, terminator->payload.branch.false_target));
            return;
        }
        case Merge_TAG: error("merge terminators are supposed to be eliminated in the instr2bb pass");
        case Unreachable_TAG: {
            spvb_unreachable(basic_block_builder);
            return;
        }
        default: error("TODO: emit terminator %s", node_tags[terminator->tag]);
    }
    SHADY_UNREACHABLE;
}

static void emit_block(Emitter* emitter, FnBuilder fn_builder, BBBuilder basic_block_builder, const Node* node) {
    assert(node->tag == Block_TAG);
    const Block* block = &node->payload.block;
    for (size_t i = 0; i < block->instructions.count; i++)
        basic_block_builder = emit_let(emitter, fn_builder, basic_block_builder, block->instructions.nodes[i]);
    emit_terminator(emitter, fn_builder, basic_block_builder, block->terminator);
}

void emit_basic_block(Emitter* emitter, FnBuilder fn_builder, const CFNode* node) {
    assert(node->node->tag == Function_TAG);
    // Find the preassigned ID to this
    SpvId reserved_id = find_reserved_id(emitter, node->node);
    BBBuilder basic_block_builder = spvb_begin_bb(fn_builder, reserved_id);
    spvb_name(emitter->file_builder, reserved_id, node->node->payload.fn.name);

    emit_block(emitter, fn_builder, basic_block_builder, node->node->payload.fn.block);

    // Emit the child nodes for real
    size_t dom_count = entries_count_list(node->dominates);
    for (size_t i = 0; i < dom_count; i++) {
        CFNode* child_node = read_list(CFNode*, node->dominates)[i];
        emit_basic_block(emitter, fn_builder, child_node);
    }
}

static SpvId nodes_to_codom(Emitter* emitter, Nodes return_types) {
    switch (return_types.count) {
        case 0: return emitter->void_t;
        case 1: return emit_type(emitter, return_types.nodes[0]);
        default: {
            const Type* codom_ret_type = record_type(emitter->arena, (RecordType) {.members = return_types});
            return emit_type(emitter, codom_ret_type);
        }
    }
}

void emit_function(Emitter* emitter, const Node* node) {
    assert(node->tag == Function_TAG);

    const Type* fn_type = node->type;
    FnBuilder fn_builder = spvb_begin_fn(emitter->file_builder, find_reserved_id(emitter, node), emit_type(emitter, fn_type), nodes_to_codom(emitter, node->payload.fn.return_types));

    Nodes params = node->payload.fn.params;
    for (size_t i = 0; i < params.count; i++) {
        SpvId param_id = spvb_parameter(fn_builder, emit_type(emitter, params.nodes[i]->payload.var.type));
        insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, params.nodes[i], param_id);
    }

    Scope scope = build_scope(node);
    emit_basic_block(emitter, fn_builder, scope.entry);
    dispose_scope(&scope);

    spvb_define_function(emitter->file_builder, fn_builder);
}

SpvId emit_value(Emitter* emitter, const Node* node, const SpvId* use_id) {
    SpvId* existing = find_value_dict(struct Node*, SpvId, emitter->node_ids, node);
    if (existing)
        return *existing;

    SpvId new = use_id ? *use_id : spvb_fresh_id(emitter->file_builder);
    insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, node, new);

    switch (node->tag) {
        case Variable_TAG: error("this node should have been resolved already");
        case IntLiteral_TAG: {
            SpvId ty = emit_type(emitter, node->type);
            uint32_t arr[] = { node->payload.int_literal.value };
            spvb_constant(emitter->file_builder, new, ty, 1, arr);
            break;
        }
        case True_TAG: {
            spvb_bool_constant(emitter->file_builder, new, emit_type(emitter, bool_type(emitter->arena)), true);
            break;
        }
        case False_TAG: {
            spvb_bool_constant(emitter->file_builder, new, emit_type(emitter, bool_type(emitter->arena)), false);
            break;
        }
        default: error("don't know hot to emit value");
    }
    return new;
}

SpvId emit_type(Emitter* emitter, const Type* type) {
    SpvId* existing = find_value_dict(struct Node*, SpvId, emitter->node_ids, type);
    if (existing)
        return *existing;
    
    SpvId new;
    switch (type->tag) {
        case Int_TAG:
            new = spvb_int_type(emitter->file_builder, 32, true);
            break;
        case Bool_TAG:
            new = spvb_bool_type(emitter->file_builder);
            break;
        case PtrType_TAG: {
            SpvId pointee = emit_type(emitter, type->payload.ptr_type.pointed_type);
            SpvStorageClass sc = emit_addr_space(type->payload.ptr_type.address_space);
            new = spvb_ptr_type(emitter->file_builder, sc, pointee);
            break;
        }
        case RecordType_TAG: {
            LARRAY(SpvId, members, type->payload.record_type.members.count);
            for (size_t i = 0; i < type->payload.record_type.members.count; i++)
                members[i] = emit_type(emitter, type->payload.record_type.members.nodes[i]);
            new = spvb_struct_type(emitter->file_builder, type->payload.record_type.members.count, members);
            break;
        }
        case FnType_TAG: {
            const FnType* fnt = &type->payload.fn_type;
            assert(!fnt->is_continuation);
            LARRAY(SpvId, params, fnt->param_types.count);
            for (size_t i = 0; i < fnt->param_types.count; i++)
                params[i] = emit_type(emitter, fnt->param_types.nodes[i]);

            new = spvb_fn_type(emitter->file_builder, fnt->param_types.count, params, nodes_to_codom(emitter, fnt->return_types));
            break;
        }
        case QualifiedType_TAG: {
            // SPIR-V does not care about our type qualifiers.
            new = emit_type(emitter, type->payload.qualified_type.type);
            break;
        }
        default: error("Don't know how to emit type")
    }

    insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, type, new);
    return new;
}

void emit_spirv(CompilerConfig* config, IrArena* arena, const Node* root_node, FILE* output) {
    const Root* top_level = &root_node->payload.root;
    struct List* words = new_list(uint32_t);

    FileBuilder file_builder = spvb_begin();

    Emitter emitter = {
        .configuration = config,
        .arena = arena,
        .file_builder = file_builder,
        .node_ids = new_dict(Node*, SpvId, (HashFn) hash_node, (CmpFn) compare_node),
    };

    emitter.void_t = spvb_void_type(emitter.file_builder);

    spvb_capability(file_builder, SpvCapabilityShader);
    spvb_capability(file_builder, SpvCapabilityLinkage);
    spvb_capability(file_builder, SpvCapabilityPhysicalStorageBufferAddresses);

    // First reserve IDs for declarations
    LARRAY(SpvId, ids, top_level->declarations.count);
    int global_fn_case_number = 1;
    for (size_t i = 0; i < top_level->declarations.count; i++) {
        const Node* decl = top_level->declarations.nodes[i];
        ids[i] = spvb_fresh_id(file_builder);
        insert_dict_and_get_result(struct Node*, SpvId, emitter.node_ids, decl, ids[i]);
    }

    if (config->use_loop_for_fn_calls) {
        // TODO: generate outer dispatcher function
    }

    for (size_t i = 0; i < top_level->declarations.count; i++) {
        const Node* decl = top_level->declarations.nodes[i];
        switch (decl->tag) {
            case GlobalVariable_TAG: {
                const GlobalVariable* gvar = &decl->payload.global_variable;
                SpvId init = 0;
                if (gvar->init)
                    init = emit_value(&emitter, gvar->init, NULL);
                spvb_global_variable(file_builder, ids[i], emit_type(&emitter, gvar->type), emit_addr_space(gvar->address_space), false, init);
                spvb_name(file_builder, ids[i], gvar->name);
                break;
            } case Function_TAG: {
                emit_function(&emitter, decl);
                spvb_name(file_builder, ids[i], decl->payload.fn.name);
                break;
            } case Constant_TAG: {
                const Constant* cnst = &decl->payload.constant;
                emit_value(&emitter, cnst->value, &ids[i]);
                spvb_name(file_builder, ids[i], cnst->name);
                break;
            }
            default: error("unhandled declaration kind")
        }
    }

    spvb_finish(file_builder, words);

    // cleanup the emitter
    destroy_dict(emitter.node_ids);

    fwrite(words->alloc, words->elements_count, 4, output);

    destroy_list(words);
}
