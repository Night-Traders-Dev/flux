#include "flux_internal.h"

/*===========================================================================
 * Reference Simulator
 *
 * A simple interpreter that walks the DAG executing instructions.
 * This is a reference implementation - it simulates the dataflow semantics.
 *===========================================================================*/

struct flux_sim_state {
    const flux_module *mod;
    flux_arena       *arena;

    /* Register file: maps register index -> int64_t value */
    int64_t *reg_values;
    int     *reg_written;   /* flags for which regs have been written */
    int      num_regs;

    /* Current wave state */
    int      current_wave;
    int64_t *wave_params;   /* parameter values for current wave activation */

    /* Execution trace (for debugging) */
    int      max_steps;
    int      current_step;
};

flux_sim_state* flux_sim_create(const flux_module *mod, flux_arena *arena)
{
    if (!mod || !arena) return NULL;

    flux_sim_state *s = (flux_sim_state*)flux_arena_alloc(arena, sizeof(flux_sim_state));
    if (!s) return NULL;
    memset(s, 0, sizeof(flux_sim_state));

    s->mod = mod;
    s->arena = arena;
    s->num_regs = mod->num_registers;

    if (s->num_regs > 0) {
        s->reg_values = (int64_t*)flux_arena_alloc(arena, s->num_regs * sizeof(int64_t));
        s->reg_written = (int*)flux_arena_alloc(arena, s->num_regs * sizeof(int));
        if (!s->reg_values || !s->reg_written) return NULL;
        memset(s->reg_values, 0, s->num_regs * sizeof(int64_t));
        memset(s->reg_written, 0, s->num_regs * sizeof(int));
    }

    s->max_steps = 100000;
    s->current_step = 0;

    return s;
}

void flux_sim_destroy(flux_sim_state *s)
{
    (void)s;
}

/*===========================================================================
 * Execute a unit's instructions
 *===========================================================================*/
static void sim_exec_unit(flux_sim_state *s, flux_unit *u,
                          int64_t *regs, int *written)
{
    for (int i = 0; i < u->num_instructions; i++) {
        flux_instruction *inst = &u->instructions[i];
        s->current_step++;
        if (s->current_step > s->max_steps) return;

        /* Resolve register index - first operand is typically dst */
        int dst_idx = -1;
        if (inst->num_operands > 0 && inst->operands[0].kind == FLUX_OP_REG) {
            dst_idx = flux_find_register(s->mod, inst->operands[0].u.reg.name);
        }

        /* Helper: get register index from operand */
        #define GET_REG(op_idx) \
            ((inst->num_operands > (op_idx) && inst->operands[op_idx].kind == FLUX_OP_REG) \
             ? flux_find_register(s->mod, inst->operands[op_idx].u.reg.name) : -1)
        #define GET_VAL(op_idx) \
            (inst->num_operands > (op_idx) ? \
             (inst->operands[op_idx].kind == FLUX_OP_IMM ? \
              (inst->operands[op_idx].u.imm.is_int ? inst->operands[op_idx].u.imm.ival : \
               (int64_t)inst->operands[op_idx].u.imm.fval) : \
              (GET_REG(op_idx) >= 0 && written[GET_REG(op_idx)] ? regs[GET_REG(op_idx)] : 0)) : 0)

        /* Simple opcode dispatch */
        const char *op = inst->opcode;

        if (strcmp(op, "mov.s") == 0 || strcmp(op, "mov") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "add.s") == 0 || strcmp(op, "add") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1) + GET_VAL(2);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "sub.s") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1) - GET_VAL(2);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "mul.s") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1) * GET_VAL(2);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "div.s") == 0) {
            if (dst_idx >= 0) {
                int64_t denom = GET_VAL(2);
                if (denom != 0) regs[dst_idx] = GET_VAL(1) / denom;
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "and.s") == 0 || strcmp(op, "and") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1) & GET_VAL(2);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "or.s") == 0 || strcmp(op, "or") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1) | GET_VAL(2);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "xor.s") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1) ^ GET_VAL(2);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "not.s") == 0) {
            if (dst_idx >= 0) {
                regs[dst_idx] = ~GET_VAL(1);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "cmp.s") == 0) {
            if (dst_idx >= 0 && inst->num_operands >= 4) {
                int64_t a = GET_VAL(1);
                int64_t b = GET_VAL(2);
                const char *cmp_op = inst->operands[3].u.imm.str;
                int result = 0;
                if (cmp_op) {
                    if (strcmp(cmp_op, "EQ") == 0) result = (a == b);
                    else if (strcmp(cmp_op, "NE") == 0) result = (a != b);
                    else if (strcmp(cmp_op, "LT") == 0) result = (a < b);
                    else if (strcmp(cmp_op, "GT") == 0) result = (a > b);
                    else if (strcmp(cmp_op, "LE") == 0) result = (a <= b);
                    else if (strcmp(cmp_op, "GE") == 0) result = (a >= b);
                }
                regs[dst_idx] = result;
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "sel.s") == 0) {
            if (dst_idx >= 0) {
                int64_t pred = GET_VAL(1);
                regs[dst_idx] = pred ? GET_VAL(2) : GET_VAL(3);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "ld.s") == 0) {
            /* Memory load - simulated: loads from zero-initialized memory */
            if (dst_idx >= 0) {
                regs[dst_idx] = 0; /* All memory reads return 0 in simulation */
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "st.s") == 0) {
            /* Memory store - no-op in simulation */
        }
        else if (strcmp(op, "bind_device") == 0 || strcmp(op, "bind_memory") == 0) {
            /* Effect binding - create a token value */
            if (dst_idx >= 0) {
                regs[dst_idx] = (int64_t)(uintptr_t)inst->opcode; /* Unique token */
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "e_barrier") == 0 || strcmp(op, "e_fence") == 0) {
            /* Effect ordering - pass through the token */
            if (dst_idx >= 0) {
                regs[dst_idx] = GET_VAL(1);
                written[dst_idx] = 1;
            }
        }
        else if (strcmp(op, "write") == 0) {
            /* IO write - no-op in simulation, but we can print */
            /* Effect reg is operand 0, data is operand 1 */
        }
        else if (strcmp(op, "branch") == 0 || strcmp(op, "goto") == 0) {
            /* Control flow is handled at the wave level, not instruction level */
        }
        else if (strcmp(op, "end") == 0) {
            /* Termination - handled at wave level */
        }
        else if (strcmp(op, "atomic_add") == 0) {
            /* Atomic add - no-op in simulation */
        }
        else if (strcmp(op, "atomic_cmpxchg") == 0) {
            /* Atomic cmpxchg - no-op in simulation */
        }
        else {
            /* Unknown opcode - skip silently */
        }

        #undef GET_REG
        #undef GET_VAL
    }
}

/*===========================================================================
 * Run simulation from entry point
 *===========================================================================*/
flux_status flux_sim_run(flux_sim_state *s, const char *entry,
                         int num_args, const int64_t *args, flux_error *err)
{
    if (!s || !entry) {
        flux_error_set(err, FLUX_ERR_INTERNAL, 0, 0, "null argument");
        return FLUX_ERR_INTERNAL;
    }
    flux_error_init(err);

    /* Find entry wave */
    int wi = flux_find_wave(s->mod, entry);
    if (wi < 0) {
        flux_error_set(err, FLUX_ERR_NOT_FOUND, 0, 0, "entry wave '%s' not found", entry);
        return FLUX_ERR_NOT_FOUND;
    }

    /* Reset register state */
    if (s->reg_values)
        memset(s->reg_values, 0, s->num_regs * sizeof(int64_t));
    if (s->reg_written)
        memset(s->reg_written, 0, s->num_regs * sizeof(int));

    /* Set initial parameters */
    flux_wave *w = &s->mod->waves[wi];
    for (int i = 0; i < num_args && i < w->num_params; i++) {
        int ri = flux_find_register(s->mod, w->params[i].reg);
        if (ri >= 0) {
            s->reg_values[ri] = args[i];
            s->reg_written[ri] = 1;
        }
    }

    /* Simple execution loop - walk through units in dependency order */
    /* For a reference simulator, we use a simple topological execution */
    int max_iterations = 1000;
    int iteration = 0;
    int current_wi = wi;

    while (iteration < max_iterations) {
        iteration++;
        s->current_step = 0;

        flux_wave *cw = &s->mod->waves[current_wi];

        /* Find all units belonging to this wave */
        /* Execute units in dependency order (simple BFS topological) */
        int num_wave_units = 0;
        int *wave_unit_indices = (int*)flux_arena_alloc(s->arena,
                                    s->mod->num_units * sizeof(int));
        if (!wave_unit_indices) {
            flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory");
            return FLUX_ERR_OOM;
        }

        for (int i = 0; i < s->mod->num_units; i++) {
            if (s->mod->units[i].wave && strcmp(s->mod->units[i].wave, cw->name) == 0) {
                wave_unit_indices[num_wave_units++] = i;
            }
        }

        /* Simple linear execution (order doesn't matter for dataflow,
         * but we execute all units in the wave) */
        for (int i = 0; i < num_wave_units; i++) {
            flux_unit *u = &s->mod->units[wave_unit_indices[i]];
            sim_exec_unit(s, u, s->reg_values, s->reg_written);
        }

        /* Check for end instruction or branch */
        int found_branch = 0;
        int found_end = 0;

        for (int i = 0; i < num_wave_units; i++) {
            flux_unit *u = &s->mod->units[wave_unit_indices[i]];
            for (int j = 0; j < u->num_instructions; j++) {
                if (strcmp(u->instructions[j].opcode, "end") == 0) {
                    found_end = 1;
                }
                if (strcmp(u->instructions[j].opcode, "branch") == 0) {
                    flux_instruction *binst = &u->instructions[j];
                    /* Evaluate condition (first operand) */
                    int cond_ri = -1;
                    int64_t cond_val = 0;
                    if (binst->num_operands >= 1 && binst->operands[0].kind == FLUX_OP_REG) {
                        cond_ri = flux_find_register(s->mod, binst->operands[0].u.reg.name);
                        if (cond_ri >= 0 && s->reg_written[cond_ri])
                            cond_val = s->reg_values[cond_ri];
                    }

                    /* Find the target wave operand */
                    int target_idx = cond_val ? 1 : 2;
                    if (binst->num_operands > target_idx &&
                        binst->operands[target_idx].kind == FLUX_OP_WAVE) {
                        flux_operand *wop = &binst->operands[target_idx];
                        int twi = flux_find_wave(s->mod, wop->u.wave.wave_name);
                        if (twi >= 0) {
                            /* Pass arguments */
                            for (int a = 0; a < wop->u.wave.num_args; a++) {
                                int pri = flux_find_register(s->mod, wop->u.wave.args[a].param_reg);
                                int ari = flux_find_register(s->mod, wop->u.wave.args[a].arg_reg);
                                if (pri >= 0 && ari >= 0 && s->reg_written[ari]) {
                                    s->reg_values[pri] = s->reg_values[ari];
                                    s->reg_written[pri] = 1;
                                }
                            }
                            current_wi = twi;
                            found_branch = 1;
                            break;
                        }
                    }
                }
                if (found_branch) break;
            }
            if (found_branch || found_end) break;
        }

        if (found_end) break;
        if (!found_branch) break; /* No branch and no end - terminate */
    }

    return FLUX_OK;
}
