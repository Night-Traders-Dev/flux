# Reference Simulator

## Overview

The Flux reference simulator (`flux_sim_run`) is a simple interpreter that executes Flux programs in the host process. It is a reference implementation that demonstrates the dataflow semantics of Flux without requiring any hardware acceleration.

The simulator is not designed for performance. Its purpose is to provide a predictable, debuggable execution environment for testing and validation.

## Architecture

The simulator state (`flux_sim_state`) contains:

```c
struct flux_sim_state {
    const flux_module *mod;
    flux_arena       *arena;

    int64_t *reg_values;    // register file: index -> value
    int     *reg_written;   // flags: which registers have been written
    int      num_regs;

    int      current_wave;  // index of the currently executing wave
    int64_t *wave_params;   // parameter values for wave activation

    int      max_steps;     // safety limit on instruction execution
    int      current_step;  // current step counter
};
```

### Register File

- **`reg_values`**: Array of `int64_t` values, one per module register. Indexed by the register's position in `mod->registers`.
- **`reg_written`**: Boolean array tracking which registers have been assigned a value. Unwritten registers read as 0.

### Wave Parameters

When a `branch` transfers control to a new wave, argument registers from the source wave are copied into the parameter registers of the target wave. The `wave_params` field is reserved for future use in passing scalar arguments directly.

### Step Limit

`max_steps` is hardcoded to 100,000. If the simulator executes more than this many instructions, it halts to prevent infinite loops.

## Execution Model

### Entry Point Resolution

`flux_sim_run` takes an entry point name (not a wave name). It looks up the entry point in `mod->entry_points`, then reads its `start_wave` field to determine the initial wave.

```c
int ep_idx = flux_find_entry_point(s->mod, entry);
const char *start_wave_name = s->mod->entry_points[ep_idx].start_wave;
int wi = flux_find_wave(s->mod, start_wave_name);
```

This design allows a module to expose multiple entry points that start at different waves.

### Wave Execution Loop

The simulator executes waves in a loop:

1. Find all units belonging to the current wave.
2. Execute every unit's instructions (linear order, not topological).
3. Scan for `end` or `branch` instructions.
4. If `end` is found, halt.
5. If `branch` is found, evaluate the condition and transfer control to the target wave.
6. If neither is found, halt (no implicit continuation).

```
while (iteration < max_iterations) {
    execute all units in current wave;
    if (end found) break;
    if (branch found) transfer to target wave;
    else break;
}
```

The maximum number of wave iterations is 1000.

### Unit Execution

Each unit's instructions are executed in order. The simulator uses two helper macros:

```c
#define GET_REG(op_idx) \
    ((inst->num_operands > (op_idx) && inst->operands[op_idx].kind == FLUX_OP_REG) \
     ? flux_find_register(s->mod, inst->operands[op_idx].u.reg.name) : -1)

#define GET_VAL(op_idx) \
    (inst->num_operands > (op_idx) ? \
     (inst->operands[op_idx].kind == FLUX_OP_IMM ? \
      (inst->operands[op_idx].u.imm.is_int ? inst->operands[op_idx].u.imm.ival : \
       (int64_t)inst->operands[op_idx].u.imm.fval) : \
      (GET_REG(op_idx) >= 0 && written[GET_REG(op_idx)] ? regs[GET_REG(op_idx)] : 0)) : 0)
```

- `GET_REG(idx)` resolves an operand to a register index, or -1 if the operand is not a register.
- `GET_VAL(idx)` resolves an operand to its current value. For immediates, it returns the literal value. For registers, it returns the register file value (or 0 if the register has not been written yet).

### Operand Resolution

- **Register operands**: Resolved via `flux_find_register`. If the register has been written, its value is returned; otherwise, 0.
- **Immediate operands**: If `is_int` is true, `ival` is returned; otherwise, `fval` is cast to `int64_t`. String immediates are also supported for non-numeric values.
- **Reference operands**: Not directly used by the simulator (memory and device binding are handled at the effect level).
- **Wave operands**: Not directly executed; they are interpreted at the wave level by the branch handler.

## Supported Opcodes

### Data Movement

| Opcode | Behavior |
|--------|----------|
| `mov.s` / `mov` | `regs[dst] = GET_VAL(1)` |
| `mov.v` | Not explicitly implemented (falls through to unknown opcode) |

### Arithmetic

| Opcode | Behavior |
|--------|----------|
| `add.s` / `add` | `regs[dst] = GET_VAL(1) + GET_VAL(2)` |
| `sub.s` | `regs[dst] = GET_VAL(1) - GET_VAL(2)` |
| `mul.s` | `regs[dst] = GET_VAL(1) * GET_VAL(2)` |
| `div.s` | `regs[dst] = GET_VAL(1) / GET_VAL(2)` (no divide-by-zero check) |

### Bitwise

| Opcode | Behavior |
|--------|----------|
| `and.s` / `and` | `regs[dst] = GET_VAL(1) & GET_VAL(2)` |
| `or.s` / `or` | `regs[dst] = GET_VAL(1) | GET_VAL(2)` |
| `xor.s` | `regs[dst] = GET_VAL(1) ^ GET_VAL(2)` |
| `not.s` | `regs[dst] = ~GET_VAL(1)` |

### Comparison and Selection

| Opcode | Behavior |
|--------|----------|
| `cmp.s` | Compares `GET_VAL(1)` and `GET_VAL(2)` using the operator string in operand 3 (`"EQ"`, `"NE"`, `"LT"`, `"GT"`, `"LE"`, `"GE"`). Sets `regs[dst]` to 0 or 1. |
| `sel.s` | `regs[dst] = GET_VAL(1) ? GET_VAL(2) : GET_VAL(3)` |

### Memory and Effects

| Opcode | Behavior |
|--------|----------|
| `ld.s` | `regs[dst] = 0` (memory reads return 0 in simulation) |
| `st.s` | No-op |
| `ld.v` | Not explicitly implemented |
| `st.v` | Not explicitly implemented |
| `bind_memory` / `bind_device` | Creates a unique token value from the opcode string pointer. `regs[dst] = (int64_t)(uintptr_t)inst->opcode` |
| `e_barrier` / `e_fence` | `regs[dst] = GET_VAL(1)` (pass-through) |
| `write` | No-op |

### Atomic Operations

| Opcode | Behavior |
|--------|----------|
| `atomic_add` | No-op |
| `atomic_cmpxchg` | No-op |

### Control Flow

| Opcode | Behavior |
|--------|----------|
| `branch` | Evaluated at the wave level (see below) |
| `goto` | Evaluated at the wave level (see below) |
| `end` | Evaluated at the wave level (see below) |

Unknown opcodes are silently skipped.

## Branch Semantics

`branch` and `goto` are not executed as individual instructions inside `sim_exec_unit`. Instead, they are scanned after all units in a wave have executed.

### Branch Evaluation

1. The condition operand (operand 0) is evaluated as a boolean (non-zero = true).
2. If true, operand 1 (the `true` target wave) is selected.
3. If false, operand 2 (the `false` target wave) is selected.
4. The target wave operand's `args` object is iterated. For each `param_reg=arg_reg` pair, the source register's value is copied into the target parameter register.
5. Control transfers to the target wave.

### Goto Semantics

`goto` has a single wave operand. It unconditionally transfers control to the target wave. The validator enforces that the target wave has zero parameters.

### End Semantics

`end` terminates the current wave and halts the simulation.

## API

### `flux_sim_state* flux_sim_create(const flux_module *mod, flux_arena *arena)`

Creates a simulator state for the given module.

- **Parameters:**
  - `mod`: The module to simulate. Must not be `NULL`.
  - `arena`: Arena for simulator allocations (register file, working buffers).
- **Returns:** Simulator state, or `NULL` on allocation failure.

### `void flux_sim_destroy(flux_sim_state *s)`

Destroys the simulator state. Currently a no-op because all memory is arena-allocated.

### `flux_status flux_sim_run(flux_sim_state *s, const char *entry, int num_args, const int64_t *args, flux_error *err)`

Runs the simulation from the specified entry point.

- **Parameters:**
  - `s`: Simulator state created by `flux_sim_create`.
  - `entry`: Name of the entry point (not wave) to start from.
  - `num_args`: Number of initial arguments.
  - `args`: Array of `int64_t` arguments. These are copied into the start wave's parameter registers.
  - `err`: Error output.
- **Returns:** `FLUX_OK` on successful completion, `FLUX_ERR_NOT_FOUND` if the entry point or start wave does not exist, `FLUX_ERR_INTERNAL` on null argument.

### Argument Passing

Arguments are passed by position. The first `num_args` values are copied into the start wave's parameter registers in order. If `num_args` is less than the number of wave parameters, the remaining parameters are left at 0.

## Example

```c
flux_module *mod = flux_module_create("dag_test");
flux_ir_parse_file("program.json", mod, &err);

flux_arena *sim_arena = flux_arena_create(65536);
flux_sim_state *sim = flux_sim_create(mod, sim_arena);

int64_t args[2] = {100, 5};
flux_status status = flux_sim_run(sim, "main", 2, args, &err);

if (status == FLUX_OK) {
    int64_t s0 = sim->reg_values[flux_find_register(mod, "s0")];
    printf("s0 = %lld\n", (long long)s0);
}

flux_sim_destroy(sim);
flux_arena_destroy(sim_arena);
flux_module_destroy(mod);
```

## Limitations

- **No memory model**: All memory reads return 0. Stores are no-ops. There is no actual memory hierarchy.
- **No device IO**: `write` and device operations are no-ops.
- **No SIMD**: Vector operations are not implemented beyond register assignment.
- **No concurrency**: Waves execute sequentially. There is no parallelism.
- **No overflow detection**: Arithmetic wraps around on `int64_t` overflow (C standard behavior).
- **No divide-by-zero handling**: Division by zero produces undefined behavior (typically a crash or zero, depending on the platform).
- **No precise timing**: `latency` fields are parsed but not used for scheduling.
- **No symbolic execution**: All values are concrete `int64_t` integers.

## Future Directions

- Actual memory model with configurable regions.
- Deterministic scheduling with wave-level parallelism.
- Tracing and debugging support (instruction trace, register history).
- Checkpoint and restore for long-running simulations.
