#include "memory_layout.h"
#include "ir_gen_helpers.h"

#include "log.h"
#include "../body_builder.h"

#include <assert.h>

TypeMemLayout get_mem_layout(const CompilerConfig* config, IrArena* arena, const Type* type) {
    switch (type->tag) {
        case FnType_TAG:  error("Functions have an opaque memory representation");
        case PtrType_TAG: switch (type->payload.ptr_type.address_space) {
            case AsProgramCode: return get_mem_layout(config, arena, int32_type(arena));
            default: error("unhandled")
        }
        // case MaskType_TAG: return get_mem_layout(config, arena, int_type(arena));
        case Int_TAG:     return (TypeMemLayout) {
            .type = type,
            .size_in_bytes = type->payload.int_type.width == IntTy64 ? 8 : 4,
        };
        case Float_TAG:   return (TypeMemLayout) {
            .type = type,
            .size_in_bytes = 4,
        };
        case Bool_TAG:   return (TypeMemLayout) {
            .type = type,
            .size_in_bytes = 4,
        };
        case ArrType_TAG: {
            const Node* size = type->payload.arr_type.size;
            assert(size && "We can't know the full layout of arrays of unknown size !");
            size_t actual_size = extract_int_literal_value(size, false);
            TypeMemLayout element_layout = get_mem_layout(config, arena, type->payload.arr_type.element_type);
            return (TypeMemLayout) {
                .type = type,
                .size_in_bytes = actual_size * element_layout.size_in_bytes
            };
        }
        case QualifiedType_TAG: return get_mem_layout(config, arena, type->payload.qualified_type.type);
        case RecordType_TAG: error("TODO");
        default: error("not a known type");
    }
}

const Node* gen_deserialisation(BodyBuilder* bb, const Type* element_type, const Node* arr, const Node* base_offset) {
    switch (element_type->tag) {
        case Bool_TAG: {
            const Node* logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, base_offset });
            const Node* value = gen_load(bb, logical_ptr);
            const Node* zero = int32_literal(bb->arena, 0);
            return gen_primop_ce(bb, neq_op, 2, (const Node*[]) {value, zero});
        }
        case PtrType_TAG: switch (element_type->payload.ptr_type.address_space) {
            case AsProgramCode: goto ser_int;
            default: error("TODO")
        }
        case Int_TAG: ser_int: {
            if (element_type->payload.int_type.width != IntTy64) {
                // One load suffices
                const Node* logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, base_offset });
                const Node* value = gen_load(bb, logical_ptr);
                // cast into the appropriate width and throw other bits away
                // note: folding gets rid of identity casts
                value = gen_primop_ce(bb, reinterpret_op, 2, (const Node* []){ element_type, value});
                return value;
            } else {
                // We need to decompose this into two loads, then we use the merge routine
                const Node* logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, base_offset });
                const Node* lo = gen_load(bb, logical_ptr);
                const Node* hi_destination_offset = gen_primop_ce(bb, add_op, 2, (const Node* []) { base_offset, int32_literal(bb->arena, 1) });
                            logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, hi_destination_offset });
                const Node* hi = gen_load(bb, logical_ptr);
                return gen_merge_i32s_i64(bb, lo, hi);
            }
        }
        default: error("TODO");
    }
}

void gen_serialisation(BodyBuilder* bb, const Type* element_type, const Node* arr, const Node* base_offset, const Node* value) {
    switch (element_type->tag) {
        case Bool_TAG: {
            const Node* logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, base_offset });
            const Node* zero = int32_literal(bb->arena, 0);
            const Node* one = int32_literal(bb->arena, 1);
            const Node* int_value = gen_primop_ce(bb, select_op, 3, (const Node*[]) { value, one, zero });
            gen_store(bb, logical_ptr, int_value);
            return;
        }
        case PtrType_TAG: switch (element_type->payload.ptr_type.address_space) {
            case AsProgramCode: goto des_int;
            default: error("TODO")
        }
        case Int_TAG: des_int: {
            // Same story as for deser
            if (element_type->payload.int_type.width != IntTy64) {
                value = gen_primop_ce(bb, reinterpret_op, 2, (const Node* []){ int32_type(bb->arena), value });
                const Node* logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, base_offset});
                gen_store(bb, logical_ptr, value);
            } else {
                const Node* lo = gen_primop_ce(bb, reinterpret_op, 2, (const Node* []){ int32_type(bb->arena), value });
                const Node* hi = gen_primop_ce(bb, lshift_op, 2, (const Node* []){ value, int64_literal(bb->arena, 32) });
                hi = gen_primop_ce(bb, reinterpret_op, 2, (const Node* []){ int32_type(bb->arena), hi });
                // TODO: make this dependant on the emulation array type
                const Node* logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, base_offset});
                gen_store(bb, logical_ptr, lo);
                const Node* hi_destination_offset = gen_primop_ce(bb, add_op, 2, (const Node* []) { base_offset, int32_literal(bb->arena, 1) });
                            logical_ptr = gen_primop_ce(bb, lea_op, 3, (const Node* []) { arr, NULL, hi_destination_offset});
                gen_store(bb, logical_ptr, hi);
            }
            return;
        }
        default: error("TODO");
    }
}
