# Type System

## Overview

The Flux type system defines the data types that can be used for registers, wave parameters, and immediate values. It is deliberately minimal, focusing on the types needed for heterogeneous compute pipelines: scalar integers, scalar floating-point, vectors, predicates, and effect tokens.

All type information is stored inline in `flux_type` structures. Types are value-typed (no pointer indirection), so they can be copied, compared, and passed by value.

## Type Kinds

The type system supports four fundamental kinds:

| Kind | Enum | Description |
|------|------|-------------|
| Scalar | `FLUX_KIND_SCALAR` | Fixed-width integer or floating-point value |
| Vector | `FLUX_KIND_VECTOR` | Fixed-width SIMD vector of scalar elements |
| Predicate | `FLUX_KIND_PREDICATE` | Boolean control token (1-bit semantics) |
| Effect | `FLUX_KIND_EFFECT` | Opaque effect token for memory/IO ordering |

## Type Structure

```c
typedef struct flux_type {
    flux_type_kind kind;
    int            bits;     // bit-width for scalar, total bits for vector
    int            lanes;    // lanes for vector, 0 otherwise
    flux_sign      sign;     // signed/unsigned for integer scalars
    int            is_float; // non-zero for floating-point types
} flux_type;
```

### Fields

- **`kind`**: One of the four `flux_type_kind` enum values.
- **`bits`**: For scalars, the bit-width (e.g., 32 for `i32`). For vectors, the element bit-width (e.g., 32 for `v4<f32>`). For predicates and effects, this is 0.
- **`lanes`**: For vectors, the number of lanes (e.g., 4 for `v4<f32>`). For all other kinds, this is 0.
- **`sign`**: For integer scalars, `FLUX_SIGN_SIGNED` or `FLUX_SIGN_UNSIGNED`. For non-integer types, this field is unused.
- **`is_float`**: Non-zero for floating-point scalars and vectors, zero for integer types.

## Predefined Type Constructors

### `flux_type flux_type_scalar(int bits, flux_sign sign, int is_float)`

Creates a scalar type.

```c
flux_type i32  = flux_type_scalar(32, FLUX_SIGN_SIGNED, 0);
flux_type u8   = flux_type_scalar(8,  FLUX_SIGN_UNSIGNED, 0);
flux_type f32  = flux_type_scalar(32, FLUX_SIGN_SIGNED, 1);
flux_type f64  = flux_type_scalar(64, FLUX_SIGN_SIGNED, 1);
```

### `flux_type flux_type_vector(int lanes, int elem_bits, flux_sign sign, int is_float)`

Creates a vector type. The `sign` and `is_float` parameters describe the element type.

```c
flux_type v4f32 = flux_type_vector(4, 32, FLUX_SIGN_SIGNED, 1);
flux_type v8i32 = flux_type_vector(8, 32, FLUX_SIGN_SIGNED, 0);
```

### `flux_type flux_type_pred(void)`

Creates a predicate type.

```c
flux_type pred = flux_type_pred();
```

### `flux_type flux_type_effect(void)`

Creates an effect type.

```c
flux_type effect = flux_type_effect();
```

## Parsing and Formatting

### `int flux_type_parse(const char *name, flux_type *out)`

Parses a type name string into a `flux_type`. Returns 1 on success, 0 on failure.

Supported formats:

| Format | Example | Result |
|--------|---------|--------|
| Integer scalar | `i8`, `i16`, `i32`, `i64` | Signed integer of given bit-width |
| Unsigned scalar | `u8`, `u16`, `u32`, `u64` | Unsigned integer of given bit-width |
| Float scalar | `f16`, `f32`, `f64` | Floating-point of given bit-width |
| Vector | `v4<f32>`, `v8<i32>` | Vector with given lanes and element type |
| Predicate | `pred` | Predicate type |
| Effect | `effect` | Effect type |

Notes:
- The parser accepts `v` followed by digits, then `<`, then a valid scalar type name, then `>`.
- Bit-width must be positive. Zero or negative widths are rejected.
- Vector element types must be scalar; `v4<pred>` is invalid.

### `int flux_type_format(const flux_type *t, char *buf, size_t cap)`

Formats a `flux_type` into a human-readable string. Returns the number of characters written (not counting the null terminator), or 0 on error.

```c
char buf[32];
flux_type_format(&i32,  buf, sizeof(buf));  // writes "i32"
flux_type_format(&v4f32, buf, sizeof(buf)); // writes "v4<f32>"
flux_type_format(&pred, buf, sizeof(buf));  // writes "pred"
flux_type_format(&effect, buf, sizeof(buf)); // writes "effect"
```

## Type Equality

Two `flux_type` values are equal if and only if all relevant fields match:

- For scalars: `kind`, `bits`, `sign`, and `is_float` must match.
- For vectors: `kind`, `lanes`, `bits`, `sign`, and `is_float` must match.
- For predicates: all predicates are equal (no parameters).
- For effects: all effects are equal (no parameters).

There is no `flux_type_equal` function exposed in the public API, but equality is used internally during validation (e.g., checking that wave parameter types match register types).

## Type Usage in the Module

### Module-Level Type Registry

The `flux_module` struct contains a parallel array of type names and `flux_type` values:

```c
int                   num_types;
flux_type            *types;       // array of type structs
char                **type_names;  // parallel array of type names
```

When parsing FluxIR JSON, the `types` array is populated from the `"types"` key. Each entry must have a `"name"` and a `"kind"`. For scalar types, `"bits"` and `"sign"` are required. For vector types, `"lanes"` and `"element_type"` are required.

### Register Types

Every `flux_register` has an associated `flux_type`. The register's type determines what operations can be performed on it and how immediates are coerced.

### Wave Parameter Types

Each `flux_wave_param` has a `reg` name and a `flux_type`. The validator checks that wave parameter types match the types of the corresponding registers declared in the module's `registers` section.

## Validated Constraints

The validator (`flux_validate`) enforces the following type-related rules:

1. **Wave parameter registration**: Every register listed in a wave's `params` must also appear in the module's `registers` array.
2. **Wave parameter type matching**: The type of a wave parameter must match the type of its corresponding register (`kind` and `bits` must be equal).

These checks are performed in `src/flux_val.c` around lines 119-145.

## Examples

### Defining types in FluxIR JSON

```json
{
  "module": {
    "types": [
      { "name": "i32",  "kind": "scalar", "bits": 32, "sign": "signed" },
      { "name": "u8",   "kind": "scalar", "bits": 8,  "sign": "unsigned" },
      { "name": "f32",  "kind": "scalar", "bits": 32, "sign": "signed", "is_float": 1 },
      { "name": "v4f32", "kind": "vector", "lanes": 4, "element_type": "f32" },
      { "name": "pred", "kind": "predicate" },
      { "name": "effect", "kind": "effect" }
    ]
  }
}
```

### Using types in registers

```json
{
  "registers": [
    { "name": "s0", "class": "scalar", "type": "i32" },
    { "name": "v0", "class": "vector", "type": "v4<f32>" },
    { "name": "p0", "class": "predicate", "type": "pred" },
    { "name": "e0", "class": "effect", "type": "effect" }
  ]
}
```

### Using types in wave parameters

```json
{
  "waves": [
    {
      "name": "Wloop",
      "params": [
        { "reg": "s_acc", "type": "i32" },
        { "reg": "s_index", "type": "i32" }
      ],
      "units": ["U_body", "U_check", "U_branch"]
    }
  ]
}
```
