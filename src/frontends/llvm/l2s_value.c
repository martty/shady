#include "l2s_private.h"

#include "portability.h"
#include "log.h"
#include "dict.h"
#include "../../shady/transform/ir_gen_helpers.h"
#include "../../shady/type.h"

static const Node* data_composite(const Type* t, size_t size, LLVMValueRef v) {
    IrArena* a = t->arena;
    LARRAY(const Node*, elements, size);
    size_t idc;
    const char* raw_bytes = LLVMGetAsString(v, &idc);
    for (size_t i = 0; i < size; i++) {
        const Type* et = get_fill_type_element_type(t);
        switch (et->tag) {
            case Int_TAG: {
                switch (et->payload.int_type.width) {
                    case IntTy8:  elements[i] =  uint8_literal(a, ((uint8_t*) raw_bytes)[i]); break;
                    case IntTy16: elements[i] = uint16_literal(a, ((uint16_t*) raw_bytes)[i]); break;
                    case IntTy32: elements[i] = uint32_literal(a, ((uint32_t*) raw_bytes)[i]); break;
                    case IntTy64: elements[i] = uint64_literal(a, ((uint64_t*) raw_bytes)[i]); break;
                }
                break;
            }
            case Float_TAG: {
                switch (et->payload.float_type.width) {
                    case FloatTy16:
                        elements[i] = float_literal(a, (FloatLiteral) { .width = et->payload.float_type.width, .value = ((uint16_t*) raw_bytes)[i] });
                        break;
                    case FloatTy32:
                        elements[i] = float_literal(a, (FloatLiteral) { .width = et->payload.float_type.width, .value = ((uint32_t*) raw_bytes)[i] });
                        break;
                    case FloatTy64:
                        elements[i] = float_literal(a, (FloatLiteral) { .width = et->payload.float_type.width, .value = ((uint64_t*) raw_bytes)[i] });
                        break;
                }
                break;
            }
            default: assert(false);
        }
    }
    return composite_helper(a, t, nodes(a, size, elements));
}

const Node* convert_value(Parser* p, LLVMValueRef v) {
    const Type** found = find_value_dict(LLVMTypeRef, const Type*, p->map, v);
    if (found) return *found;
    IrArena* a = get_module_arena(p->dst);

    const Node* r = NULL;
    const Type* t = LLVMGetValueKind(v) != LLVMMetadataAsValueValueKind ? convert_type(p, LLVMTypeOf(v)) : NULL;

    switch (LLVMGetValueKind(v)) {
        case LLVMArgumentValueKind:
            break;
        case LLVMBasicBlockValueKind:
            break;
        case LLVMMemoryUseValueKind:
            break;
        case LLVMMemoryDefValueKind:
            break;
        case LLVMMemoryPhiValueKind:
            break;
        case LLVMFunctionValueKind:
            r = convert_function(p, v);
            break;
        case LLVMGlobalAliasValueKind:
            break;
        case LLVMGlobalIFuncValueKind:
            break;
        case LLVMGlobalVariableValueKind:
            r = convert_global(p, v);
            break;
        case LLVMBlockAddressValueKind:
            break;
        case LLVMConstantExprValueKind: {
            String name = LLVMGetValueName(v);
            if (!name || strlen(name) == 0)
                name = unique_name(a, "constant_expr");
            Nodes annotations = singleton(annotation(a, (Annotation) { .name = "SkipOnInfer" }));
            annotations = empty(a);
            Node* decl = constant(p->dst, annotations, NULL, name);
            r = ref_decl_helper(a, decl);
            insert_dict(LLVMTypeRef, const Type*, p->map, v, r);
            BodyBuilder* bb = begin_body(a);
            EmittedInstr emitted = convert_instruction(p, NULL, bb, v);
            Nodes types = singleton(convert_type(p, LLVMTypeOf(v)));
            decl->payload.constant.instruction = bind_last_instruction_and_wrap_in_block_explicit_return_types(bb, emitted.instruction, &types);
            return r;
        }
        case LLVMConstantDataArrayValueKind: {
            assert(t->tag == ArrType_TAG);
            size_t arr_size = get_int_literal_value(*resolve_to_int_literal(t->payload.arr_type.size), false);
            assert(arr_size >= 0 && arr_size < INT32_MAX && "sanity check");
            return data_composite(t, arr_size, v);
        }
        case LLVMConstantDataVectorValueKind: {
            assert(t->tag == PackType_TAG);
            size_t width = t->payload.pack_type.width;
            assert(width >= 0 && width < INT32_MAX && "sanity check");
            return data_composite(t, width, v);
        }
        case LLVMConstantStructValueKind: {
            assert(t->tag == RecordType_TAG);
            size_t size = t->payload.record_type.members.count;
            LARRAY(const Node*, elements, size);
            for (size_t i = 0; i < size; i++) {
                LLVMValueRef value = LLVMGetOperand(v, i);
                assert(value);
                elements[i] = convert_value(p, value);
            }
            return composite_helper(a, t, nodes(a, size, elements));
        }
        case LLVMConstantVectorValueKind: {
            assert(t->tag == PackType_TAG);
            size_t size = t->payload.pack_type.width;
            LARRAY(const Node*, elements, size);
            for (size_t i = 0; i < size; i++) {
                LLVMValueRef value = LLVMGetOperand(v, i);
                assert(value);
                elements[i] = convert_value(p, value);
            }
            return composite_helper(a, t, nodes(a, size, elements));
        }
        case LLVMUndefValueValueKind:
            return undef(a, (Undef) { .type = convert_type(p, LLVMTypeOf(v)) });
        case LLVMConstantAggregateZeroValueKind:
            return get_default_zero_value(a, convert_type(p, LLVMTypeOf(v)));
        case LLVMConstantArrayValueKind: {
            assert(t->tag == ArrType_TAG);
            if (LLVMIsConstantString(v)) {
                size_t idc;
                r = string_lit_helper(a, LLVMGetAsString(v, &idc));
                break;
            }
            size_t arr_size = get_int_literal_value(*resolve_to_int_literal(t->payload.arr_type.size), false);
            assert(arr_size >= 0 && arr_size < INT32_MAX && "sanity check");
            LARRAY(const Node*, elements, arr_size);
            for (size_t i = 0; i < arr_size; i++) {
                LLVMValueRef value = LLVMGetOperand(v, i);
                assert(value);
                elements[i] = convert_value(p, value);
            }
            return composite_helper(a, t, nodes(a, arr_size, elements));
        }
        case LLVMConstantIntValueKind: {
            if (t->tag == Bool_TAG) {
                unsigned long long value = LLVMConstIntGetZExtValue(v);
                return value ? true_lit(a) : false_lit(a);
            }
            assert(t->tag == Int_TAG);
            unsigned long long value = LLVMConstIntGetZExtValue(v);
            switch (t->payload.int_type.width) {
                case IntTy8: return uint8_literal(a, value);
                case IntTy16: return uint16_literal(a, value);
                case IntTy32: return uint32_literal(a, value);
                case IntTy64: return uint64_literal(a, value);
            }
        }
        case LLVMConstantFPValueKind: {
            assert(t->tag == Float_TAG);
            LLVMBool lossy;
            double d = LLVMConstRealGetDouble(v, &lossy);
            uint64_t u = 0;
            static_assert(sizeof(u) == sizeof(d), "");
            switch (t->payload.float_type.width) {
                case FloatTy16: error("todo")
                case FloatTy32: {
                    float f = (float) d;
                    static_assert(sizeof(f) == sizeof(uint32_t), "");
                    memcpy(&u, &f, sizeof(f));
                    return float_literal(a, (FloatLiteral) { .width = t->payload.float_type.width, .value = u });
                }
                case FloatTy64: {
                    memcpy(&u, &d, sizeof(double));
                    return float_literal(a, (FloatLiteral) { .width = t->payload.float_type.width, .value = u });
                }
            }
        }
        case LLVMConstantPointerNullValueKind:
            r = null_ptr(a, (NullPtr) { .ptr_type = t });
            break;
        case LLVMConstantTokenNoneValueKind:
            break;
        case LLVMMetadataAsValueValueKind: {
            LLVMMetadataRef meta = LLVMValueAsMetadata(v);
            r = convert_metadata(p, meta);
        }
        case LLVMInlineAsmValueKind:
            break;
        case LLVMInstructionValueKind:
            break;
        case LLVMPoisonValueValueKind:
            return undef(a, (Undef) { .type = convert_type(p, LLVMTypeOf(v)) });
    }

    if (r) {
        insert_dict(LLVMTypeRef, const Type*, p->map, v, r);
        return r;
    }

    error_print("Failed to find value ");
    LLVMDumpValue(v);
    error_print(" in the already emitted map (kind=%d)\n", LLVMGetValueKind(v));
    error_die();
}
