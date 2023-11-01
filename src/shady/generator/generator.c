#include "util.h"
#include "log.h"
#include "growy.h"

#include <json-c/json.h>
#include <assert.h>

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

typedef const char* String;

static bool should_include_instruction(json_object* instruction) {
    String class = json_object_get_string(json_object_object_get(instruction, "class"));
    if (strcmp(class, "@exclude") == 0)
        return false;
    return true;
}

static void add_comments(Growy* g, String indent, json_object* comments) {
    if (!indent)
        indent = "";
    if (json_object_get_type(comments) == json_type_string) {
        growy_append_formatted(g, "%s/// %s\n", indent, json_object_get_string(comments));
    } else if (json_object_get_type(comments) == json_type_array) {
        size_t size = json_object_array_length(comments);
        for (size_t i = 0; i < size; i++)
            add_comments(g, indent, json_object_array_get_idx(comments, i));
    }
}

static String to_snake_case(String camel) {
    size_t camel_len = strlen(camel);
    size_t buffer_size = camel_len + 16;
    char* dst = malloc(buffer_size);
    while (true) {
        size_t j = 0;
        for (size_t i = 0; i < camel_len; i++) {
            if (j >= buffer_size)
                goto start_over;
            if (isupper(camel[i])) {
                if (i > 0 && !isupper(camel[i - 1]))
                    dst[j++] = '_';
                dst[j++] = tolower(camel[i]);
            } else {
                dst[j++] = camel[i];
            }
        }

        // null terminate if we have space
        if (j + 1 < buffer_size) {
            dst[j] = '\0';
            return dst;
        }

        start_over:
        buffer_size += 16;
        dst = realloc(dst, buffer_size);
    }
}

static String capitalize(String str) {
    size_t len = strlen(str);
    assert(len > 0);
    size_t buffer_size = len + 1;
    char* dst = malloc(buffer_size);
    dst[0] = toupper(str[0]);
    for (size_t i = 1; i < len; i++) {
        dst[i] = str[i];
    }
    dst[len] = '\0';
    return dst;
}

static String class_to_type(String class) {
    assert(class);
    if (strcmp(class, "STRING") == 0)
        return "String";
    if (strcmp(class, "STRINGS") == 0)
        return "Strings";
    return string_ends_with(class, "S") ? "Nodes" : "const Node*";
}

static void generate_header(Growy* g, json_object* shd, json_object* spv) {
    int32_t major = json_object_get_int(json_object_object_get(spv, "major_version"));
    int32_t minor = json_object_get_int(json_object_object_get(spv, "minor_version"));
    int32_t revision = json_object_get_int(json_object_object_get(spv, "revision"));
    growy_append_formatted(g, "/* Generated from SPIR-V %d.%d revision %d */\n", major, minor, revision);
    growy_append_formatted(g, "/* Do not edit this file manually ! */\n");
    growy_append_formatted(g, "/* It is generated by the 'generator' target using Json grammar files. */\n\n");
}

static void generate_address_spaces(Growy* g, json_object* address_spaces) {
    growy_append_formatted(g, "typedef enum AddressSpace_ {\n");
    for (size_t i = 0; i < json_object_array_length(address_spaces); i++) {
        json_object* as = json_object_array_get_idx(address_spaces, i);
        String name = json_object_get_string(json_object_object_get(as, "name"));
        add_comments(g, "\t", json_object_object_get(as, "description"));
        growy_append_formatted(g, "\tAs%s,\n", name);
    }
    growy_append_formatted(g, "\tNumAddressSpaces,\n");
    growy_append_formatted(g, "} AddressSpace;\n\n");

    growy_append_formatted(g, "static inline bool is_physical_as(AddressSpace as) {\n");
    growy_append_formatted(g, "\tswitch(as) {\n");
    for (size_t i = 0; i < json_object_array_length(address_spaces); i++) {
        json_object* as = json_object_array_get_idx(address_spaces, i);
        String name = json_object_get_string(json_object_object_get(as, "name"));
        if (json_object_get_boolean(json_object_object_get(as, "physical")))
            growy_append_formatted(g, "\t\tcase As%s: return true;\n", name);
    }
    growy_append_formatted(g, "\t\tdefault: return false;\n");
    growy_append_formatted(g, "\t}\n");
    growy_append_formatted(g, "}\n\n");
}

static void generate_node_classes(Growy* g, json_object* node_classes) {
    assert(json_object_get_type(node_classes) == json_type_array);
    growy_append_formatted(g, "typedef enum {\n");
    for (size_t i = 0; i < json_object_array_length(node_classes); i++) {
        json_object* node_class = json_object_array_get_idx(node_classes, i);
        String name = json_object_get_string(json_object_object_get(node_class, "name"));
        String capitalized = capitalize(name);
        growy_append_formatted(g, "\tNc%s = 0b1", capitalized);
        for (int c = 0; c < i; c++)
            growy_append_string_literal(g, "0");
        growy_append_formatted(g, ",\n");
        free(capitalized);
    }
    growy_append_formatted(g, "} NodeClass;\n\n");
}

static void generate_node_tags(Growy* g, json_object* nodes) {
    assert(json_object_get_type(nodes) == json_type_array);
    growy_append_formatted(g, "typedef enum {\n");
    growy_append_formatted(g, "\tInvalidNode_TAG,\n");

    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        assert(name);
        json_object* ops = json_object_object_get(node, "ops");
        if (!ops)
            add_comments(g, "\t", json_object_object_get(node, "description"));

        growy_append_formatted(g, "\t%s_TAG,\n", name);
    }
    growy_append_formatted(g, "} NodeTag;\n\n");
}

static bool starts_with_vowel(String str) {
    char vowels[] = { 'a', 'e', 'i', 'o', 'u' };
    for (size_t i = 0; i < (sizeof(vowels) / sizeof(char)); i++) {
        if (str[0] == vowels[i]) {
            return true;
        }
    }
    return false;
}

static void generate_node_tags_for_class(Growy* g, json_object* nodes, String class, String capitalized_class) {
    assert(json_object_get_type(nodes) == json_type_array);
    growy_append_formatted(g, "typedef enum {\n");
    if (starts_with_vowel(class))
        growy_append_formatted(g, "\tNotAn%s = 0,\n", capitalized_class);
    else
        growy_append_formatted(g, "\tNotA%s = 0,\n", capitalized_class);

    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        assert(name);
        String nclass = json_object_get_string(json_object_object_get(node, "class"));
        if (nclass && strcmp(nclass, class) == 0)
            growy_append_formatted(g, "\t%s_%s_TAG = %s_TAG,\n", capitalized_class, name, name);
    }
    growy_append_formatted(g, "} %sTag;\n\n", capitalized_class);
}

static void generate_node_payloads(Growy* g, json_object* nodes) {
    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);

        String name = json_object_get_string(json_object_object_get(node, "name"));
        assert(name);

        json_object* ops = json_object_object_get(node, "ops");
        if (ops) {
            assert(json_object_get_type(ops) == json_type_array);
            add_comments(g, "", json_object_object_get(node, "description"));
            growy_append_formatted(g, "typedef struct %s_ {\n", name);
            for (size_t j = 0; j < json_object_array_length(ops); j++) {
                json_object* op = json_object_array_get_idx(ops, j);
                String op_name = json_object_get_string(json_object_object_get(op, "name"));
                String op_type = json_object_get_string(json_object_object_get(op, "type"));
                String op_class = NULL;
                if (!op_type) {
                    op_class = json_object_get_string(json_object_object_get(op, "class"));
                    op_type = class_to_type(op_class);
                }
                assert(op_type);
                growy_append_formatted(g, "\t%s %s;\n", op_type, op_name);
            }
            growy_append_formatted(g, "} %s;\n\n", name);
        }
    }
}

static void generate_node_type(Growy* g, json_object* nodes) {
    growy_append_formatted(g, "struct Node_ {\n");
    growy_append_formatted(g, "\tIrArena* arena;\n");
    growy_append_formatted(g, "\tconst Type* type;\n");
    growy_append_formatted(g, "\tNodeTag tag;\n");
    growy_append_formatted(g, "\tunion NodesUnion {\n");

    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);

        String name = json_object_get_string(json_object_object_get(node, "name"));
        assert(name);

        String snake_name = json_object_get_string(json_object_object_get(node, "snake_name"));
        void* alloc = NULL;
        if (!snake_name) {
            alloc = snake_name = to_snake_case(name);
        }

        json_object* ops = json_object_object_get(node, "ops");
        if (ops)
            growy_append_formatted(g, "\t\t%s %s;\n", name, snake_name);

        if (alloc)
            free(alloc);
    }

    growy_append_formatted(g, "\t} payload;\n");
    growy_append_formatted(g, "};\n\n");
}

static void generate_grammar_header(Growy* g, json_object* shd, json_object* spv) {
    generate_header(g, shd, spv);

    generate_address_spaces(g, json_object_object_get(shd, "address-spaces"));

    json_object* node_classes = json_object_object_get(shd, "node-classes");
    generate_node_classes(g, node_classes);

    json_object* nodes = json_object_object_get(shd, "nodes");
    generate_node_tags(g, nodes);
    growy_append_formatted(g, "NodeClass get_node_class_from_tag(NodeTag tag);\n\n");
    generate_node_payloads(g, nodes);
    generate_node_type(g, nodes);

    for (size_t i = 0; i < json_object_array_length(node_classes); i++) {
        json_object* node_class = json_object_array_get_idx(node_classes, i);
        String name = json_object_get_string(json_object_object_get(node_class, "name"));
        assert(name);
        String capitalized = capitalize(name);
        generate_node_tags_for_class(g, nodes, name, capitalized);

        growy_append_formatted(g, "%sTag is_%s(const Node*);\n", capitalized, name);
        free(capitalized);
    }
}

void generate_llvm_shady_address_space_conversion(Growy* g, json_object* address_spaces) {
    growy_append_formatted(g, "AddressSpace convert_llvm_address_space(unsigned as) {\n");
    growy_append_formatted(g, "\tstatic bool warned = false;\n");
    growy_append_formatted(g, "\tswitch (as) {\n");
    for (size_t i = 0; i < json_object_array_length(address_spaces); i++) {
        json_object* as = json_object_array_get_idx(address_spaces, i);
        String name = json_object_get_string(json_object_object_get(as, "name"));
        json_object* llvm_id = json_object_object_get(as, "llvm-id");
        if (!llvm_id || json_object_get_type(llvm_id) != json_type_int)
            continue;
        growy_append_formatted(g, "\t\t case %d: return As%s;\n", json_object_get_int(llvm_id), name);
    }
    growy_append_formatted(g, "\t\tdefault:\n");
    growy_append_formatted(g, "\t\t\tif (!warned)\n");
    growy_append_string(g, "\t\t\t\twarn_print(\"Warning: unrecognised address space %d\", as);\n");
    growy_append_formatted(g, "\t\t\twarned = true;\n");
    growy_append_formatted(g, "\t\t\treturn AsGeneric;\n");
    growy_append_formatted(g, "\t}\n");
    growy_append_formatted(g, "}\n");
}

static void generate_l2s_code(Growy* g, json_object* shd, json_object* spv) {
    generate_header(g, shd, spv);
    growy_append_formatted(g, "#include \"l2s_private.h\"\n");
    growy_append_formatted(g, "#include \"log.h\"\n");
    growy_append_formatted(g, "#include <stdbool.h>\n");

    generate_llvm_shady_address_space_conversion(g, json_object_object_get(shd, "address-spaces"));
}

static void generate_node_names_string_array(Growy* g, json_object* nodes) {
    growy_append_formatted(g, "const char* node_tags[] = {\n");
    growy_append_formatted(g, "\t\"invalid\",\n");
    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        String snake_name = json_object_get_string(json_object_object_get(node, "snake_name"));
        void* alloc = NULL;
        if (!snake_name) {
            alloc = snake_name = to_snake_case(name);
        }
        assert(name);
        growy_append_formatted(g, "\t\"%s\",\n", snake_name);
        if (alloc)
            free(alloc);
    }
    growy_append_formatted(g, "};\n\n");
}

static void generate_node_has_payload_array(Growy* g, json_object* nodes) {
    growy_append_formatted(g, "const bool node_type_has_payload[]  = {\n");
    growy_append_formatted(g, "\tfalse,\n");
    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        json_object* ops = json_object_object_get(node, "ops");
        growy_append_formatted(g, "\t%s,\n", ops ? "true" : "false");
    }
    growy_append_formatted(g, "};\n\n");
}

static void generate_node_payload_hash_fn(Growy* g, json_object* nodes) {
    growy_append_formatted(g, "KeyHash hash_node_payload(const Node* node) {\n");
    growy_append_formatted(g, "\tKeyHash hash = 0;\n");
    growy_append_formatted(g, "\tswitch (node->tag) { \n");
    assert(json_object_get_type(nodes) == json_type_array);
    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        String snake_name = json_object_get_string(json_object_object_get(node, "snake_name"));
        void* alloc = NULL;
        if (!snake_name) {
            alloc = snake_name = to_snake_case(name);
        }
        json_object* ops = json_object_object_get(node, "ops");
        if (ops) {
            assert(json_object_get_type(ops) == json_type_array);
            growy_append_formatted(g, "\tcase %s_TAG: {\n", name);
            growy_append_formatted(g, "\t\t%s payload = node->payload.%s;\n", name, snake_name);
            for (size_t j = 0; j < json_object_array_length(ops); j++) {
                json_object* op = json_object_array_get_idx(ops, j);
                String op_name = json_object_get_string(json_object_object_get(op, "name"));
                String op_type = json_object_get_string(json_object_object_get(op, "type"));
                String op_class = NULL;
                if (!op_type) {
                    op_class = json_object_get_string(json_object_object_get(op, "class"));
                    assert(op_class);
                    op_type = class_to_type(op_class);
                }
                assert(op_type);

                bool ignore = json_object_get_boolean(json_object_object_get(op, "ignore"));
                if (!ignore) {
                    growy_append_formatted(g, "\t\thash = hash ^ hash_murmur(&payload.%s, sizeof(payload.%s));\n", op_name, op_name);
                }
            }
            growy_append_formatted(g, "\t\tbreak;\n");
            growy_append_formatted(g, "\t}\n", name);
        }
        if (alloc)
            free(alloc);
    }
    growy_append_formatted(g, "\t\tdefault: assert(false);\n");
    growy_append_formatted(g, "\t}\n");
    growy_append_formatted(g, "\treturn hash;\n");
    growy_append_formatted(g, "}\n");
}

static void generate_node_payload_cmp_fn(Growy* g, json_object* nodes) {
    growy_append_formatted(g, "bool compare_node_payload(const Node* a, const Node* b) {\n");
    growy_append_formatted(g, "\tbool eq = true;\n");
    growy_append_formatted(g, "\tswitch (a->tag) { \n");
    assert(json_object_get_type(nodes) == json_type_array);
    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        String snake_name = json_object_get_string(json_object_object_get(node, "snake_name"));
        void* alloc = NULL;
        if (!snake_name) {
            alloc = snake_name = to_snake_case(name);
        }
        json_object* ops = json_object_object_get(node, "ops");
        if (ops) {
            assert(json_object_get_type(ops) == json_type_array);
            growy_append_formatted(g, "\tcase %s_TAG: {\n", name);
            growy_append_formatted(g, "\t\t%s payload_a = a->payload.%s;\n", name, snake_name);
            growy_append_formatted(g, "\t\t%s payload_b = b->payload.%s;\n", name, snake_name);
            for (size_t j = 0; j < json_object_array_length(ops); j++) {
                json_object* op = json_object_array_get_idx(ops, j);
                String op_name = json_object_get_string(json_object_object_get(op, "name"));
                String op_type = json_object_get_string(json_object_object_get(op, "type"));
                String op_class = NULL;
                if (!op_type) {
                    op_class = json_object_get_string(json_object_object_get(op, "class"));
                    assert(op_class);
                    op_type = class_to_type(op_class);
                }
                assert(op_type);

                bool ignore = json_object_get_boolean(json_object_object_get(op, "ignore"));
                if (!ignore) {
                    growy_append_formatted(g, "\t\teq &= memcmp(&payload_a.%s, &payload_b.%s, sizeof(payload_a.%s)) == 0;\n", op_name, op_name, op_name);
                }
            }
            growy_append_formatted(g, "\t\tbreak;\n");
            growy_append_formatted(g, "\t}\n", name);
        }
        if (alloc)
            free(alloc);
    }
    growy_append_formatted(g, "\t\tdefault: assert(false);\n");
    growy_append_formatted(g, "\t}\n");
    growy_append_formatted(g, "\treturn eq;\n");
    growy_append_formatted(g, "}\n");
}

static void generate_node_class_from_tag(Growy* g, json_object* nodes) {
    growy_append_formatted(g, "NodeClass get_node_class_from_tag(NodeTag tag) {\n");
    growy_append_formatted(g, "\tswitch (tag) { \n");
    assert(json_object_get_type(nodes) == json_type_array);
    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        growy_append_formatted(g, "\t\tcase %s_TAG: \n", name);
        json_object* class = json_object_object_get(node, "class");
        if (class) {
            String cap = capitalize(json_object_get_string(class));
            growy_append_formatted(g, "\t\t\treturn Nc%s;\n", cap);
            free(cap);
        } else {
            growy_append_formatted(g, "\t\t\treturn 0;\n");
        }
    }
    growy_append_formatted(g, "\t\tdefault: assert(false);\n");
    growy_append_formatted(g, "\t}\n");
    growy_append_formatted(g, "\tSHADY_UNREACHABLE;\n");
    growy_append_formatted(g, "}\n");
}

static void generate_isa_for_class(Growy* g, json_object* nodes, String class, String capitalized_class) {
    assert(json_object_get_type(nodes) == json_type_array);
    growy_append_formatted(g, "%sTag is_%s(const Node* node) {\n", capitalized_class, class);
    growy_append_formatted(g, "\tif (get_node_class_from_tag(node->tag) & Nc%s)\n", capitalized_class);
    growy_append_formatted(g, "\t\treturn (%sTag) node->tag;\n", capitalized_class);
    growy_append_formatted(g, "\treturn (%sTag) 0;\n", capitalized_class);
    growy_append_formatted(g, "}\n\n");
}

static void generate_nodes_code(Growy* g, json_object* shd, json_object* spv) {
    generate_header(g, shd, spv);
    growy_append_formatted(g, "#include \"shady/ir.h\"\n");
    growy_append_formatted(g, "#include \"log.h\"\n");
    growy_append_formatted(g, "#include \"dict.h\"\n");
    growy_append_formatted(g, "#include <stdbool.h>\n\n");
    growy_append_formatted(g, "#include <string.h>\n\n");
    growy_append_formatted(g, "#include <assert.h>\n\n");

    json_object* nodes = json_object_object_get(shd, "nodes");
    generate_node_names_string_array(g, nodes);
    generate_node_has_payload_array(g, nodes);
    generate_node_payload_hash_fn(g, nodes);
    generate_node_payload_cmp_fn(g, nodes);
    generate_node_class_from_tag(g, nodes);

    json_object* node_classes = json_object_object_get(shd, "node-classes");
    for (size_t i = 0; i < json_object_array_length(node_classes); i++) {
        json_object* node_class = json_object_array_get_idx(node_classes, i);
        String name = json_object_get_string(json_object_object_get(node_class, "name"));
        assert(name);
        String capitalized = capitalize(name);
        generate_isa_for_class(g, nodes, name, capitalized);

        free(capitalized);
    }
}

enum {
    ArgSelf = 0,
    ArgGeneratorFn,
    ArgDstFile,
    ArgShadyJson,
    ArgSpirvGrammarSearchPathBegins
};

int main(int argc, char** argv) {
    assert(argc > ArgSpirvGrammarSearchPathBegins);

    char* mode = argv[ArgGeneratorFn];
    char* dst_file = argv[ArgDstFile];
    char* shd_grammar_json_path = argv[ArgShadyJson];
    // search the include path for spirv.core.grammar.json
    char* spv_core_json_path = NULL;
    for (size_t i = ArgSpirvGrammarSearchPathBegins; i < argc; i++) {
        char* path = format_string_new("%s/spirv/unified1/spirv.core.grammar.json", argv[i]);
        info_print("trying path %s\n", path);
        FILE* f = fopen(path, "rb");
        if (f) {
            spv_core_json_path = path;
            fclose(f);
            break;
        }
        free(path);
    }

    if (!spv_core_json_path)
        abort();

    json_tokener* tokener = json_tokener_new_ex(32);
    enum json_tokener_error json_err;

    struct {
        size_t size;
        char* contents;
        json_object* root;
    } shd_grammar;
    read_file(shd_grammar_json_path, &shd_grammar.size, &shd_grammar.contents);
    shd_grammar.root = json_tokener_parse_ex(tokener, shd_grammar.contents, shd_grammar.size);
    json_err = json_tokener_get_error(tokener);
    if (json_err != json_tokener_success) {
        error("Json tokener error: %s\n", json_tokener_error_desc(json_err));
    }

    struct {
        size_t size;
        char* contents;
        json_object* root;
    } spv_grammar;

    read_file(spv_core_json_path, &spv_grammar.size, &spv_grammar.contents);
    spv_grammar.root = json_tokener_parse_ex(tokener, spv_grammar.contents, spv_grammar.size);
    json_err = json_tokener_get_error(tokener);
    if (json_err != json_tokener_success) {
        error("Json tokener error: %s\n", json_tokener_error_desc(json_err));
    }

    info_print("Correctly opened json file: %s\n", spv_core_json_path);
    Growy* g = new_growy();

    if (strcmp(mode, "grammar-headers") == 0) {
        generate_grammar_header(g, shd_grammar.root, spv_grammar.root);
    } else if (strcmp(mode, "l2s") == 0) {
        generate_l2s_code(g, shd_grammar.root, spv_grammar.root);
    } else if (strcmp(mode, "nodes") == 0) {
        generate_nodes_code(g, shd_grammar.root, spv_grammar.root);
    } else {
        error_print("Unknown mode '%s'\n", mode);
        exit(-1);
    }

    size_t final_size = growy_size(g);
    growy_append_bytes(g, 1, (char[]) { 0 });
    char* generated = growy_deconstruct(g);
    info_print("debug: %s\n", generated);
    write_file(dst_file, final_size, generated);
    free(shd_grammar.contents);
    free(spv_grammar.contents);
    free(generated);

    // dump_module(mod);

    json_object_put(shd_grammar.root);
    json_object_put(spv_grammar.root);
    json_tokener_free(tokener);
    free(spv_core_json_path);
}