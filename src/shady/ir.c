#include "ir_private.h"
#include "type.h"
#include "portability.h"

#include "list.h"
#include "dict.h"
#include "murmur3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>

static KeyHash hash_nodes(Nodes* nodes);
static bool compare_nodes(Nodes* a, Nodes* b);

static KeyHash hash_strings(Strings* strings);
static bool compare_strings(Strings* a, Strings* b);

static KeyHash hash_string(const char** string);
static bool compare_string(const char** a, const char** b);

KeyHash hash_node(const Node**);
bool compare_node(const Node** a, const Node** b);

IrArena* new_ir_arena(ArenaConfig config) {
    IrArena* arena = malloc(sizeof(IrArena));
    *arena = (IrArena) {
        .arena = new_arena(),
        .config = config,

        .next_free_id = 0,

        .node_set = new_set(const Node*, (HashFn) hash_node, (CmpFn) compare_node),
        .string_set = new_set(const char*, (HashFn) hash_string, (CmpFn) compare_string),

        .nodes_set   = new_set(Nodes, (HashFn) hash_nodes, (CmpFn) compare_nodes),
        .strings_set = new_set(Strings, (HashFn) hash_strings, (CmpFn) compare_strings),
    };
    return arena;
}

void destroy_ir_arena(IrArena* arena) {
    destroy_dict(arena->strings_set);
    destroy_dict(arena->string_set);
    destroy_dict(arena->nodes_set);
    destroy_dict(arena->node_set);
    destroy_arena(arena->arena);
    free(arena);
}

VarId fresh_id(IrArena* arena) {
    return arena->next_free_id++;
}

inline static size_t round_up(size_t a, size_t b) {
    size_t divided = (a + b - 1) / b;
    return divided * b;
}

Nodes nodes(IrArena* arena, size_t count, const Node* in_nodes[]) {
    Nodes tmp = {
        .count = count,
        .nodes = in_nodes
    };
    const Nodes* found = find_key_dict(Nodes, arena->nodes_set, tmp);
    if (found)
        return *found;

    Nodes nodes;
    nodes.count = count;
    nodes.nodes = arena_alloc(arena->arena, sizeof(Node*) * count);
    for (size_t i = 0; i < count; i++)
        nodes.nodes[i] = in_nodes[i];

    insert_set_get_result(Nodes, arena->nodes_set, nodes);
    return nodes;
}

Strings strings(IrArena* arena, size_t count, const char* in_strs[])  {
    Strings tmp = {
        .count = count,
        .strings = in_strs,
    };
    const Strings* found = find_key_dict(Strings, arena->strings_set, tmp);
    if (found)
        return *found;

    Strings strings;
    strings.count = count;
    strings.strings = arena_alloc(arena->arena, sizeof(const char*) * count);
    for (size_t i = 0; i < count; i++)
        strings.strings[i] = in_strs[i];

    insert_set_get_result(Strings, arena->strings_set, strings);
    return strings;
}

Nodes append_nodes(IrArena* arena, Nodes old, const Node* new) {
    LARRAY(const Node*, tmp, old.count + 1);
    for (size_t i = 0; i < old.count; i++)
        tmp[i] = old.nodes[i];
    tmp[old.count] = new;
    return nodes(arena, old.count + 1, tmp);
}

Nodes concat_nodes(IrArena* arena, Nodes a, Nodes b) {
    LARRAY(const Node*, tmp, a.count + b.count);
    size_t j = 0;
    for (size_t i = 0; i < a.count; i++)
        tmp[j++] = a.nodes[i];
    for (size_t i = 0; i < b.count; i++)
        tmp[j++] = b.nodes[i];
    assert(j == a.count + b.count);
    return nodes(arena, j, tmp);
}

/// takes care of structural sharing
static const char* string_impl(IrArena* arena, size_t size, const char* zero_terminated) {
    const char* ptr = zero_terminated;
    const char** found = find_key_dict(const char*, arena->string_set, ptr);
    if (found)
        return *found;

    char* new_str = (char*) arena_alloc(arena->arena, strlen(zero_terminated) + 1);
    strncpy(new_str, zero_terminated, size);
    new_str[size] = '\0';

    insert_set_get_result(const char*, arena->string_set, new_str);
    return new_str;
}

const char* string_sized(IrArena* arena, size_t size, const char* str) {
    LARRAY(char, new_str, size + 1);

    strncpy(new_str, str, size);
    new_str[size] = '\0';
    assert(strlen(new_str) == size);
    return string_impl(arena, size, str);
}

const char* string(IrArena* arena, const char* str) {
    return string_impl(arena, strlen(str), str);
}

String format_string(IrArena* arena, const char* str, ...) {
    char tmp[128];
    va_list args;
    va_start(args, str);
    int len = vsnprintf(tmp, 128, str, args);
    const char* interned = string_impl(arena, len, tmp);
    va_end(args);
    return interned;
}

const char* unique_name(IrArena* arena, const char* str) {
    return format_string(arena, "%s_%d", str, fresh_id(arena));
}

KeyHash hash_nodes(Nodes* nodes) {
    uint32_t out[4];
    MurmurHash3_x64_128((nodes)->nodes, (int) (sizeof(Node*) * (nodes)->count), 0x1234567, &out);
    uint32_t final = 0;
    final ^= out[0];
    final ^= out[1];
    final ^= out[2];
    final ^= out[3];
    return final;
}

bool compare_nodes(Nodes* a, Nodes* b) {
    if (a->count != b->count) return false;
    if (a->count == 0 && b->count == 0) return true;
    assert(a->nodes != NULL && b->nodes != NULL);
    return memcmp(a->nodes, b->nodes, sizeof(Node*) * (a->count)) == 0; // actually compare the data
}

KeyHash hash_strings(Strings* strings) {
    uint32_t out[4];
    MurmurHash3_x64_128(strings->strings, (int) (sizeof(const char*) * strings->count), 0x1234567, &out);
    uint32_t final = 0;
    final ^= out[0];
    final ^= out[1];
    final ^= out[2];
    final ^= out[3];
    return final;
}

bool compare_strings(Strings* a, Strings* b) {
    return a->count == b->count && memcmp(a->strings, b->strings, sizeof(const char*) * a->count) == 0;
}

KeyHash hash_string(const char** string) {
    uint32_t out[4];
    MurmurHash3_x64_128(*string, (int) strlen(*string), 0x1234567, &out);
    uint32_t final = 0;
    final ^= out[0];
    final ^= out[1];
    final ^= out[2];
    final ^= out[3];
    return final;
}

bool compare_string(const char** a, const char** b) {
    return strlen(*a) == strlen(*b) && strcmp(*a, *b) == 0;
}

Nodes list_to_nodes(IrArena* arena, struct List* list) {
    return nodes(arena, entries_count_list(list), read_list(const Node*, list));
}