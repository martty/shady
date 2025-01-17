#include "passes.h"

#include "portability.h"
#include "log.h"

#include "../rewrite.h"
#include "../type.h"
#include "../transform/ir_gen_helpers.h"
#include "../transform/memory_layout.h"

typedef struct {
    Rewriter rewriter;
    const CompilerConfig* config;
    BodyBuilder* bb;
    Node* lifted_globals_decl;
} Context;

static const Node* process(Context* ctx, const Node* node) {
    IrArena* a = ctx->rewriter.dst_arena;

    BodyBuilder* abs_bb = NULL;
    Context c = *ctx;
    ctx = &c;
    if (is_abstraction(node)) {
        c.bb = abs_bb = begin_body(a);
    }

    switch (node->tag) {
        case RefDecl_TAG: {
            const Node* odecl = node->payload.ref_decl.decl;
            if (odecl->tag != GlobalVariable_TAG || odecl->payload.global_variable.address_space != AsGlobalPhysical)
                break;
            assert(ctx->bb && "this RefDecl node isn't appearing in an abstraction - we cannot replace it with a load!");
            const Node* ptr_addr = gen_lea(ctx->bb, ref_decl_helper(a, ctx->lifted_globals_decl), int32_literal(a, 0), singleton(rewrite_node(&ctx->rewriter, odecl)));
            const Node* ptr = gen_load(ctx->bb, ptr_addr);
            return ptr;
        }
        case GlobalVariable_TAG:
            if (node->payload.global_variable.address_space != AsGlobalPhysical)
                break;
            assert(false);
        default: break;
    }

    Node* new = (Node*) recreate_node_identity(&ctx->rewriter, node);
    if (abs_bb) {
        assert(is_abstraction(new));
        if (get_abstraction_body(new))
            set_abstraction_body(new, finish_body(abs_bb, get_abstraction_body(new)));
        else
            cancel_body(abs_bb);
    }
    return new;
}

Module* spirv_lift_globals_ssbo(SHADY_UNUSED const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = get_arena_config(get_module_arena(src));
    IrArena* a = new_ir_arena(aconfig);
    Module* dst = new_module(a, get_module_name(src));

    Context ctx = {
        .rewriter = create_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config
    };

    Nodes old_decls = get_module_declarations(src);
    LARRAY(const Type*, member_tys, old_decls.count);
    LARRAY(String, member_names, old_decls.count);

    Nodes annotations = mk_nodes(a, annotation(a, (Annotation) { .name = "Generated" }));
    annotations = empty(a);

    annotations = append_nodes(a, annotations, annotation_value(a, (AnnotationValue) { .name = "DescriptorSet", .value = int32_literal(a, 0) }));
    annotations = append_nodes(a, annotations, annotation_value(a, (AnnotationValue) { .name = "DescriptorBinding", .value = int32_literal(a, 0) }));
    annotations = append_nodes(a, annotations, annotation(a, (Annotation) { .name = "Constants" }));

    size_t lifted_globals_count = 0;
    for (size_t i = 0; i < old_decls.count; i++) {
        const Node* odecl = old_decls.nodes[i];
        if (odecl->tag != GlobalVariable_TAG || odecl->payload.global_variable.address_space != AsGlobalPhysical)
            continue;

        member_tys[lifted_globals_count] = rewrite_node(&ctx.rewriter, odecl->type);
        member_names[lifted_globals_count] = get_decl_name(odecl);

        if (odecl->payload.global_variable.init)
            annotations = append_nodes(a, annotations, annotation_values(a, (AnnotationValues) {
                .name = "InitialValue",
                .values = mk_nodes(a, int32_literal(a, lifted_globals_count), rewrite_node(&ctx.rewriter, odecl->payload.global_variable.init))
            }));

        register_processed(&ctx.rewriter, odecl, int32_literal(a, lifted_globals_count));
        lifted_globals_count++;
    }

    if (lifted_globals_count > 0) {
        const Type* lifted_globals_struct_t = record_type(a, (RecordType) {
            .members = nodes(a, lifted_globals_count, member_tys),
            .names = strings(a, lifted_globals_count, member_names),
            .special = DecorateBlock
        });
        ctx.lifted_globals_decl = global_var(dst, annotations, lifted_globals_struct_t, "lifted_globals", AsShaderStorageBufferObject);
    }

    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    return dst;
}
