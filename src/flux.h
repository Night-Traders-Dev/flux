#ifndef FLUX_H
#define FLUX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Version
 *===========================================================================*/
#define FLUX_VERSION "1.1.0"

/*===========================================================================
 * Memory arena - region-based allocator for all Flux objects
 *===========================================================================*/
typedef struct flux_arena flux_arena;
flux_arena* flux_arena_create(size_t capacity);
void        flux_arena_destroy(flux_arena *a);
void*       flux_arena_alloc(flux_arena *a, size_t size);
char*       flux_arena_strdup(flux_arena *a, const char *s);
char*       flux_arena_strndup(flux_arena *a, const char *s, size_t n);
void        flux_arena_reset(flux_arena *a);
size_t      flux_arena_used(flux_arena *a);

/*===========================================================================
 * Type system
 *===========================================================================*/
typedef enum {
    FLUX_KIND_SCALAR   = 0,
    FLUX_KIND_VECTOR   = 1,
    FLUX_KIND_PREDICATE = 2,
    FLUX_KIND_EFFECT   = 3,
} flux_type_kind;

typedef enum {
    FLUX_SIGN_SIGNED   = 0,
    FLUX_SIGN_UNSIGNED = 1,
} flux_sign;

typedef struct flux_type {
    flux_type_kind kind;
    int            bits;     /* bit-width for scalar, total bits for vector */
    int            lanes;    /* lanes for vector, 0 otherwise */
    flux_sign      sign;     /* signed/unsigned for integer scalars */
    int            is_float; /* non-zero for floating-point types */
} flux_type;

/* Predefined type constructors */
flux_type flux_type_scalar(int bits, flux_sign sign, int is_float);
flux_type flux_type_vector(int lanes, int elem_bits, flux_sign sign, int is_float);
flux_type flux_type_pred(void);
flux_type flux_type_effect(void);

/* Parse a type name like "i32", "u8", "f64", "v4<f32>", "pred", "effect" */
int flux_type_parse(const char *name, flux_type *out);

/* Format a type name into buffer, returns length written */
int flux_type_format(const flux_type *t, char *buf, size_t cap);

/*===========================================================================
 * Register classes
 *===========================================================================*/
typedef enum {
    FLUX_REG_SCALAR   = 0,
    FLUX_REG_VECTOR   = 1,
    FLUX_REG_PREDICATE = 2,
    FLUX_REG_EFFECT   = 3,
} flux_reg_class;

typedef struct flux_register {
    char          *name;
    flux_reg_class reg_class;
    flux_type      type;
} flux_register;

/*===========================================================================
 * Operands
 *===========================================================================*/
typedef enum {
    FLUX_OP_REG,       /* register reference */
    FLUX_OP_IMM,       /* immediate value (string or numeric) */
    FLUX_OP_REF,       /* reference to device/memory_region by name */
    FLUX_OP_WAVE,      /* wave target with argument map */
} flux_operand_kind;

typedef struct flux_wave_arg {
    char *param_reg;  /* formal parameter register name */
    char *arg_reg;    /* actual argument register name */
} flux_wave_arg;

typedef struct flux_operand {
    flux_operand_kind kind;
    union {
        struct { char *name; } reg;
        struct { char *str; int is_int; int64_t ival; double fval; } imm;
        struct { char *type; char *name; } ref;
        struct { char *wave_name; int num_args; flux_wave_arg *args; } wave;
    } u;
} flux_operand;

/*===========================================================================
 * Instructions
 *===========================================================================*/
typedef struct flux_instruction {
    char          *opcode;
    int            num_operands;
    flux_operand  *operands;
} flux_instruction;

/*===========================================================================
 * Units
 *===========================================================================*/
typedef struct flux_unit {
    char              *name;
    char              *wave;         /* owning wave name */
    int                num_uses;
    char             **uses;         /* register names */
    int                num_defs;
    char             **defs;         /* register names */
    int                latency;
    char              *predicate;    /* optional predicate register */
    int                num_instructions;
    flux_instruction  *instructions;
} flux_unit;

/*===========================================================================
 * Waves
 *===========================================================================*/
typedef struct flux_wave_param {
    char     *reg;   /* register name */
    flux_type type;  /* parameter type */
} flux_wave_param;

typedef struct flux_wave {
    char             *name;
    int               num_params;
    flux_wave_param  *params;
    int               num_units;
    char            **units;    /* unit names belonging to this wave */
} flux_wave;

/*===========================================================================
 * Device and Memory Region
 *===========================================================================*/
typedef struct flux_device {
    char  *name;
    char  *kind;        /* "generic" etc. */
    char **prop_keys;
    char **prop_values;
    int    num_props;
} flux_device;

typedef struct flux_memory_region {
    char *name;
    char *kind;         /* "global", "shared", etc. */
    int   size;
    char *attributes;   /* JSON object string, or NULL */
} flux_memory_region;

/*===========================================================================
 * Edges
 *===========================================================================*/
typedef struct flux_dep {
    char   *from;
    char   *to;
    int     num_regs;
    char  **regs;
} flux_dep;

typedef struct flux_control_edge {
    char   *from;
    int     num_to;
    char  **to;
} flux_control_edge;

/*===========================================================================
 * Entry Point
 *===========================================================================*/
typedef struct flux_entry_point {
    char    *name;
    int      num_params;
    char   **param_regs;    /* register names */
    flux_type *param_types; /* parameter types */
    int      num_returns;
    char   **return_regs;
    flux_type *return_types;
    char    *start_wave;
} flux_entry_point;

/*===========================================================================
 * Module - top-level container for a FluxIR program
 *===========================================================================*/
typedef struct flux_module {
    char                 *name;
    char                 *version;

    int                   num_entry_points;
    flux_entry_point     *entry_points;

    int                   num_types;
    flux_type            *types;       /* using inline structs */
    char                **type_names;  /* parallel array of type names */

    int                   num_registers;
    flux_register        *registers;

    int                   num_devices;
    flux_device          *devices;

    int                   num_memory_regions;
    flux_memory_region   *memory_regions;

    int                   num_waves;
    flux_wave            *waves;

    int                   num_units;
    flux_unit            *units;

    int                   num_deps;
    flux_dep             *deps;

    int                   num_control_edges;
    flux_control_edge    *control_edges;

    /* Arena owning all memory */
    flux_arena           *arena;
} flux_module;

/*===========================================================================
 * Status / Error reporting
 *===========================================================================*/
typedef enum {
    FLUX_OK                = 0,
    FLUX_ERR_OOM           = -1,
    FLUX_ERR_PARSE         = -2,
    FLUX_ERR_VALIDATION    = -3,
    FLUX_ERR_NOT_FOUND     = -4,
    FLUX_ERR_TYPE_MISMATCH = -5,
    FLUX_ERR_INTERNAL      = -99,
} flux_status;

typedef struct flux_error {
    int    line;
    int    column;
    int    status;
    char   message[256];
} flux_error;

void flux_error_init(flux_error *e);
void flux_error_set(flux_error *e, int status, int line, int col, const char *fmt, ...);

/*===========================================================================
 * Module lifecycle
 *===========================================================================*/
flux_module* flux_module_create(const char *name);
void         flux_module_destroy(flux_module *m);

/*===========================================================================
 * FluxIR JSON parser/serializer
 *===========================================================================*/
flux_status flux_ir_parse(const char *json_text, flux_module *mod, flux_error *err);
flux_status flux_ir_parse_file(const char *path, flux_module *mod, flux_error *err);

/* Serialize to JSON string (arena-allocated) */
char* flux_ir_serialize(const flux_module *mod, flux_error *err);

/*===========================================================================
 * FluxASM parser/printer
 *===========================================================================*/
flux_status flux_asm_parse(const char *asm_text, flux_module *mod, flux_error *err);
flux_status flux_asm_parse_file(const char *path, flux_module *mod, flux_error *err);
char*       flux_asm_print(const flux_module *mod, flux_error *err);

/*===========================================================================
 * Validator
 *===========================================================================*/
typedef struct flux_validation {
    int   num_errors;
    char *descriptions; /* arena-allocated, newline-separated */
} flux_validation;

flux_status flux_validate(const flux_module *mod, flux_validation *val, flux_error *err);

/*===========================================================================
 * Reference simulator
 *===========================================================================*/
typedef struct flux_sim_state flux_sim_state;

flux_sim_state* flux_sim_create(const flux_module *mod, flux_arena *arena);
void            flux_sim_destroy(flux_sim_state *s);
flux_status     flux_sim_run(flux_sim_state *s, const char *entry,
                             int num_args, const int64_t *args, flux_error *err);

/*===========================================================================
 * Utility: lookup helpers
 *===========================================================================*/
int  flux_find_register(const flux_module *mod, const char *name);
int  flux_find_wave(const flux_module *mod, const char *name);
int  flux_find_unit(const flux_module *mod, const char *name);
int  flux_find_device(const flux_module *mod, const char *name);
int  flux_find_memory_region(const flux_module *mod, const char *name);
int  flux_find_type(const flux_module *mod, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_H */
