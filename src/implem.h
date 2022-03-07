#ifndef SHADY_IMPLEM_H
#define SHADY_IMPLEM_H

#include "ir.h"

#include "stdlib.h"
#include "stdio.h"

struct IrArena {
    int nblocks;
    int maxblocks;
    void** blocks;
    size_t available;

    struct IrConfig config;

    struct TypeTable* type_table;
};

void* arena_alloc(struct IrArena* arena, size_t size);
void emit(struct Program program, FILE* output);

struct TypeTable;
struct TypeTable* new_type_table();
void destroy_type_table(struct TypeTable*);

bool is_subtype(const struct Type* supertype, const struct Type* type);
void check_subtype(const struct Type* supertype, const struct Type* type);
enum DivergenceQualifier resolve_divergence(const struct Type* type);

const struct Type* noret_type(struct IrArena* arena);
const struct Type* needs_infer(struct IrArena* arena);

#define NODEDEF(struct_name, short_name) const struct Type* infer_##short_name(struct IrArena*, struct struct_name);
NODES()
#undef NODEDEF

#ifdef _MSC_VER
#define SHADY_UNREACHABLE __assume(0)
#else
#define SHADY_UNREACHABLE __builtin_unreachable()
#endif

#define SHADY_NOT_IMPLEM {    \
  error("not implemented\n"); \
  SHADY_UNREACHABLE;          \
}

#define error(...) {             \
  fprintf (stderr, __VA_ARGS__); \
  exit(-1);                      \
}

#endif
