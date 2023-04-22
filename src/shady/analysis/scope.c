#include "scope.h"
#include "looptree.h"
#include "log.h"

#include "list.h"
#include "dict.h"
#include "arena.h"
#include "../ir_private.h"

#include <stdlib.h>
#include <assert.h>

struct List* build_scopes(Module* mod) {
    struct List* scopes = new_list(Scope*);

    Nodes decls = get_module_declarations(mod);
    for (size_t i = 0; i < decls.count; i++) {
        const Node* decl = decls.nodes[i];
        if (decl->tag != Function_TAG) continue;
        Scope* scope = new_scope(decl);
        append_list(Scope*, scopes, scope);
    }

    return scopes;
}

KeyHash hash_node(const Node**);
bool compare_node(const Node**, const Node**);

typedef struct {
    Arena* arena;
    const Node* entry;
    LoopTree* lt;
    struct Dict* nodes;
    struct List* queue;
    struct List* contents;
} ScopeBuildContext;

CFNode* scope_lookup(Scope* scope, const Node* block) {
    CFNode** found = find_value_dict(const Node*, CFNode*, scope->map, block);
    if (found) return *found;
    assert(false);
}

static CFNode* get_or_enqueue(ScopeBuildContext* ctx, const Node* abs) {
    assert(is_abstraction(abs));
    assert(!is_function(abs) || abs == ctx->entry);
    CFNode** found = find_value_dict(const Node*, CFNode*, ctx->nodes, abs);
    if (found) return *found;

    CFNode* new = arena_alloc(ctx->arena, sizeof(CFNode));
    *new = (CFNode) {
        .node = abs,
        .succ_edges = new_list(CFEdge),
        .pred_edges = new_list(CFEdge),
        .rpo_index = SIZE_MAX,
        .idom = NULL,
        .dominates = NULL,
        .structurally_dominated = new_set(const Node*, (HashFn) hash_node, (CmpFn) compare_node),
    };
    insert_dict(const Node*, CFNode*, ctx->nodes, abs, new);
    append_list(Node*, ctx->queue, new);
    append_list(Node*, ctx->contents, new);
    return new;
}

static bool in_loop(LoopTree* lt, const Node* entry, const Node* block) {
    LTNode* lt_node = looptree_lookup(lt, block);
    assert(lt_node);
    LTNode* parent = lt_node->parent;
    assert(parent);

    while (parent) {
        if (entries_count_list(parent->cf_nodes) != 1)
            return false;

        if (read_list(CFNode*, parent->cf_nodes)[0]->node == entry)
            return true;

        parent = parent->parent;
    }

    return false;
}

static bool is_structural_edge(CFEdgeType edge_type) { return edge_type != ForwardEdge; }

/// Adds an edge to somewhere inside a basic block
static void add_edge(ScopeBuildContext* ctx, const Node* src, const Node* dst, CFEdgeType type) {
    assert(!is_function(dst));
    assert(is_structural_edge(type) == is_anonymous_lambda(dst));
    if (ctx->lt && !in_loop(ctx->lt, ctx->entry, dst))
        return;
    if (ctx->lt && dst == ctx->entry)
        return;

    CFNode* src_node = get_or_enqueue(ctx, src);
    CFNode* dst_node = get_or_enqueue(ctx, dst);
    if (is_anonymous_lambda(dst)) {
        assert(entries_count_list(dst_node->pred_edges) == 0 && "anonymous lambdas can only be used once");
    }
    CFEdge edge = {
        .type = type,
        .src = src_node,
        .dst = dst_node,
    };
    append_list(CFEdge, src_node->succ_edges, edge);
    append_list(CFEdge, dst_node->pred_edges, edge);
}

static void add_structural_edge(ScopeBuildContext* ctx, CFNode* parent, const Node* dst, CFEdgeType type) {
    add_edge(ctx, parent->node, dst, type);
    insert_set_get_result(const Node*, parent->structurally_dominated, dst);
}

static void process_instruction(ScopeBuildContext* ctx, CFNode* parent, const Node* instruction) {
    switch (is_instruction(instruction)) {
        case NotAnInstruction: if (instruction->arena->config.check_types) { error("Grammar problem"); } break;
        case Instruction_Call_TAG:
        case Instruction_PrimOp_TAG:
        case Instruction_Comment_TAG: break;
        case Instruction_If_TAG:
            add_structural_edge(ctx, parent, instruction->payload.if_instr.if_true, IfBodyEdge);
            if(instruction->payload.if_instr.if_false)
                add_structural_edge(ctx, parent, instruction->payload.if_instr.if_false, IfBodyEdge);
            break;
        case Instruction_Match_TAG:
            for (size_t i = 0; i < instruction->payload.match_instr.cases.count; i++)
                add_structural_edge(ctx, parent, instruction->payload.match_instr.cases.nodes[i], MatchBodyEdge);
            add_structural_edge(ctx, parent, instruction->payload.match_instr.default_case, MatchBodyEdge);
            break;
        case Instruction_Loop_TAG:
            add_structural_edge(ctx, parent, instruction->payload.loop_instr.body, LoopBodyEdge);
            break;
        case Instruction_Control_TAG:
            add_structural_edge(ctx, parent, instruction->payload.control.inside, ControlBodyEdge);
            break;
        case Instruction_Block_TAG:
            add_structural_edge(ctx, parent, instruction->payload.block.inside, BlockBodyEdge);
            break;
    }
}

static void process_cf_node(ScopeBuildContext* ctx, CFNode* node) {
    const Node* const abs = node->node;
    assert(is_abstraction(abs));
    assert(!is_function(abs) || abs == ctx->entry);
    const Node* terminator = get_abstraction_body(abs);
    if (!terminator)
        return;
    switch (is_terminator(terminator)) {
        case Jump_TAG: {
            const Node* target = terminator->payload.jump.target;
            add_edge(ctx, abs, target, ForwardEdge);
            break;
        }
        case Branch_TAG: {
            const Node* true_target = terminator->payload.branch.true_target;
            const Node* false_target = terminator->payload.branch.false_target;
            add_edge(ctx, abs, true_target, ForwardEdge);
            add_edge(ctx, abs, false_target, ForwardEdge);
            break;
        }
        case Switch_TAG: {
            for (size_t i = 0; i < terminator->payload.br_switch.case_targets.count; i++) {
                const Node* case_tgt = terminator->payload.br_switch.case_targets.nodes[i];
                add_edge(ctx, abs, case_tgt, ForwardEdge);
            }
            const Node* default_target = terminator->payload.br_switch.default_target;
            add_edge(ctx, abs, default_target, ForwardEdge);
            break;
        }
        case LetMut_TAG:
        case Let_TAG: {
            process_instruction(ctx, node, get_let_instruction(terminator));
            const Node* target = get_let_tail(terminator);
            add_structural_edge(ctx, node, target, LetTailEdge);
            break;
        }
        case Join_TAG: {
            break;
        }
        case MergeContinue_TAG:
        case MergeBreak_TAG:
        case Yield_TAG: {
            // error("TODO: only allow this if we have traversed structured constructs...")
            break;
        }
        case TailCall_TAG:
        case Return_TAG:
        case Unreachable_TAG: break;
        case NotATerminator: if (terminator->arena->config.check_types) { error("Grammar problem"); } break;
    }
}

/**
 * Invert all edges in this scope. Used to compute a post dominance tree.
 */
static void flip_scope(Scope* scope) {
    scope->entry = NULL;

    for (size_t i = 0; i < scope->size; i++) {
        CFNode * cur = read_list(CFNode*, scope->contents)[i];

        struct List* tmp = cur->succ_edges;
        cur->succ_edges = cur->pred_edges;
        cur->pred_edges = tmp;

        for (size_t j = 0; j < entries_count_list(cur->succ_edges); j++) {
            CFEdge* edge = &read_list(CFEdge, cur->succ_edges)[j];

            CFNode* tmp = edge->dst;
            edge->dst = edge->src;
            edge->src = tmp;
        }

        for (size_t j = 0; j < entries_count_list(cur->pred_edges); j++) {
            CFEdge* edge = &read_list(CFEdge, cur->pred_edges)[j];

            CFNode* tmp = edge->dst;
            edge->dst = edge->src;
            edge->src = tmp;
        }

        if (entries_count_list(cur->pred_edges) == 0) {
            if (scope->entry != NULL) {
                if (scope->entry->node) {
                    CFNode* new_entry = arena_alloc(scope->arena, sizeof(CFNode));
                    *new_entry = (CFNode) {
                        .node = NULL,
                        .succ_edges = new_list(CFEdge),
                        .pred_edges = new_list(CFEdge),
                        .rpo_index = SIZE_MAX,
                        .idom = NULL,
                        .dominates = NULL,
                    };

                    CFEdge prev_entry_edge = {
                        .type = ForwardEdge,
                        .src = new_entry,
                        .dst = scope->entry
                    };
                    append_list(CFEdge, new_entry->succ_edges, prev_entry_edge);
                    append_list(CFEdge, scope->entry->pred_edges, prev_entry_edge);
                    scope->entry = new_entry;
                }

                CFEdge new_edge = {
                    .type = ForwardEdge,
                    .src = scope->entry,
                    .dst = cur
                };
                append_list(CFEdge, scope->entry->succ_edges, new_edge);
                append_list(CFEdge, cur->pred_edges, new_edge);
            } else {
                scope->entry = cur;
            }
        }
    }

    if (!scope->entry->node) {
        scope->size += 1;
        append_list(Node*, scope->contents, scope->entry);
    }
}


Scope* new_scope_impl(const Node* entry, LoopTree* lt, bool flipped) {
    assert(is_abstraction(entry));
    Arena* arena = new_arena();

    ScopeBuildContext context = {
        .arena = arena,
        .entry = entry,
        .lt = lt,
        .nodes = new_dict(const Node*, CFNode*, (HashFn) hash_node, (CmpFn) compare_node),
        .queue = new_list(CFNode*),
        .contents = new_list(CFNode*),
    };

    CFNode* entry_node = get_or_enqueue(&context, entry);

    while (entries_count_list(context.queue) > 0) {
        CFNode* this = pop_last_list(CFNode*, context.queue);
        process_cf_node(&context, this);
    }

    destroy_list(context.queue);

    Scope* scope = calloc(sizeof(Scope), 1);
    *scope = (Scope) {
        .arena = arena,
        .entry = entry_node,
        .size = entries_count_list(context.contents),
        .flipped = flipped,
        .contents = context.contents,
        .map = context.nodes,
        .rpo = NULL
    };

    if (flipped)
        flip_scope(scope);

    compute_rpo(scope);
    compute_domtree(scope);

    return scope;
}

void destroy_scope(Scope* scope) {
    bool entry_destroyed = false;
    for (size_t i = 0; i < scope->size; i++) {
        CFNode* node = read_list(CFNode*, scope->contents)[i];
        entry_destroyed |= node == scope->entry;
        destroy_list(node->pred_edges);
        destroy_list(node->succ_edges);
        if (node->dominates)
            destroy_list(node->dominates);
        if (node->structurally_dominated)
            destroy_dict(node->structurally_dominated);
    }
    if (!entry_destroyed) {
        destroy_list(scope->entry->pred_edges);
        destroy_list(scope->entry->succ_edges);
        if (scope->entry->dominates)
            destroy_list(scope->entry->dominates);
    }
    destroy_dict(scope->map);
    destroy_arena(scope->arena);
    free(scope->rpo);
    destroy_list(scope->contents);
    free(scope);
}

static size_t post_order_visit(Scope* scope, CFNode* n, size_t i) {
    n->rpo_index = -2;

    for (size_t j = 0; j < entries_count_list(n->succ_edges); j++) {
        CFEdge edge = read_list(CFEdge, n->succ_edges)[j];
        if (edge.dst->rpo_index == SIZE_MAX)
            i = post_order_visit(scope, edge.dst, i);
    }

    n->rpo_index = i - 1;
    scope->rpo[n->rpo_index] = n;
    return n->rpo_index;
}

void compute_rpo(Scope* scope) {
    scope->rpo = malloc(sizeof(const CFNode*) * scope->size);
    size_t index = post_order_visit(scope,  scope->entry, scope->size);
    assert(index == 0);

    // debug_print("RPO: ");
    // for (size_t i = 0; i < scope->size; i++) {
    //     debug_print("%s, ", scope->rpo[i]->node->payload.lam.name);
    // }
    // debug_print("\n");
}

CFNode* least_common_ancestor(CFNode* i, CFNode* j) {
    assert(i && j);
    while (i->rpo_index != j->rpo_index) {
        while (i->rpo_index < j->rpo_index) j = j->idom;
        while (i->rpo_index > j->rpo_index) i = i->idom;
    }
    return i;
}

void compute_domtree(Scope* scope) {
    for (size_t i = 0; i < scope->size; i++) {
        CFNode* n = read_list(CFNode*, scope->contents)[i];
        if (n == scope->entry)
            continue;
        for (size_t j = 0; j < entries_count_list(n->pred_edges); j++) {
            CFEdge e = read_list(CFEdge, n->pred_edges)[j];
            CFNode* p = e.src;
            if (p->rpo_index < n->rpo_index) {
                n->idom = p;
                goto outer_loop;
            }
        }
        error("no idom found");
        outer_loop:;
    }

    bool todo = true;
    while (todo) {
        todo = false;
        for (size_t i = 0; i < scope->size; i++) {
            CFNode* n = read_list(CFNode*, scope->contents)[i];
            if (n == scope->entry)
                continue;
            CFNode* new_idom = NULL;
            for (size_t j = 0; j < entries_count_list(n->pred_edges); j++) {
                CFEdge e = read_list(CFEdge, n->pred_edges)[j];
                CFNode* p = e.src;
                new_idom = new_idom ? least_common_ancestor(new_idom, p) : p;
            }
            assert(new_idom);
            if (n->idom != new_idom) {
                n->idom = new_idom;
                todo = true;
            }
        }
    }

    for (size_t i = 0; i < scope->size; i++) {
        CFNode* n = read_list(CFNode*, scope->contents)[i];
        n->dominates = new_list(CFNode*);
    }
    for (size_t i = 0; i < scope->size; i++) {
        CFNode* n = read_list(CFNode*, scope->contents)[i];
        if (n == scope->entry)
            continue;
        append_list(CFNode*, n->idom->dominates, n);
    }
}

/**
 * @param node: Start node.
 * @param target: List to extend. @ref List of @ref CFNode*
 */
static void get_undominated_children(const CFNode* node, struct List* target) {
    for (size_t i = 0; i < entries_count_list(node->succ_edges); i++) {
        CFEdge edge = read_list(CFEdge, node->succ_edges)[i];

        bool contained = false;
        for (size_t j = 0; j < entries_count_list(node->dominates); j++) {
            CFNode* dominated = read_list(CFNode*, node->dominates)[j];
            if (edge.dst == dominated) {
                contained = true;
                break;
            }
        }
        if (!contained)
            append_list(CFNode*, target, edge.dst);
    }
}

//TODO: this function can produce duplicates.
struct List* scope_get_dom_frontier(Scope* scope, const CFNode* node) {
    struct List* dom_frontier = new_list(CFNode*);

    get_undominated_children(node, dom_frontier);
    for (size_t i = 0; i < entries_count_list(node->dominates); i++) {
        CFNode* dom = read_list(CFNode*, node->dominates)[i];
        get_undominated_children(dom, dom_frontier);
    }

    return dom_frontier;
}

static int extra_uniqueness = 0;

static CFNode* get_let_pred(const CFNode* n) {
    if (entries_count_list(n->pred_edges) == 1) {
        CFEdge pred = read_list(CFEdge, n->pred_edges)[0];
        assert(pred.dst == n);
        if (pred.type == LetTailEdge && entries_count_list(pred.src->succ_edges) == 1) {
            assert(is_anonymous_lambda(n->node));
            return pred.src;
        }
    }
    return NULL;
}

static void dump_cf_node(FILE* output, const CFNode* n) {
    const Node* bb = n->node;
    const Node* body = get_abstraction_body(bb);
    if (!body)
        return;
    if (get_let_pred(n))
        return;

    String color = "black";
    if (is_anonymous_lambda(bb))
        color = "green";
    else if (is_basic_block(bb))
        color = "blue";

    String label = "";

    const CFNode* let_chain_end = n;
    while (body->tag == Let_TAG) {
        const Node* instr = body->payload.let.instruction;
        // label = "";
        if (instr->tag == PrimOp_TAG)
            label = format_string(bb->arena, "%slet ... = %s (...)\n", label, primop_names[instr->payload.prim_op.op]);
        else
            label = format_string(bb->arena, "%slet ... = %s (...)\n", label, node_tags[instr->tag]);

        if (entries_count_list(let_chain_end->succ_edges) != 1 || read_list(CFEdge, let_chain_end->succ_edges)[0].type != LetTailEdge)
            break;

        let_chain_end = read_list(CFEdge, let_chain_end->succ_edges)[0].dst;
        const Node* abs = body->payload.let.tail;
        assert(let_chain_end->node == abs);
        assert(is_anonymous_lambda(abs));
        body = get_abstraction_body(abs);
    }

    label = format_string(bb->arena, "%s%s", label, node_tags[body->tag]);

    if (is_basic_block(bb)) {
        label = format_string(bb->arena, "%s\n%s", get_abstraction_name(bb), label);
    }

    fprintf(output, "bb_%zu [label=\"%s\", color=\"%s\", shape=box];\n", (size_t) n, label, color);

    for (size_t i = 0; i < entries_count_list(n->dominates); i++) {
        CFNode* d = read_list(CFNode*, n->dominates)[i];
        if (!find_key_dict(const Node*, n->structurally_dominated, d->node))
            dump_cf_node(output, d);
    }
}

static void dump_cfg_scope(FILE* output, Scope* scope) {
    extra_uniqueness++;

    const Node* entry = scope->entry->node;
    fprintf(output, "subgraph cluster_%s {\n", get_abstraction_name(entry));
    fprintf(output, "label = \"%s\";\n", get_abstraction_name(entry));
    for (size_t i = 0; i < entries_count_list(scope->contents); i++) {
        const CFNode* n = read_list(const CFNode*, scope->contents)[i];
        dump_cf_node(output, n);
    }
    for (size_t i = 0; i < entries_count_list(scope->contents); i++) {
        const CFNode* bb_node = read_list(const CFNode*, scope->contents)[i];
        const CFNode* src_node = bb_node;
        while (true) {
            const CFNode* let_parent = get_let_pred(src_node);
            if (let_parent)
                src_node = let_parent;
            else
                break;
        }

        for (size_t j = 0; j < entries_count_list(bb_node->succ_edges); j++) {
            CFEdge edge = read_list(CFEdge, bb_node->succ_edges)[j];
            const CFNode* target_node = edge.dst;

            if (edge.type == LetTailEdge && get_let_pred(target_node) == bb_node)
                continue;

            String edge_color = "black";
            switch (edge.type) {
                case LetTailEdge:     edge_color = "green"; break;
                case ControlBodyEdge: edge_color = "red"; break;
                case IfBodyEdge:
                case MatchBodyEdge:   edge_color = "orange"; break;
                case BlockBodyEdge:   edge_color = "darkred"; break;
                default: break;
            }

            fprintf(output, "bb_%zu -> bb_%zu [color=\"%s\"];\n", (size_t) (src_node), (size_t) (target_node), edge_color);
        }
    }
    fprintf(output, "}\n");
}

void dump_cfg(FILE* output, Module* mod) {
    if (output == NULL)
        output = stderr;

    fprintf(output, "digraph G {\n");
    struct List* scopes = build_scopes(mod);
    for (size_t i = 0; i < entries_count_list(scopes); i++) {
        Scope* scope = read_list(Scope*, scopes)[i];
        dump_cfg_scope(output, scope);
        destroy_scope(scope);
    }
    destroy_list(scopes);
    fprintf(output, "}\n");
}

void dump_cfg_auto(Module* mod) {
    FILE* f = fopen("cfg.dot", "wb");
    dump_cfg(f, mod);
    fclose(f);
}
