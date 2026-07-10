# FluxIR — Intermediate Representation

## Overview

FluxIR (Flux Intermediate Representation) is the canonical, lossless JSON representation of a Flux module. It is the primary interchange format between FluxASM (human-readable assembly) and the Flux runtime. Every FluxASM program can be round-tripped to FluxIR and back without semantic loss.

A FluxIR document is a single JSON object with a top-level `"module"` key containing all module metadata.

## Top-Level Structure

```json
{
  "module": {
    "name": "...",
    "version": "...",
    "entry_points": [...],
    "types": [...],
    "registers": [...],
    "devices": [...],
    "memory_regions": [...],
    "waves": [...],
    "units": [...],
    "deps": [...],
    "control_graph": [...]
  }
}
```

### `name`

- **Type:** string
- **Required:** Yes
- **Description:** The module name. Must be unique within a program.

### `version`

- **Type:** string
- **Required:** No (defaults to `"1.1.0"` if omitted)
- **Description:** The module version. Follows semantic versioning conventions.

## Entry Points

```json
"entry_points": [
  {
    "name": "main",
    "params": [
      { "reg": "s_ptr", "type": "i32" },
      { "reg": "s_len", "type": "i32" }
    ],
    "returns": [],
    "start_wave": "W0"
  }
]
```

- **`name`**: Entry point identifier.
- **`params`**: Array of input parameters. Each parameter specifies a `reg` (register name) and `type` (type name string).
- **`returns`**: Array of return value specifications, with the same shape as `params`.
- **`start_wave`**: The name of the wave where execution begins.

Entry points define the externally visible interface of the module. The simulator uses `start_wave` to determine where to begin execution.

## Types

```json
"types": [
  { "name": "i32",  "kind": "scalar", "bits": 32, "sign": "signed" },
  { "name": "f32",  "kind": "scalar", "bits": 32, "sign": "signed", "is_float": 1 },
  { "name": "u8",   "kind": "scalar", "bits": 8,  "sign": "unsigned" },
  { "name": "v4f32", "kind": "vector", "lanes": 4, "element_type": "f32" },
  { "name": "pred", "kind": "predicate" },
  { "name": "effect", "kind": "effect" }
]
```

### Scalar Type

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Unique type name used in register and parameter references |
| `kind` | `"scalar"` | Must be `"scalar"` |
| `bits` | integer | Bit-width (8, 16, 32, 64, etc.) |
| `sign` | `"signed"` or `"unsigned"` | Signedness for integer types |
| `is_float` | integer (optional) | Non-zero for floating-point types; defaults to 0 |

### Vector Type

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Unique type name |
| `kind` | `"vector"` | Must be `"vector"` |
| `lanes` | integer | Number of vector lanes |
| `element_type` | string | Name of the scalar element type (e.g., `"f32"`) |

### Predicate Type

```json
{ "name": "pred", "kind": "predicate" }
```

Predicates are 1-bit boolean values used for control flow decisions (`branch`, `sel`).

### Effect Type

```json
{ "name": "effect", "kind": "effect" }
```

Effect tokens represent ordering constraints for memory and IO operations. They are opaque values produced by `bind_memory` / `bind_device` and consumed by `ld`, `st`, `write`, `e_barrier`, and `e_fence`.

## Registers

```json
"registers": [
  { "name": "s0",    "class": "scalar",   "type": "i32" },
  { "name": "v0",    "class": "vector",   "type": "v4<f32>" },
  { "name": "p0",    "class": "predicate", "type": "pred" },
  { "name": "e_mem", "class": "effect",   "type": "effect" }
]
```

- **`name`**: Register identifier. Used in instructions, wave parameters, and dependency edges.
- **`class`**: One of `"scalar"`, `"vector"`, `"predicate"`, or `"effect"`.
- **`type`**: Name of a type defined in the `"types"` array.

Registers are the named storage locations in the module. The `class` field determines which instructions can read/write the register.

## Devices

```json
"devices": [
  {
    "name": "log",
    "kind": "generic",
    "properties": {
      "category": "logging"
    }
  }
]
```

- **`name`**: Device identifier.
- **`kind`**: Device category (e.g., `"generic"`).
- **`properties`**: Arbitrary JSON object of device-specific properties.

Devices are bound to effect registers via `bind_device` instructions.

## Memory Regions

```json
"memory_regions": [
  {
    "name": "data_region",
    "kind": "global",
    "size": 1024,
    "attributes": {}
  }
]
```

- **`name`**: Memory region identifier.
- **`kind`**: Region category (e.g., `"global"`, `"shared"`).
- **`size`**: Size in bytes.
- **`attributes`**: JSON object of region-specific attributes, or `null`.

Memory regions are bound to effect registers via `bind_memory` instructions.

## Waves

```json
"waves": [
  {
    "name": "W0",
    "params": [],
    "units": ["U_bind"]
  },
  {
    "name": "Wloop",
    "params": [
      { "reg": "s_acc", "type": "i32" },
      { "reg": "s_index", "type": "i32" }
    ],
    "units": ["U_body", "U_check", "U_branch"]
  }
]
```

- **`name`**: Wave identifier. Must be unique within the module.
- **`params`**: Array of wave parameters. Each parameter has a `reg` (register name) and `type` (type name). Wave parameters are the inputs passed from one wave to the next via `branch` instructions.
- **`units`**: Array of unit names that belong to this wave.

Waves are the execution stages of a Flux program. A wave activates all its units, then transitions to another wave via `branch` or terminates via `end`.

## Units

```json
"units": [
  {
    "name": "U_bind",
    "wave": "W0",
    "uses": ["s_ptr", "s_len"],
    "defs": ["e_mem", "e_log", "s_acc", "s_index"],
    "latency": 0,
    "predicate": "p_cond",
    "instructions": [
      {
        "opcode": "bind_memory",
        "operands": [
          { "kind": "reg", "name": "e_mem" },
          { "kind": "ref", "type": "memory_region", "name": "data_region" }
        ]
      },
      {
        "opcode": "mov.s",
        "operands": [
          { "kind": "reg", "name": "s_acc" },
          { "kind": "imm", "value": 0 }
        ]
      }
    ]
  }
]
```

- **`name`**: Unit identifier. Must be unique within the module.
- **`wave`**: Name of the owning wave.
- **`uses`**: Array of register names read by this unit (inputs from dependency predecessors or entry parameters).
- **`defs`**: Array of register names written by this unit (outputs to dependency successors).
- **`latency`**: Optional integer latency hint (default 0).
- **`predicate`**: Optional predicate register name for conditional execution.
- **`instructions`**: Array of `flux_instruction` objects.

### Instructions

```json
{
  "opcode": "add.s",
  "operands": [
    { "kind": "reg",  "name": "s0" },
    { "kind": "reg",  "name": "s1" },
    { "kind": "imm",  "value": 42 }
  ]
}
```

- **`opcode`**: Instruction mnemonic (e.g., `mov.s`, `add.s`, `branch`, `end`).
- **`operands`**: Array of operand objects.

### Operand Kinds

| Kind | Fields | Description |
|------|--------|-------------|
| `reg` | `name` | Register reference |
| `imm` | `value` (number or string) | Immediate value |
| `ref` | `type`, `name` | Reference to a device (`type: "device"`) or memory region (`type: "memory_region"`) |
| `wave` | `name`, `args` (object) | Control flow target wave with argument mapping |

#### Wave Operand

```json
{ "kind": "wave", "name": "Wloop", "args": { "s_acc": "s_newacc", "s_index": "s_newindex" } }
```

- **`name`**: Target wave name.
- **`args`**: Object mapping target wave parameter names to source register names. The keys are the formal parameter register names of the target wave; the values are the actual argument register names from the current wave.

## Dependencies

```json
"deps": [
  {
    "from": "U_bind",
    "to": "U_body",
    "regs": ["s_acc", "s_index", "e_mem"]
  }
]
```

- **`from`**: Source unit name.
- **`to`**: Target unit name.
- **`regs`**: Array of register names that flow from `from` to `to`.

Dependencies define the dataflow edges between units. They specify which registers produced by one unit are consumed by another. The validator checks that every register in a unit's `uses` list is either:
1. Declared as a wave parameter,
2. Defined by a dependency predecessor, or
3. Declared in the module's registers (assumed to be an entry-point input).

## Control Graph

```json
"control_graph": [
  {
    "from": "W0",
    "to": ["Wloop"]
  },
  {
    "from": "Wloop",
    "to": ["Wend", "Wloop"]
  }
]
```

- **`from`**: Source wave name.
- **`to`**: Array of target wave names.

The control graph is optional. When present, it constrains which `branch` targets are valid. When absent, all `branch` targets are accepted (the validator does not enforce control graph membership).

## Parsing API

### `flux_status flux_ir_parse(const char *json_text, flux_module *mod, flux_error *err)`

Parses a FluxIR JSON document from a null-terminated string.

- **Parameters:**
  - `json_text`: Null-terminated JSON string.
  - `mod`: Pre-created module to populate. Must have a valid `arena`.
  - `err`: Error output structure.
- **Returns:** `FLUX_OK` on success, `FLUX_ERR_PARSE` on malformed JSON or invalid FluxIR structure, `FLUX_ERR_OOM` on allocation failure.

### `flux_status flux_ir_parse_file(const char *path, flux_module *mod, flux_error *err)`

Reads a file and parses it as FluxIR JSON.

- **Parameters:**
  - `path`: Filesystem path to the JSON file.
  - `mod`: Pre-created module to populate.
  - `err`: Error output structure.
- **Returns:** `FLUX_OK` on success, `FLUX_ERR_PARSE` if the file cannot be opened or contains invalid JSON, `FLUX_ERR_OOM` on allocation failure.

## Serialization API

### `char* flux_ir_serialize(const flux_module *mod, flux_error *err)`

Serializes a `flux_module` to a compact JSON string. The returned string is allocated from the module's arena and remains valid until the module is destroyed or reset.

- **Returns:** Null-terminated JSON string, or `NULL` on error.
- **Output format:** The JSON is compact (no extra whitespace) and round-trips correctly through `flux_ir_parse`.

The serializer builds a temporary JSON value tree in a scratch arena, then serializes it to a string, which is finally copied into the module's arena for lifetime safety.

## Round-Trip Guarantee

FluxIR is designed to be lossless:

```
FluxASM -> flux_ir_serialize -> JSON -> flux_ir_parse -> Flux Module
```

The round-trip preserves all semantic information: types, registers, waves, units, instructions, dependencies, and control edges. Pretty-printed and compact JSON forms are both accepted by the parser.

## Error Handling

Parse errors are reported via `flux_error`:

```c
typedef struct flux_error {
    int    line;
    int    column;
    int    status;
    char   message[256];
} flux_error;
```

- **`line`**: 1-based line number where the error occurred.
- **`column`**: 1-based column number where the error occurred.
- **`status`**: Error code (`FLUX_ERR_PARSE`, `FLUX_ERR_OOM`, etc.).
- **`message`**: Human-readable error description.

## Examples

### Minimal Module

```json
{
  "module": {
    "name": "empty",
    "version": "1.1.0",
    "entry_points": [],
    "types": [],
    "registers": [],
    "devices": [],
    "memory_regions": [],
    "waves": [],
    "units": [],
    "deps": [],
    "control_graph": []
  }
}
```

### Module with a Single Wave

```json
{
  "module": {
    "name": "add_one",
    "version": "1.1.0",
    "entry_points": [
      { "name": "main", "params": [], "returns": [], "start_wave": "W0" }
    ],
    "types": [
      { "name": "i32", "kind": "scalar", "bits": 32, "sign": "signed" }
    ],
    "registers": [
      { "name": "s0", "class": "scalar", "type": "i32" }
    ],
    "waves": [
      { "name": "W0", "params": [], "units": ["U0"] }
    ],
    "units": [
      {
        "name": "U0",
        "wave": "W0",
        "uses": [],
        "defs": ["s0"],
        "instructions": [
          { "opcode": "mov.s", "operands": [
            { "kind": "reg", "name": "s0" },
            { "kind": "imm", "value": 1 }
          ]}
        ]
      }
    ],
    "deps": [],
    "control_graph": []
  }
}
```
