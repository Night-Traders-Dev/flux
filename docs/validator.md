# Validator

## Overview

The Flux validator (`flux_validate`) performs comprehensive structural, type, dependency, effect, and control-safety checks on a parsed `flux_module`. It implements the validation rules specified in the Flux specification (section 8).

The validator does not modify the module. It collects all detected errors into a `flux_validation` output structure and returns `FLUX_ERR_VALIDATION` if any errors were found.

## API

### `flux_status flux_validate(const flux_module *mod, flux_validation *val, flux_error *err)`

Validates a Flux module.

- **Parameters:**
  - `mod`: The module to validate. Must not be `NULL`.
  - `val`: Output structure for validation results. `num_errors` is set to the count of errors; `descriptions` contains newline-separated error messages allocated from the module's arena.
  - `err`: Error output for internal/operational errors (not validation errors).
- **Returns:** `FLUX_OK` if the module is valid, `FLUX_ERR_VALIDATION` if errors were found, `FLUX_ERR_INTERNAL` on null argument.

### `flux_validation` Structure

```c
typedef struct flux_validation {
    int   num_errors;
    char *descriptions; /* arena-allocated, newline-separated */
} flux_validation;
```

- **`num_errors`**: Total number of validation errors detected.
- **`descriptions`**: Concatenated error messages, one per line. Allocated from the module's arena. The caller should not free this directly; it is reclaimed when the module's arena is destroyed.

## Validation Phases

The validator executes the following checks in order:

### 8.1 Structural Rules

These checks verify the basic integrity of the module graph.

#### Module Name

The module must have a non-empty name.

```
Structural: module has no name
```

#### Unit-to-Wave References

Every unit must reference a valid wave name in its `wave` field.

```
Structural: unit 'U0' references unknown wave 'W99'
```

#### Unit Wave Membership

Every unit must belong to exactly one wave. Units with a `NULL` or empty `wave` field are rejected.

```
Structural: unit 'U0' does not belong to any wave
```

#### Wave-to-Unit References

Every unit name listed in a wave's `units` array must correspond to an actual unit in the module.

```
Structural: wave 'W0' references unknown unit 'U99'
```

#### Unique Wave Names

Wave names must be unique within the module.

```
Structural: duplicate wave name 'W0'
```

#### Entry Point Start Wave

Every entry point's `start_wave` must reference a valid wave name.

```
Structural: entry point 'main' references unknown start_wave 'W99'
```

#### Dependency Edge References

Every dependency edge's `from` and `to` fields must reference valid unit names.

```
Structural: dep 'U0' -> 'U1' references unknown source unit 'U99'
Structural: dep 'U0' -> 'U1' references unknown target unit 'U99'
```

#### Wave Parameter Registration

Every register listed in a wave's `params` must also be declared in the module's `registers` array.

```
Structural: wave 'Wloop' parameter 's_acc' not declared in registers
```

#### Wave Parameter Type Matching

The type of each wave parameter must match the type of its corresponding register. The check compares `kind` and `bits` (and `sign` for integers).

```
Structural: wave 'Wloop' param 's_acc' type i32 doesn't match register type i64
```

#### Control Graph References

If a control graph is present, every edge's `from` and `to` fields must reference valid wave names.

```
Structural: control_graph edge 'W0' references unknown wave
Structural: control_graph edge 'W0' -> 'W99' references unknown wave
```

### 8.3 Dependency Safety

These checks verify that dataflow is well-formed.

#### Register Use Validation

For every register in a unit's `uses` list, the validator checks:

1. Is it a wave parameter? If so, it is valid (wave parameters are implicit inputs).
2. Is it defined by a dependency predecessor? If so, it is valid.
3. Is it defined by the unit itself (self-loop)? If so, it is valid.
4. Is it declared in the module's `registers`? If so, it is assumed to be an entry-point input and is valid.
5. Otherwise, it is an error.

```
Dependency: unit 'U_body' uses undeclared register 's_secret'
```

### 8.3.1 Cycle Detection

The validator performs a depth-first search (DFS) over the dependency graph to detect cycles. The graph is built from the `deps` array, mapping unit names to unit indices.

If a cycle is found:

```
Structural: dependency graph contains a cycle
```

Implementation details:
- Uses iterative DFS to avoid stack overflow on large modules.
- Builds an adjacency list from the dependency edges.
- Tracks visited nodes and nodes on the current recursion stack.

### 8.4 Effect Safety

These checks verify correct usage of effect tokens.

#### Bind Operand Types

`bind_memory` and `bind_device` instructions must have their first operand be an effect register.

```
Effect: bind_device/bind_memory output 's_mem' is not an effect register
```

#### Memory Access Effect Operands

`ld.s`, `st.s`, `ld.v`, `st.v`, `atomic_add`, and `atomic_cmpxchg` must have their second operand be an effect register.

```
Effect: opcode 'ld.s' uses non-effect register 's0' for memory access
```

#### Write Operand Type

`write` must have its first operand be an effect register.

```
Effect: 'write' uses non-effect register 's0'
```

### 8.5 Control Safety

These checks verify correct control flow usage.

#### Branch Target Validation

For every `branch` instruction, each `wave` operand is checked:

1. The target wave must exist.
2. The number of arguments provided must match the target wave's parameter count.
3. If a control graph is present, the branch target must be listed as a valid edge from the current wave.

```
Control: branch target 'W99' is not a valid wave
Control: branch to 'Wloop' provides 1 args but wave expects 2 params
Control: branch from 'W0' to 'Wloop' not in control_graph
```

If no control graph is present, branch target validation is skipped for rule 3.

#### End and Branch Mutual Exclusion

A wave must not contain both `end` and `branch` instructions.

```
Control: wave 'W0' contains both 'end' and 'branch'
```

#### Goto Parameter Check

`goto` instructions must target waves with zero parameters. Waves with parameters require `branch` to pass arguments.

```
Control: 'goto' to wave 'Wloop' which has parameters (use branch)
```

## Error Reporting

All validation errors are accumulated into `val->descriptions` as a single newline-separated string. The caller can iterate over the lines or display the entire string.

Example:

```c
flux_validation val;
flux_error err;
flux_status status = flux_validate(mod, &val, &err);
if (status == FLUX_ERR_VALIDATION) {
    printf("Validation found %d error(s):\n%s\n", val.num_errors, val.descriptions);
}
```

Output:

```
Validation found 3 error(s):
Structural: wave 'Wloop' parameter 's_acc' not declared in registers
Dependency: unit 'U_body' uses undeclared register 's_secret'
Control: branch from 'W0' to 'Wloop' not in control_graph
```

## Memory Model

Validation uses the module's arena for all temporary allocations (adjacency lists, visited arrays, error strings). No additional memory is allocated outside the arena. This means validation does not introduce separate cleanup responsibilities.

## Limitations

- **No type narrowing**: The validator checks structural and effect rules but does not perform full type inference or narrowing.
- **No liveness analysis**: The validator does not check for unused registers or dead code.
- **No constant folding**: Immediate value validation is limited to syntactic checks.
- **No alias analysis**: Memory access aliasing is not analyzed.
