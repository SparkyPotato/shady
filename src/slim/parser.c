#include "token.h"
#include "parser.h"

#include "../containers/list.h"

#include "../log.h"
#include "../type.h"
#include "../portability.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

extern const char* token_tags[];

// to avoid some repetition
#define ctxparams SHADY_UNUSED ParserConfig config, SHADY_UNUSED char* contents, SHADY_UNUSED IrArena* arena, SHADY_UNUSED struct Tokenizer* tokenizer
#define ctx config, contents, arena, tokenizer

#define expect(condition) expect_impl(condition, #condition)
static void expect_impl(bool condition, const char* err) {
    if (!condition) {
        error_print("expected to parse: %s\n", err);
        exit(-4);
    }
}

static bool accept_token(ctxparams, enum TokenTag tag) {
    if (curr_token(tokenizer).tag == tag) {
        next_token(tokenizer);
        return true;
    }
    return false;
}

static const char* accept_identifier(ctxparams) {
    struct Token tok = curr_token(tokenizer);
    if (tok.tag == identifier_tok) {
        next_token(tokenizer);
        size_t size = tok.end - tok.start;
        return string_sized(arena, (int) size, &contents[tok.start]);
    }
    return NULL;
}

static const Node* expect_block(ctxparams, const Node*);
static const Node* accept_value(ctxparams);
static const Node* accept_primop(ctxparams);
static const Node* accept_expr(ctxparams);

static AddressSpace expect_ptr_address_space(ctxparams) {
    switch (curr_token(tokenizer).tag) {
        case global_tok:  next_token(tokenizer); return AsGlobalPhysical;
        case private_tok: next_token(tokenizer); return AsPrivatePhysical;
        case shared_tok:  next_token(tokenizer); return AsSharedPhysical;
        default: error("expected address space qualifier");
    }
    SHADY_UNREACHABLE;
}

static const Type* accept_unqualified_type(ctxparams) {
    if (accept_token(ctx, int_tok)) {
        return int_type(arena);
    } else if (accept_token(ctx, float_tok)) {
        return float_type(arena);
    } else if (accept_token(ctx, bool_tok)) {
        return bool_type(arena);
    } else if (accept_token(ctx, mask_tok)) {
        return mask_type(arena);
    } else if (accept_token(ctx, ptr_tok)) {
        AddressSpace as = expect_ptr_address_space(ctx);
        const Type* elem_type = accept_unqualified_type(ctx);
        expect(elem_type);
        return ptr_type(arena, (PtrType) {
           .address_space = as,
           .pointed_type = elem_type,
        });
    } else if (config.front_end && accept_token(ctx, lsbracket_tok)) {
        const Type* elem_type = accept_unqualified_type(ctx);
        assert(elem_type);
        expect(accept_token(ctx, star_tok));
        const Node* expr = accept_value(ctx);
        expect(expr);
        expect(accept_token(ctx, rsbracket_tok));
        return arr_type(arena, (ArrType) {
            .element_type = elem_type,
            .size = expr
        });
    } else {

        return NULL;
    }
}

static DivergenceQualifier accept_uniformity_qualifier(ctxparams) {
    DivergenceQualifier divergence = Unknown;
    if (accept_token(ctx, uniform_tok))
        divergence = Uniform;
    else if (accept_token(ctx, varying_tok))
        divergence = Varying;
    return divergence;
}

static const Type* accept_maybe_qualified_type(ctxparams) {
    DivergenceQualifier qualifier = accept_uniformity_qualifier(ctx);
    const Type* unqualified = accept_unqualified_type(ctx);
    if (qualifier != Unknown)
        expect(unqualified && "we read a uniformity qualifier and expected a type to follow");
    if (qualifier == Unknown)
        return unqualified;
    else
        return qualified_type(arena, (QualifiedType) { .is_uniform = qualifier == Uniform, .type = unqualified });
}

static const Type* accept_qualified_type(ctxparams) {
    DivergenceQualifier qualifier = accept_uniformity_qualifier(ctx);
    if (qualifier == Unknown)
        return NULL;
    const Type* unqualified = accept_unqualified_type(ctx);
    expect(unqualified);
    return qualified_type(arena, (QualifiedType) { .is_uniform = qualifier == Uniform, .type = unqualified });
}

static const Node* accept_operand(ctxparams) {
    return config.front_end ? accept_expr(ctx) : accept_value(ctx);
}

static void expect_parameters(ctxparams, Nodes* parameters, Nodes* default_values) {
    expect(accept_token(ctx, lpar_tok));
    struct List* params = new_list(Node*);
    struct List* default_vals = default_values ? new_list(Node*) : NULL;

    while (true) {
        if (accept_token(ctx, rpar_tok))
            break;

        next: {
            const Type* qtype = accept_qualified_type(ctx);
            expect(qtype);
            const char* id = accept_identifier(ctx);
            expect(id);

            const Node* node = var(arena, qtype, id);
            append_list(Node*, params, node);

            if (default_values) {
                expect(accept_token(ctx, equal_tok));
                const Node* default_val = accept_operand(ctx);
                append_list(const Node*, default_vals, default_val);
            }

            if (accept_token(ctx, comma_tok))
                goto next;
        }
    }

    size_t count = entries_count_list(params);
    *parameters = nodes(arena, count, read_list(const Node*, params));
    destroy_list(params);
    if (default_values) {
        *default_values = nodes(arena, count, read_list(const Node*, default_vals));
        destroy_list(default_vals);
    }
}

static Nodes accept_types(ctxparams, enum TokenTag separator, bool expect_qualified) {
    struct List* types = new_list(Type*);
    while (true) {
        const Type* type = expect_qualified ? accept_qualified_type(ctx) : accept_maybe_qualified_type(ctx);
        if (!type)
            break;

        append_list(Type*, types, type);

        if (separator != 0)
            accept_token(ctx, separator);
    }

    Nodes types2 = nodes(arena, types->elements_count, (const Type**) types->alloc);
    destroy_list(types);
    return types2;
}

static const Node* accept_literal(ctxparams) {
    struct Token tok = curr_token(tokenizer);
    switch (tok.tag) {
        case dec_lit_tok: {
            next_token(tokenizer);
            size_t size = tok.end - tok.start;
            return untyped_number(arena, (UntypedNumber) {
                .plaintext = string_sized(arena, (int) size, &contents[tok.start])
            });
            //int64_t value = strtol(&contents[tok.start], NULL, 10)
            //return untyped_number(value);
        }
        case true_tok: next_token(tokenizer); return true_lit(arena);
        case false_tok: next_token(tokenizer); return false_lit(arena);
        default: return NULL;
    }
}

static const Node* accept_value(ctxparams) {
    const char* id = accept_identifier(ctx);
    if (id) {
        return unbound(arena, (Unbound) {
            .name = id
        });
    }

    const Node* lit = accept_literal(ctx);
    if (lit) return lit;

    return NULL;
}

static const Node* accept_expr(ctxparams) {
    const Node* expr = accept_value(ctx);
    if (!expr) expr = accept_primop(ctx);
    return expr;
}

static Strings expect_identifiers(ctxparams) {
    struct List* list = new_list(const char*);
    while (true) {
        const char* id = accept_identifier(ctx);
        expect(id);

        append_list(const char*, list, id);

        if (accept_token(ctx, comma_tok))
            continue;
        else
            break;
    }

    Strings final = strings(arena, list->elements_count, (const char**) list->alloc);
    destroy_list(list);
    return final;
}

static Nodes expect_operands(ctxparams) {
    if (!accept_token(ctx, lpar_tok))
        error("Expected left parenthesis")

    struct List* list = new_list(Node*);

    bool expect = false;
    while (true) {
        const Node* val = accept_operand(ctx);
        if (!val) {
            if (expect)
                error("expected value but got none")
            else if (accept_token(ctx, rpar_tok))
                break;
            else
                error("Expected value or closing parenthesis")
        }

        append_list(Node*, list, val);

        if (accept_token(ctx, comma_tok))
            expect = true;
        else if (accept_token(ctx, rpar_tok))
            break;
        else
            error("Expected comma or closing parenthesis")
    }

    Nodes final = nodes(arena, list->elements_count, (const Node**) list->alloc);
    destroy_list(list);
    return final;
}

static const Node* accept_control_flow_instruction(ctxparams) {
    struct Token current_token = curr_token(tokenizer);
    switch (current_token.tag) {
        case if_tok: {
            next_token(tokenizer);
            Nodes yield_types = accept_types(ctx, 0, false);
            expect(accept_token(ctx, lpar_tok));
            const Node* condition = accept_operand(ctx);
            expect(condition);
            expect(accept_token(ctx, rpar_tok));
            const Node* merge = config.front_end ? merge_construct(arena, (MergeConstruct) { .construct = Selection }) : NULL;
            const Node* if_true = expect_block(ctx, merge);
            bool has_else = accept_token(ctx, else_tok);
            // default to an empty block
            const Node* if_false = NULL;
            if (has_else) {
                if_false = expect_block(ctx, merge);
            }
            return if_instr(arena, (If) {
                .yield_types = yield_types,
                .condition = condition,
                .if_true = if_true,
                .if_false = if_false
            });
        }
        case loop_tok: {
            next_token(tokenizer);
            Nodes yield_types = accept_types(ctx, 0, false);
            Nodes parameters;
            Nodes default_values;
            expect_parameters(ctx, &parameters, &default_values);
            const Node* body = expect_block(ctx, config.front_end ? merge_construct(arena, (MergeConstruct) { .construct = Continue }) : NULL);
            return loop_instr(arena, (Loop) {
                .initial_args = default_values,
                .params = parameters,
                .yield_types = yield_types,
                .body = body
            });
        }
        default: break;
    }
    return NULL;
}

static const Node* accept_primop(ctxparams) {
    Op op;
    switch (curr_token(tokenizer).tag) {
        case load_tok: {
            next_token(tokenizer);
            expect(accept_token(ctx, lpar_tok));
            const Type* element_type = accept_unqualified_type(ctx);
            assert(!element_type && "we haven't enabled that syntax yet");
            if (element_type)
                expect(accept_token(ctx, comma_tok));
            const Node* ptr = accept_operand(ctx);
            expect(ptr);
            const Node* ops[] = {ptr};
            expect(accept_token(ctx, rpar_tok));
            expect(accept_token(ctx, semi_tok));
            return prim_op(arena, (PrimOp) {
                .op = load_op,
                .operands = nodes(arena, 1, ops)
            });
        }
        case store_tok: {
            next_token(tokenizer);
            expect(accept_token(ctx, lpar_tok));
            const Node* ptr = accept_operand(ctx);
            expect(ptr);
            expect(accept_token(ctx, comma_tok));
            const Node* data = accept_operand(ctx);
            expect(data);
            const Node* ops[] = {ptr, data};
            expect(accept_token(ctx, rpar_tok));
            expect(accept_token(ctx, semi_tok));
            return prim_op(arena, (PrimOp) {
                .op = store_op,
                .operands = nodes(arena, 2, ops)
            });
        }
        case alloca_tok: {
            next_token(tokenizer);
            expect(accept_token(ctx, lpar_tok));
            const Type* element_type = accept_unqualified_type(ctx);
            expect(element_type);
            const Node* ops[] = {element_type};
            expect(accept_token(ctx, rpar_tok));
            expect(accept_token(ctx, semi_tok));
            return prim_op(arena, (PrimOp) {
                .op = alloca_op,
                .operands = nodes(arena, 1, ops)
            });
        }
        case add_tok:    op = add_op; break;
        case sub_tok:    op = sub_op; break;
        case mul_tok:    op = mul_op; break;
        case div_tok:    op = div_op; break;
        case mod_tok:    op = mod_op; break;
        case lt_tok:     op = lt_op;  break;
        case lte_tok:    op = lte_op; break;
        case gt_tok:     op = gt_op;  break;
        case gte_tok:    op = gte_op; break;
        case eq_tok:     op = eq_op;  break;
        case neq_tok:    op = neq_op; break;
        case and_tok:    op = and_op; break;
        case or_tok:     op = or_op;  break;
        case xor_tok:    op = xor_op; break;
        case not_tok:    op = not_op; break;
        /// Only used for IR parsing
        /// Otherwise accept_expression handles this
        case call_tok: {
            next_token(tokenizer);
            expect(accept_token(ctx, lpar_tok));
            const Node* callee = accept_operand(ctx);
            assert(callee);
            expect(accept_token(ctx, rpar_tok));
            Nodes args = expect_operands(ctx);
            expect(accept_token(ctx, semi_tok));
            return call_instr(arena, (Call) {
                .callee = callee,
                .args = args,
            });
        }
        default: return NULL;
    }
    next_token(tokenizer);
    const Node* node = prim_op(arena, (PrimOp) {
        .op = op,
        .operands = expect_operands(ctx)
    });
    expect(accept_token(ctx, semi_tok));
    return node;
}

static const Node* accept_instruction(ctxparams) {
    const Node* instr = config.front_end ? accept_expr(ctx) : accept_primop(ctx);
    if (!instr) instr = accept_control_flow_instruction(ctx);
    return instr;
}

static const Node* accept_instruction_maybe_with_let_too(ctxparams) {
    Strings ids = strings(arena, 0, NULL);
    if (accept_token(ctx, let_tok)) {
        ids = expect_identifiers(ctx);
        expect(accept_token(ctx, equal_tok));
    }

    const Node* instruction = accept_instruction(ctx);

    if (ids.count > 0)
        expect(instruction);

    if (instruction == NULL)
        return NULL;

    if (ids.count == 0)
        return instruction;

    return let(arena, instruction, ids.count, ids.strings);
}

static const Node* accept_terminator(ctxparams) {
    struct Token current_token = curr_token(tokenizer);
    switch (current_token.tag) {
        case jump_tok: {
            next_token(tokenizer);
            expect(accept_token(ctx, lpar_tok));
            const Node* target = accept_operand(ctx);
            expect(accept_token(ctx, rpar_tok));
            expect(target);
            Nodes args = expect_operands(ctx);
            expect(accept_token(ctx, semi_tok));
            return branch(arena, (Branch) {
                .yield = false,
                .branch_mode = BrJump,
                .target = target,
                .args = args
            });
        }
        case branch_tok: {
            next_token(tokenizer);

            expect(accept_token(ctx, lpar_tok));
            const Node* condition = accept_value(ctx);
            expect(condition);
            expect(accept_token(ctx, comma_tok));
            const Node* true_target = accept_value(ctx);
            expect(true_target);
            expect(accept_token(ctx, comma_tok));
            const Node* false_target = accept_value(ctx);
            expect(false_target);
            expect(accept_token(ctx, rpar_tok));

            Nodes args = expect_operands(ctx);
            expect(accept_token(ctx, semi_tok));
            return branch(arena, (Branch) {
                .yield = false,
                .branch_mode = BrIfElse,
                .branch_condition = condition,
                .true_target = true_target,
                .false_target = false_target,
                .args = args
            });
        }
        case return_tok: {
            next_token(tokenizer);
            Nodes values = expect_operands(ctx);
            expect(accept_token(ctx, semi_tok));
            return fn_ret(arena, (Return) {
                .fn = NULL,
                .values = values
            });
        }
        case merge_tok: {
            next_token(tokenizer);
            Nodes values = expect_operands(ctx);
            expect(accept_token(ctx, semi_tok));
            return merge_construct(arena, (MergeConstruct) {
                .construct = Selection,
                .args = values
            });
        }
        case continue_tok: {
            next_token(tokenizer);
            Nodes values = expect_operands(ctx);
            expect(accept_token(ctx, semi_tok));
            return merge_construct(arena, (MergeConstruct) {
                .construct = Continue,
                .args = values
            });
        }
        case break_tok: {
            next_token(tokenizer);
            Nodes values = expect_operands(ctx);
            expect(accept_token(ctx, semi_tok));
            return merge_construct(arena, (MergeConstruct) {
                .construct = Break,
                .args = values
            });
        }
        case unreachable_tok: {
            next_token(tokenizer);
            expect(accept_token(ctx, semi_tok));
            return unreachable(arena);
        }
        default: break;
    }
    return NULL;
}

static const Node* expect_block(ctxparams, const Node* implicit_join) {
    expect(accept_token(ctx, lbracket_tok));
    struct List* instructions = new_list(Node*);

    Nodes continuations = nodes(arena, 0, NULL);
    Nodes continuations_names = nodes(arena, 0, NULL);

    while (true) {
        const Node* instruction = accept_instruction_maybe_with_let_too(ctx);
        if (!instruction) break;
        append_list(Node*, instructions, instruction);
    }

    Nodes instrs = nodes(arena, entries_count_list(instructions), read_list(const Node*, instructions));
    destroy_list(instructions);

    const Node* terminator = accept_terminator(ctx);

    if (!terminator) {
        if (implicit_join)
            terminator = implicit_join;
        else
            error("expected terminator: return, jump, branch ...");
    }

    if (curr_token(tokenizer).tag == identifier_tok) {
        struct List* conts = new_list(Node*);
        struct List* names = new_list(Node*);
        while (true) {
            const char* identifier = accept_identifier(ctx);
            if (!identifier)
                break;
            expect(accept_token(ctx, colon_tok));

            Nodes parameters;
            expect_parameters(ctx, &parameters, NULL);
            const Node* block = expect_block(ctx, NULL);

            FnAttributes attributes = {
                .is_continuation = true,
                .entry_point_type = NotAnEntryPoint
            };
            Node* continuation = fn(arena, attributes, identifier, parameters, nodes(arena, 0, NULL));
            continuation->payload.fn.block= block;
            const Node* contvar = var(arena, qualified_type(arena, (QualifiedType) {
                .type = derive_fn_type(arena, &continuation->payload.fn),
                .is_uniform = true
            }), identifier);
            append_list(Node*, conts, continuation);
            append_list(Node*, names, contvar);
        }

        continuations = nodes(arena, entries_count_list(conts), read_list(const Node*, conts));
        continuations_names = nodes(arena, entries_count_list(names), read_list(const Node*, names));
        destroy_list(conts);
        destroy_list(names);
    }

    expect(accept_token(ctx, rbracket_tok));

    return parsed_block(arena, (ParsedBlock) {
        .instructions = instrs,
        .continuations = continuations,
        .continuations_vars = continuations_names,
        .terminator = terminator,
    });
}

static const Node* accept_const(ctxparams) {
    if (!accept_token(ctx, const_tok))
        return NULL;

    const Type* type = accept_unqualified_type(ctx);
    const char* id = accept_identifier(ctx);
    expect(id);
    expect(accept_token(ctx, equal_tok));
    const Node* definition = accept_value(ctx);
    assert(definition);

    expect(accept_token(ctx, semi_tok));

    Node* cnst = constant(arena, id);
    cnst->payload.constant.value = definition;
    cnst->payload.constant.type_hint = type;
    return cnst;
}

static FnAttributes accept_fn_annotations(ctxparams) {
    FnAttributes annotations = {
        .is_continuation = false,
        .entry_point_type = NotAnEntryPoint
    };

    while (true) {
        if (accept_token(ctx, compute_tok)) {
            annotations.entry_point_type = true;
            continue;
        }
        break;
    }

    return annotations;
}

static const Node* accept_fn_decl(ctxparams) {
    if (!accept_token(ctx, fn_tok))
        return NULL;

    FnAttributes attributes = accept_fn_annotations(ctx);

    const char* id = accept_identifier(ctx);
    expect(id);
    Nodes types = accept_types(ctx, comma_tok, false);
    expect(curr_token(tokenizer).tag == lpar_tok);
    Nodes parameters;
    expect_parameters(ctx, &parameters, NULL);
    const Node *block1 = expect_block(ctx, /* TODO default return when void */ NULL);

    Node *function = fn(arena, attributes, id, parameters, types);
    function->payload.fn.block = block1;

    const Node* declaration = function;
    assert(declaration);

    return declaration;
}

static const Node* accept_global_var_decl(ctxparams) {
    AddressSpace as;
    if (accept_token(ctx, private_tok))
        as = AsPrivateLogical;
    else if (accept_token(ctx, shared_tok))
        as = AsSharedLogical;
    else if (accept_token(ctx, subgroup_tok))
        as = AsSubgroupPhysical;
    else if (accept_token(ctx, extern_tok))
        as = AsExternal;
    else if (accept_token(ctx, input_tok))
        as = AsInput;
    else if (accept_token(ctx, output_tok))
        as = AsOutput;
    else
        return NULL;

    const Type* type = accept_unqualified_type(ctx);
    expect(type);
    const char* id = accept_identifier(ctx);
    expect(id);
    expect(accept_token(ctx, semi_tok));

    return global_var(arena, type, id, as);
}

const Node* parse(ParserConfig config, char* contents, IrArena* arena) {
    struct Tokenizer* tokenizer = new_tokenizer(contents);

    struct List* declarations = new_list(const Node*);

    while (true) {
        struct Token token = curr_token(tokenizer);
        if (token.tag == EOF_tok)
            break;

        const Node* decl = accept_const(ctx);
        if (!decl)
            decl = accept_fn_decl(ctx);
        if (!decl)
            decl = accept_global_var_decl(ctx);
        
        if (decl) {
            debug_print("decl parsed : ");
            debug_node(decl);
            debug_print("\n");

            append_list(const Node*, declarations, decl);
            continue;
        }

        error_print("No idea what to parse here... (tok=(tag = %s, pos = %zu))\n", token_tags[token.tag], token.start);
        exit(-3);
    }

    size_t count = declarations->elements_count;

    const Node* n = root(arena, (Root) {
        .declarations = nodes(arena, count, read_list(const Node*, declarations)),
    });

    destroy_list(declarations);
    destroy_tokenizer(tokenizer);

    return n;
}
