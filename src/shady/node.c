#include "type.h"
#include "log.h"
#include "arena.h"
#include "fold.h"
#include "portability.h"

#include "murmur3.h"
#include "dict.h"

#include <string.h>
#include <assert.h>

static Node* create_node_helper(IrArena* arena, Node node) {
    Node* ptr = &node;
    Node** found = find_key_dict(Node*, arena->node_set, ptr);
    // sanity check nominal nodes to be unique, check for duplicates in structural nodes
    if (is_nominal(node.tag))
        assert(!found);
    else if (found)
        return *found;

    if (arena->config.allow_fold) {
        Node* folded = (Node*) fold_node(arena, ptr);
        if (folded != ptr) {
            // The folding process simplified the node, we store a mapping to that simplified node and bail out !
            insert_set_get_result(Node*, arena->node_set, folded);
            return folded;
        }
    }

    // place the node in the arena and return it
    Node* alloc = (Node*) arena_alloc(arena, sizeof(Node));
    *alloc = node;
    insert_set_get_result(const Node*, arena->node_set, alloc);

    return alloc;
}

#define LAST_ARG_1(struct_name) ,struct_name in_node
#define LAST_ARG_0(struct_name)

#define CALL_TYPING_METHOD_11(short_name) arena->config.check_types ? check_type_##short_name(arena, in_node) : NULL
#define CALL_TYPING_METHOD_01(short_name) NULL
#define CALL_TYPING_METHOD_10(short_name) arena->config.check_types ? check_type_##short_name(arena) : NULL
#define CALL_TYPING_METHOD_00(short_name) NULL

#define SET_PAYLOAD_1(short_name) .payload = (union NodesUnion) { .short_name = in_node }
#define SET_PAYLOAD_0(_)

#define NODE_CTOR_1(has_typing_fn, has_payload, struct_name, short_name) const Node* short_name(IrArena* arena LAST_ARG_##has_payload(struct_name)) { \
    Node node;                                                                                                                                        \
    memset((void*) &node, 0, sizeof(Node));                                                                                                           \
    node = (Node) {                                                                                                                                   \
        .arena = arena,                                                                                                                                 \
        .type = CALL_TYPING_METHOD_##has_typing_fn##has_payload(short_name),                                                                            \
        .tag = struct_name##_TAG,                                                                                                                       \
        SET_PAYLOAD_##has_payload(short_name)                                                                                                           \
    };                                                                                                                                                \
    return create_node_helper(arena, node);                                                                                                           \
}

#define NODE_CTOR_0(has_typing_fn, has_payload, struct_name, short_name)
#define NODE_CTOR(autogen_ctor, has_typing_fn, has_payload, struct_name, short_name) NODE_CTOR_##autogen_ctor(has_typing_fn, has_payload, struct_name, short_name)
NODES(NODE_CTOR)
#undef NODE_CTOR

TypeTag is_type(const Node* node) {
    switch (node->tag) {
#define IS_TYPE(_, _2, _3, name, _4) case name##_TAG: return Type_##name##_TAG;
TYPE_NODES(IS_TYPE)
#undef IS_TYPE
        default: return NotAType;
    }
}

ValueTag is_value(const Node* node) {
    switch (node->tag) {
#define IS_VALUE(_, _2, _3, name, _4) case name##_TAG: return Value_##name##_TAG;
        VALUE_NODES(IS_VALUE)
#undef IS_VALUE
        default: return NotAValue;
    }
}

InstructionTag is_instruction(const Node* node) {
    switch (node->tag) {
#define IS_INSTRUCTION(_, _2, _3, name, _4) case name##_TAG: return Instruction_##name##_TAG;
        INSTRUCTION_NODES(IS_INSTRUCTION)
#undef IS_INSTRUCTION
        default: return NotAnInstruction;
    }
}

TerminatorTag is_terminator(const Node* node) {
    switch (node->tag) {
#define IS_TERMINATOR(_, _2, _3, name, _4) case name##_TAG: return Terminator_##name##_TAG;
        TERMINATOR_NODES(IS_TERMINATOR)
#undef IS_TERMINATOR
        default: return NotATerminator;
    }
}

const Node* var(IrArena* arena, const Type* type, const char* name) {
    Variable variable = {
        .type = type,
        .name = string(arena, name),
        .id = fresh_id(arena)
    };

    Node node;
    memset((void*) &node, 0, sizeof(Node));
    node = (Node) {
        .arena = arena,
        .type = arena->config.check_types ? check_type_var(arena, variable) : NULL,
        .tag = Variable_TAG,
        .payload.var = variable
    };
    return create_node_helper(arena, node);
}

static const Node* let_internal(IrArena* arena, bool is_mutable, Nodes* provided_types, const Node* instruction, size_t outputs_count, const char* output_names[]) {
    assert(outputs_count > 0 && "do not use let if the outputs count isn't zero !");
    LARRAY(Node*, vars, outputs_count);

    if (provided_types)
        assert(provided_types->count == outputs_count);

    if (arena->config.check_types) {
        Nodes types = unwrap_multiple_yield_types(arena, instruction->type);
        assert(types.count == outputs_count);
        if (provided_types) {
            // Check that the types we got are subtypes of what we care about
            for (size_t i = 0; i < outputs_count; i++)
                assert(is_subtype(provided_types->nodes[i], types.nodes[i]));
            types = *provided_types;
        }

        for (size_t i = 0; i < outputs_count; i++)
            vars[i] = (Node*) var(arena, types.nodes[i], output_names ? output_names[i] : node_tags[instruction->tag]);
    } else {
        for (size_t i = 0; i < outputs_count; i++)
            vars[i] = (Node*) var(arena, provided_types ? provided_types->nodes[i] : NULL, output_names ? output_names[i] : node_tags[instruction->tag]);
    }

    for (size_t i = 0; i < outputs_count; i++) {
        vars[i]->payload.var.instruction = instruction;
        vars[i]->payload.var.output = i;
    }

    Let payload = {
        .instruction = instruction,
        .variables = nodes(arena, outputs_count, (const Node**) vars),
        .is_mutable = is_mutable
    };

    Node node;
    memset((void*) &node, 0, sizeof(Node));
    node = (Node) {
        .arena = arena,
        .type = arena->config.check_types ? check_type_let(arena, payload) : NULL,
        .tag = Let_TAG,
        .payload.let = payload
    };
    return create_node_helper(arena, node);
}

const Node* let(IrArena* arena, const Node* instruction, size_t outputs_count, const char* output_names[]) {
    return let_internal(arena, false, NULL, instruction, outputs_count, output_names);
}

const Node* let_mut(IrArena* arena, const Node* instruction, Nodes types, size_t outputs_count, const char* output_names[]) {
    return let_internal(arena, true, &types, instruction, outputs_count, output_names);
}

const Node* tuple(IrArena* arena, Nodes contents) {
    Tuple t = {
        .contents = contents
    };

    Node node;
    memset((void*) &node, 0, sizeof(Node));
    node = (Node) {
        .arena = arena,
        .type = arena->config.check_types ? check_type_tuple(arena, t) : NULL,
        .tag = Tuple_TAG,
        .payload.tuple = t
    };
    return create_node_helper(arena, node);
}

Node* fn(IrArena* arena, Nodes annotations, const char* name, bool is_basic_block, Nodes params, Nodes return_types) {
    Function fn = {
        .annotations = annotations,
        .name = string(arena, name),
        .is_basic_block = is_basic_block,
        .params = params,
        .return_types = return_types,
        .block = NULL,
    };

    Node node;
    memset((void*) &node, 0, sizeof(Node));
    node = (Node) {
        .arena = arena,
        .type = arena->config.check_types ? check_type_fn(arena, fn) : NULL,
        .tag = Function_TAG,
        .payload.fn = fn
    };
    return create_node_helper(arena, node);
}

Node* constant(IrArena* arena, Nodes annotations, String name) {
    Constant cnst = {
        .annotations = annotations,
        .name = string(arena, name),
        .value = NULL,
        .type_hint = NULL,
    };

    Node node;
    memset((void*) &node, 0, sizeof(Node));
    node = (Node) {
        .arena = arena,
        .type = NULL,
        .tag = Constant_TAG,
        .payload.constant = cnst
    };
    return create_node_helper(arena, node);
}

Node* global_var(IrArena* arena, Nodes annotations, const Type* type, const char* name, AddressSpace as) {
    GlobalVariable gvar = {
        .annotations = annotations,
        .name = string(arena, name),
        .type = type,
        .address_space = as,
        .init = NULL,
    };

    Node node;
    memset((void*) &node, 0, sizeof(Node));
    node = (Node) {
        .arena = arena,
        .type = arena->config.check_types ? check_type_global_variable(arena, gvar) : NULL,
        .tag = GlobalVariable_TAG,
        .payload.global_variable = gvar
    };
    return create_node_helper(arena, node);
}

const char* node_tags[] = {
#define NODE_NAME(_, _2, _3, _4, str) #str,
NODES(NODE_NAME)
#undef NODE_NAME
};

const char* primop_names[] = {
#define PRIMOP(se, str) #str,
PRIMOPS()
#undef PRIMOP
};

const bool primop_side_effects[] = {
#define PRIMOP(se, str) se,
PRIMOPS()
#undef PRIMOP
};

bool has_primop_got_side_effects(Op op) {
    return primop_side_effects[op];
}

const bool node_type_has_payload[] = {
#define NODE_HAS_PAYLOAD(_, _2, has_payload, _4, _5) has_payload,
NODES(NODE_HAS_PAYLOAD)
#undef NODE_HAS_PAYLOAD
};

String merge_what_string[] = { "join", "continue", "break" };

KeyHash hash_murmur(const void* data, size_t size) {
    int32_t out[4];
    MurmurHash3_x64_128(data, (int) size, 0x1234567, &out);

    uint32_t final = 0;
    final ^= out[0];
    final ^= out[1];
    final ^= out[2];
    final ^= out[3];
    return final;
}

#define FIELDS                        \
case Variable_TAG: {                  \
    field(var.id);                    \
    break;                            \
}                                     \
case IntLiteral_TAG: {                \
    field(int_literal.width);         \
    field(int_literal.value_i64);     \
    break;                            \
}                                     \
case Let_TAG: {                       \
    field(let.variables);             \
    field(let.instruction);           \
    break;                            \
}                                     \
case QualifiedType_TAG: {             \
    field(qualified_type.type);       \
    field(qualified_type.is_uniform); \
    break;                            \
}                                     \
case PackType_TAG: {                  \
    field(pack_type.element_type);    \
    field(pack_type.width);           \
    break;                            \
}                                     \
case RecordType_TAG: {                \
    field(record_type.members);       \
    field(record_type.names);         \
    field(record_type.special);       \
    break;                            \
}                                     \
case FnType_TAG: {                    \
    field(fn_type.is_basic_block);    \
    field(fn_type.return_types);      \
    field(fn_type.param_types);       \
    break;                            \
}                                     \
case PtrType_TAG: {                   \
    field(ptr_type.address_space);    \
    field(ptr_type.pointed_type);     \
    break;                            \
}                                     \

KeyHash hash_node(Node** pnode) {
    const Node* node = *pnode;
    KeyHash combined;

    if (is_nominal(node->tag)) {
        size_t ptr = (size_t) node;
        uint32_t upper = ptr >> 32;
        uint32_t lower = ptr;
        combined = upper ^ lower;
        goto end;
    }

    KeyHash tag_hash = hash_murmur(&node->tag, sizeof(NodeTag));
    KeyHash payload_hash = 0;

    #define field(d) payload_hash ^= hash_murmur(&node->payload.d, sizeof(node->payload.d));

    if (node_type_has_payload[node->tag]) {
        switch (node->tag) {
            FIELDS
            default: payload_hash = hash_murmur(&node->payload, sizeof(node->payload)); break;
        }
    }
    combined = tag_hash ^ payload_hash;

    end:
    // debug_print("hash of :");
    // debug_node(node);
    // debug_print(" = [%u] %u\n", combined, combined % 32);
    return combined;
}

bool compare_node(Node** pa, Node** pb) {
    if ((*pa)->tag != (*pb)->tag) return false;
    if (is_nominal((*pa)->tag)) {
        // debug_node(*pa);
        // debug_print(" vs ");
        // debug_node(*pb);
        // debug_print(" ptrs: %lu vs %lu %d\n", *pa, *pb, *pa == *pb);
        return *pa == *pb;
    }

    const Node* a = *pa;
    const Node* b = *pb;

    #undef field
    #define field(w) eq &= memcmp(&a->payload.w, &b->payload.w, sizeof(a->payload.w)) == 0;

    if (node_type_has_payload[a->tag]) {
        bool eq = true;
        switch ((*pa)->tag) {
            FIELDS
            default: return memcmp(&a->payload, &b->payload, sizeof(a->payload)) == 0;
        }
        return eq;
    } else return true;
}

String get_decl_name(const Node* node) {
    switch (node->tag) {
        case Constant_TAG: return node->payload.constant.name;
        case Function_TAG: return node->payload.fn.name;
        case GlobalVariable_TAG: return node->payload.global_variable.name;
        default: error("Not a decl !");
    }
}

int64_t extract_int_literal_value(const Node* node, bool sign_extend) {
    assert(node->tag == IntLiteral_TAG);
    if (sign_extend) {
        switch (node->payload.int_literal.width) {
            case IntTy8:  return (int64_t) node->payload.int_literal.value_i8;
            case IntTy16: return (int64_t) node->payload.int_literal.value_i16;
            case IntTy32: return (int64_t) node->payload.int_literal.value_i32;
            case IntTy64: return           node->payload.int_literal.value_i64;
            default: assert(false);
        }
    } else {
        switch (node->payload.int_literal.width) {
            case IntTy8:  return (int64_t) ((uint64_t) (node->payload.int_literal.value_u8 ));
            case IntTy16: return (int64_t) ((uint64_t) (node->payload.int_literal.value_u16));
            case IntTy32: return (int64_t) ((uint64_t) (node->payload.int_literal.value_u32));
            case IntTy64: return                        node->payload.int_literal.value_i64  ;
            default: assert(false);
        }
    }
}

const IntLiteral* resolve_to_literal(const Node* node) {
    while (true) {
        switch (node->tag) {
            case Constant_TAG: return resolve_to_literal(node->payload.constant.value);
            case IntLiteral_TAG: {
                return &node->payload.int_literal;
            }
            default: return NULL;
        }
    }
}

const char* extract_string_literal(const Node* node) {
    assert(node->tag == StringLiteral_TAG);
    return node->payload.string_lit.string;
}

const Type* int8_type(IrArena* arena) { return int_type(arena, (Int) { .width = IntTy8 }); }
const Type* int16_type(IrArena* arena) { return int_type(arena, (Int) { .width = IntTy16 }); }
const Type* int32_type(IrArena* arena) { return int_type(arena, (Int) { .width = IntTy32 }); }
const Type* int64_type(IrArena* arena) { return int_type(arena, (Int) { .width = IntTy64 }); }

const Type* int8_literal(IrArena* arena, int8_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy8, .value_i8 = i }); }
const Type* int16_literal(IrArena* arena, int16_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy16, .value_i16 = i }); }
const Type* int32_literal(IrArena* arena, int32_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy32, .value_i32 = i }); }
const Type* int64_literal(IrArena* arena, int64_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy64, .value_i64 = i }); }

const Type* uint8_literal(IrArena* arena, uint8_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy8, .value_u8 = i }); }
const Type* uint16_literal(IrArena* arena, uint16_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy16, .value_u16 = i }); }
const Type* uint32_literal(IrArena* arena, uint32_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy32, .value_u32 = i }); }
const Type* uint64_literal(IrArena* arena, uint64_t i) { return int_literal(arena, (IntLiteral) { .width = IntTy64, .value_u64 = i }); }
