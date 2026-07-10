# FluxASM — Assembly Language

## Overview

FluxASM is the human-readable assembly language for Flux programs. It is a line-oriented text format that maps directly to the FluxIR intermediate representation. Every valid FluxASM program can be parsed into a `flux_module` and printed back to equivalent FluxASM text.

FluxASM is designed to be familiar to programmers who know assembly languages, while adding constructs for dataflow and heterogeneous compute.

## Lexical Conventions

- **Identifiers**: Sequences of letters, digits, underscores, and dots. Must start with a letter, underscore, or dot.
  - Examples: `U0`, `s_index`, `.wave`, `Wloop`
- **Numbers**: Decimal integers and floating-point literals.
  - Integer: `42`, `-7`, `0`
  - Float: `3.14`, `-0.5`, `1e6`, `2.5e-3`
- **String literals**: Double-quoted strings with C-style escape sequences.
  - Examples: `"hello"`, `"line1\nline2"`
- **Comments**: Lines starting with `#` or `;` are comments and are ignored.
- **Whitespace**: Spaces and tabs are ignored. Newlines terminate directives and instructions.

## Directives

Directives start with a dot (`.`) and control module structure.

### `.wave`

Defines a wave (execution stage).

```
.wave Wname
.wave Wname (param1:type1, param2:type2)
```

- **`Wname`**: Wave identifier (must be unique).
- **Parameters** (optional): Comma-separated list of `name:type` pairs inside parentheses. These are the inputs passed from predecessor waves.

Example:

```
.wave W0
.wave Wloop (s_acc:i32, s_index:i32)
```

### `.unit`

Defines a unit within the current wave.

```
Uname:
Uname: uses: r1, r2  defs: r3  latency: 2  predicate: p0
```

- **`Uname`**: Unit identifier (must be unique). The colon is mandatory.
- **`uses:`**: Comma-separated list of register names read by this unit.
- **`defs:`**: Comma-separated list of register names written by this unit.
- **`latency:`**: Optional integer latency hint.
- **`predicate:`**: Optional predicate register name for conditional execution.

Example:

```
U_bind:
  uses: s_ptr, s_len
  defs: e_mem, e_log, s_acc, s_index
```

### `.dep`

Defines a dependency edge between units.

```
.dep U0 -> U1
.dep U0 -> U1 (s0, s1)
```

- **`U0`**: Source unit name.
- **`U1`**: Target unit name.
- **`(regs)`** (optional): Comma-separated list of register names that flow along this edge.

Example:

```
.dep U_bind -> U_body (s_acc, s_index, e_mem)
```

### `.mem`

Declares a memory region and binds it to an effect register.

```
.mem e_mem, MEM_REGION("data_region")
```

- **`e_mem`**: Effect register name. The register is auto-created if it does not exist.
- **`MEM_REGION("name")`**: Memory region reference. The region name is a string literal.

### `.io`

Declares a device and binds it to an effect register.

```
.io e_log, DEV_GENERIC("log")
```

- **`e_log`**: Effect register name.
- **`DEV_GENERIC("name")`**: Device reference. Currently only `DEV_GENERIC` is supported.

## Instructions

Instructions are written inside a unit body, indented with spaces (conventional but not required).

```
    opcode  operand1, operand2, operand3
```

### Operands

| Operand | Syntax | Description |
|---------|--------|-------------|
| Register | `s0`, `v1`, `p_cond`, `e_mem` | Named register. First letter determines class: `s`=scalar, `v`=vector, `p`=predicate, `e`=effect. |
| Immediate (integer) | `42`, `-7` | Integer literal |
| Immediate (float) | `3.14`, `1e6` | Floating-point literal |
| Immediate (string) | `"hello"` | String literal |
| Reference | `memory_region:data_region`, `device:log` | Reference to a memory region or device |
| Wave target | `Wloop(s_acc=s_newacc, s_index=s_newindex)` | Control flow target with argument mapping |

### Wave Target Syntax

```
Wavename(param_reg=arg_reg, ...)
```

- **`Wavename`**: Target wave name.
- **`param_reg`**: Formal parameter register name of the target wave.
- **`arg_reg`**: Actual argument register name from the current wave.

Example:

```
    branch  p_cond, Wloop(s_acc=s_newacc, s_index=s_newindex), Wend()
```

## Instruction Set

### Data Movement

| Opcode | Operands | Description |
|--------|----------|-------------|
| `mov.s` / `mov` | `dst, src` | Move scalar value |
| `mov.v` | `dst, src` | Move vector value |

### Arithmetic

| Opcode | Operands | Description |
|--------|----------|-------------|
| `add.s` / `add` | `dst, src1, src2` | Integer/scalar addition |
| `sub.s` | `dst, src1, src2` | Subtraction |
| `mul.s` | `dst, src1, src2` | Multiplication |
| `div.s` | `dst, src1, src2` | Division (integer or floating-point) |

### Bitwise

| Opcode | Operands | Description |
|--------|----------|-------------|
| `and.s` / `and` | `dst, src1, src2` | Bitwise AND |
| `or.s` / `or` | `dst, src1, src2` | Bitwise OR |
| `xor.s` | `dst, src1, src2` | Bitwise XOR |
| `not.s` | `dst, src` | Bitwise NOT |

### Comparison and Selection

| Opcode | Operands | Description |
|--------|----------|-------------|
| `cmp.s` | `dst, a, b, "EQ"\|"NE"\|"LT"\|"GT"\|"LE"\|"GE"` | Compare and set predicate |
| `sel.s` | `dst, pred, true_val, false_val` | Select based on predicate |

### Memory and Effects

| Opcode | Operands | Description |
|--------|----------|-------------|
| `ld.s` | `dst, eff, offset` | Scalar load from memory region |
| `st.s` | `eff, offset, src` | Scalar store to memory region |
| `ld.v` | `dst, eff, offset` | Vector load |
| `st.v` | `eff, offset, src` | Vector store |
| `bind_memory` | `eff, memory_region:name` | Bind memory region to effect token |
| `bind_device` | `eff, device:name` | Bind device to effect token |
| `e_barrier` | `eff_in, eff_out` | Effect barrier (ordering) |
| `e_fence` | `eff_in, eff_out` | Effect fence (stronger ordering) |
| `write` | `eff, data` | IO write (no-op in simulator) |

### Atomic Operations

| Opcode | Operands | Description |
|--------|----------|-------------|
| `atomic_add` | `dst, eff, src` | Atomic add to memory |
| `atomic_cmpxchg` | `dst, eff, expected, new` | Atomic compare-and-exchange |

### Control Flow

| Opcode | Operands | Description |
|--------|----------|-------------|
| `branch` | `cond, target_false, target_true` | Conditional branch to wave |
| `goto` | `target` | Unconditional jump to wave (target must have zero params) |
| `end` | — | Terminate the current wave |

## Register Naming Convention

FluxASM uses a prefix-based register naming convention that automatically determines the register class:

| Prefix | Class | Example |
|--------|-------|---------|
| `s` | Scalar | `s0`, `s_acc`, `s_index` |
| `v` | Vector | `v0`, `v_data` |
| `p` | Predicate | `p0`, `p_cond` |
| `e` | Effect | `e0`, `e_mem`, `e_log` |

When the parser encounters an identifier starting with one of these prefixes in an operand position, it automatically creates the register in the module if it does not already exist.

## Parsing API

### `flux_status flux_asm_parse(const char *asm_text, flux_module *mod, flux_error *err)`

Parses FluxASM text into a `flux_module`.

- **Parameters:**
  - `asm_text`: Null-terminated FluxASM source text.
  - `mod`: Pre-created module to populate.
  - `err`: Error output structure.
- **Returns:** `FLUX_OK` on success, `FLUX_ERR_PARSE` on syntax error, `FLUX_ERR_OOM` on allocation failure.

### `flux_status flux_asm_parse_file(const char *path, flux_module *mod, flux_error *err)`

Reads a file and parses it as FluxASM.

### `char* flux_asm_print(const flux_module *mod, flux_error *err)`

Pretty-prints a `flux_module` as FluxASM text. The returned string is allocated from the module's arena.

## Pretty-Print Format

The printer outputs:

1. A header comment with module name and version.
2. Type definitions (as comments).
3. Register declarations (as comments).
4. Device declarations (`.io` directives).
5. Memory region declarations (`.mem` directives).
6. Waves with their units and parameters (`.wave` directives).
7. Unit definitions with `uses`, `defs`, `latency`, and `predicate`.
8. Instructions with operands.
9. Dependency edges (`.dep` directives).
10. Control flow graph (as comments).

Example output:

```
# FluxASM module: array_sum
# Version: 1.1.0

# Types:
#   i32 = i32
#   pred = pred
#   effect = effect

# Registers:
#   s_ptr: scalar<i32>
#   s_len: scalar<i32>
#   e_mem: effect<effect>

.wave W0
  U_bind:
    uses: s_ptr, s_len
    defs: e_mem, e_log, s_acc, s_index
    bind_memory e_mem, memory_region:data_region
    bind_device e_log, device:log
    mov.s s_acc, 0
    mov.s s_index, 0

.wave Wloop (s_acc:i32, s_index:i32)
  U_body:
    uses: s_acc, s_index, e_mem, s_ptr
    defs: s_val, s_newacc, s_newindex
    ld.s s_val, e_mem, 0
    add.s s_newacc, s_acc, s_val
    add.s s_newindex, s_index, 1

  U_check:
    uses: s_index, s_len
    defs: p_cond
    cmp.s p_cond, s_index, s_len, "LT"

  U_branch:
    uses: p_cond
    branch p_cond, Wloop(s_acc=s_newacc, s_index=s_newindex), Wend()

# Dependencies:
.dep U_bind -> U_body (s_acc, s_index, e_mem)
.dep U_body -> U_check (s_val, s_newacc, s_newindex)
.dep U_check -> U_branch (p_cond)

# Control flow:
# .ctrl W0 -> Wloop
# .ctrl Wloop -> Wend
# .ctrl Wloop -> Wloop
```

## Parsing Internals

The FluxASM parser (`flux_asm_parse`) is a line-oriented recursive descent parser. It maintains an `asm_parser` state structure with the current line, column, and parsing context (current wave and unit).

Key implementation details:
- **Dynamic arrays**: Uses `flux_vec_grow` for all growable arrays (operands, wave parameters, unit lists, dependency registers). This replaces the previous fixed-size limits with arena-backed, overflow-checked growth.
- **Register auto-creation**: When an identifier with a register prefix (`s`, `v`, `p`, `e`) is encountered in an operand position, `asm_find_or_add_reg` creates the register in the module with an appropriate class and default type.
- **Comment handling**: Lines starting with `#` or `;` are skipped entirely.
- **Error recovery**: On parse failure within a unit, the parser skips to the next line and continues if possible.

## Examples

### Minimal Program

```
.wave W0
  U0:
    mov.s s0, 42
    end
```

### Loop with Branch

```
.wave W0
  U_init:
    uses: s_len
    defs: s_index
    mov.s s_index, 0
    branch p_cond, Wloop(s_index=s_index), Wend()

.wave Wloop (s_index:i32)
  U_body:
    uses: s_index
    defs: s_val
    ld.s s_val, e_mem, 0

  U_check:
    uses: s_index, s_len
    defs: p_cond
    cmp.s p_cond, s_index, s_len, "LT"

  U_branch:
    branch p_cond, Wloop(s_index=s_index), Wend()
```
