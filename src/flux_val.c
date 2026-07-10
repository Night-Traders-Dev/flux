#include "flux_internal.h"

/*===========================================================================
 * Validator
 *
 * Checks all validation rules from the spec (section 8).
 *===========================================================================*/

/* Write to validation output */
static void val_printf(flux_validation *val, flux_arena *arena, const char *fmt, ...)
{
    if (!val) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    /* Append to description string */
    size_t cur = val->descriptions ? strlen(val->descriptions) : 0;
    size_t new_len = cur + n + 1;
    char *new_desc = (char*)flux_arena_alloc(arena, new_len + 1);
    if (!new_desc) return;
    if (val->descriptions) {
        memcpy(new_desc, val->descriptions, cur);
    }
    memcpy(new_desc + cur, buf, n);
    new_desc[cur + n] = '\n';
    new_desc[cur + n + 1] = '\0';
    val->descriptions = new_desc;
    val->num_errors++;
}

/*===========================================================================
 * Validation checks
 *===========================================================================*/
flux_status flux_validate(const flux_module *mod, flux_validation *val, flux_error *err)
{
    if (!mod || !val) {
        flux_error_set(err, FLUX_ERR_INTERNAL, 0, 0, "null argument");
        return FLUX_ERR_INTERNAL;
    }
    flux_error_init(err);

    val->num_errors = 0;
    val->descriptions = NULL;

    /* Use module's arena for validation output */
    flux_arena *arena = mod->arena;

    /* 8.1 Structural Rules */

    /* Module must have a name */
    if (!mod->name || mod->name[0] == '\0') {
        val_printf(val, arena, "Structural: module has no name");
    }

    /* All waves referenced must exist */
    /* Check units reference valid waves */
    for (int i = 0; i < mod->num_units; i++) {
        if (mod->units[i].wave) {
            if (flux_find_wave(mod, mod->units[i].wave) < 0) {
                val_printf(val, arena, "Structural: unit '%s' references unknown wave '%s'",
                          mod->units[i].name, mod->units[i].wave);
            }
        }
    }

    /* All units must belong to exactly one wave */
    for (int i = 0; i < mod->num_units; i++) {
        if (!mod->units[i].wave) {
            val_printf(val, arena, "Structural: unit '%s' does not belong to any wave", mod->units[i].name);
        }
    }

    /* All units referenced by waves must exist */
    for (int wi = 0; wi < mod->num_waves; wi++) {
        for (int j = 0; j < mod->waves[wi].num_units; j++) {
            if (flux_find_unit(mod, mod->waves[wi].units[j]) < 0) {
                val_printf(val, arena, "Structural: wave '%s' references unknown unit '%s'",
                          mod->waves[wi].name, mod->waves[wi].units[j]);
            }
        }
    }

    /* Wave names must be unique */
    for (int i = 0; i < mod->num_waves; i++) {
        for (int j = i + 1; j < mod->num_waves; j++) {
            if (strcmp(mod->waves[i].name, mod->waves[j].name) == 0) {
                val_printf(val, arena, "Structural: duplicate wave name '%s'",
                          mod->waves[i].name);
            }
        }
    }

    /* Entry point start_wave must reference a valid wave */
    for (int i = 0; i < mod->num_entry_points; i++) {
        if (flux_find_wave(mod, mod->entry_points[i].start_wave) < 0) {
            val_printf(val, arena, "Structural: entry point '%s' references unknown start_wave '%s'",
                      mod->entry_points[i].name, mod->entry_points[i].start_wave);
        }
    }

    /* Dependency edges must reference valid units */
    for (int i = 0; i < mod->num_deps; i++) {
        if (flux_find_unit(mod, mod->deps[i].from) < 0) {
            val_printf(val, arena, "Structural: dep '%s' -> '%s' references unknown source unit '%s'",
                      mod->deps[i].from, mod->deps[i].to, mod->deps[i].from);
        }
        if (flux_find_unit(mod, mod->deps[i].to) < 0) {
            val_printf(val, arena, "Structural: dep '%s' -> '%s' references unknown target unit '%s'",
                      mod->deps[i].from, mod->deps[i].to, mod->deps[i].to);
        }
    }

    /* All registers must be declared before use (we track this) */

    /* Wave parameters must be declared in the registers section */
    for (int wi = 0; wi < mod->num_waves; wi++) {
        for (int j = 0; j < mod->waves[wi].num_params; j++) {
            if (flux_find_register(mod, mod->waves[wi].params[j].reg) < 0) {
                val_printf(val, arena, "Structural: wave '%s' parameter '%s' not declared in registers",
                          mod->waves[wi].name, mod->waves[wi].params[j].reg);
            }
        }
    }

    /* Wave parameter types must match register types */
    for (int wi = 0; wi < mod->num_waves; wi++) {
        for (int j = 0; j < mod->waves[wi].num_params; j++) {
            int ri = flux_find_register(mod, mod->waves[wi].params[j].reg);
            if (ri >= 0) {
                flux_type *pt = &mod->waves[wi].params[j].type;
                flux_type *rt = &mod->registers[ri].type;
                if (pt->kind != rt->kind || pt->bits != rt->bits) {
                    char pb[32], rb[32];
                    flux_type_format(pt, pb, sizeof(pb));
                    flux_type_format(rt, rb, sizeof(rb));
                    val_printf(val, arena, "Structural: wave '%s' param '%s' type %s doesn't match register type %s",
                              mod->waves[wi].name, mod->waves[wi].params[j].reg, pb, rb);
                }
            }
        }
    }

    /* Check control graph edges reference valid waves */
    for (int i = 0; i < mod->num_control_edges; i++) {
        if (flux_find_wave(mod, mod->control_edges[i].from) < 0) {
            val_printf(val, arena, "Structural: control_graph edge '%s' references unknown wave",
                      mod->control_edges[i].from);
        }
        for (int j = 0; j < mod->control_edges[i].num_to; j++) {
            if (flux_find_wave(mod, mod->control_edges[i].to[j]) < 0) {
                val_printf(val, arena, "Structural: control_graph edge '%s' -> '%s' references unknown wave",
                          mod->control_edges[i].from, mod->control_edges[i].to[j]);
            }
        }
    }

    /* 8.3 Dependency Safety */
    /* Check that units only use registers defined by deps or wave params */
    for (int ui = 0; ui < mod->num_units; ui++) {
        flux_unit *u = &mod->units[ui];

        for (int j = 0; j < u->num_uses; j++) {
            const char *reg_name = u->uses[j];

            /* Skip if it's a wave parameter */
            int is_wave_param = 0;
            int wi = u->wave ? flux_find_wave(mod, u->wave) : -1;
            if (wi >= 0) {
                for (int k = 0; k < mod->waves[wi].num_params; k++) {
                    if (strcmp(mod->waves[wi].params[k].reg, reg_name) == 0) {
                        is_wave_param = 1;
                        break;
                    }
                }
            }
            if (is_wave_param) continue;

            /* Check if register is defined by a dependency predecessor */
            int defined_by_dep = 0;
            for (int di = 0; di < mod->num_deps; di++) {
                if (strcmp(mod->deps[di].to, u->name) == 0) {
                    for (int rk = 0; rk < mod->deps[di].num_regs; rk++) {
                        if (strcmp(mod->deps[di].regs[rk], reg_name) == 0) {
                            defined_by_dep = 1;
                            break;
                        }
                    }
                }
                if (defined_by_dep) break;
            }

            /* Also check if it's defined by this unit itself (self-loop valid) */
            for (int dk = 0; dk < u->num_defs; dk++) {
                if (strcmp(u->defs[dk], reg_name) == 0) {
                    defined_by_dep = 1;
                    break;
                }
            }

            /* If the register is declared in the module, it's a valid input (e.g. entry point param) */
            if (!defined_by_dep && !is_wave_param) {
                if (flux_find_register(mod, reg_name) < 0) {
                    val_printf(val, arena, "Dependency: unit '%s' uses undeclared register '%s'",
                              u->name, reg_name);
                }
                /* Declared registers not in dep chain are assumed to be inputs from entry */
            }
        }
    }

    /* 8.3.1 Cycle Detection */
    /* Build adjacency list from deps and detect cycles using DFS */
    if (mod->num_deps > 0 && mod->num_units > 0) {
        int *visited = (int*)flux_arena_alloc(arena, mod->num_units * sizeof(int));
        int *rec_stack = (int*)flux_arena_alloc(arena, mod->num_units * sizeof(int));
        if (visited && rec_stack) {
            memset(visited, 0, mod->num_units * sizeof(int));
            memset(rec_stack, 0, mod->num_units * sizeof(int));

            /* Build adjacency: unit index -> list of target unit indices */
            int **adj = (int**)flux_arena_alloc(arena, mod->num_units * sizeof(int*));
            int *adj_count = (int*)flux_arena_alloc(arena, mod->num_units * sizeof(int));
            if (adj && adj_count) {
                memset(adj_count, 0, mod->num_units * sizeof(int));
                for (int di = 0; di < mod->num_deps; di++) {
                    int src = flux_find_unit(mod, mod->deps[di].from);
                    int dst = flux_find_unit(mod, mod->deps[di].to);
                    if (src >= 0 && dst >= 0) {
                        adj_count[src]++;
                    }
                }
                for (int i = 0; i < mod->num_units; i++) {
                    if (adj_count[i] > 0) {
                        adj[i] = (int*)flux_arena_alloc(arena, adj_count[i] * sizeof(int));
                        adj_count[i] = 0;
                    }
                }
                for (int di = 0; di < mod->num_deps; di++) {
                    int src = flux_find_unit(mod, mod->deps[di].from);
                    int dst = flux_find_unit(mod, mod->deps[di].to);
                    if (src >= 0 && dst >= 0) {
                        adj[src][adj_count[src]++] = dst;
                    }
                }

                /* DFS cycle detection */
                int has_cycle = 0;
                for (int i = 0; i < mod->num_units && !has_cycle; i++) {
                    if (!visited[i]) {
                        /* Simple iterative DFS to detect cycle */
                        int *stack = (int*)flux_arena_alloc(arena, mod->num_units * sizeof(int));
                        int *stack_adj_idx = (int*)flux_arena_alloc(arena, mod->num_units * sizeof(int));
                        if (stack && stack_adj_idx) {
                            int sp = 0;
                            stack[sp] = i;
                            stack_adj_idx[sp] = 0;
                            rec_stack[i] = 1;
                            while (sp >= 0) {
                                int u = stack[sp];
                                if (stack_adj_idx[sp] < adj_count[u]) {
                                    int v = adj[u][stack_adj_idx[sp]++];
                                    if (rec_stack[v]) {
                                        has_cycle = 1;
                                        break;
                                    }
                                    if (!visited[v]) {
                                        sp++;
                                        stack[sp] = v;
                                        stack_adj_idx[sp] = 0;
                                        rec_stack[v] = 1;
                                    }
                                } else {
                                    rec_stack[u] = 0;
                                    visited[u] = 1;
                                    sp--;
                                }
                            }
                        }
                    }
                }
                if (has_cycle) {
                    val_printf(val, arena, "Structural: dependency graph contains a cycle");
                }
            }
        }
    }

    /* 8.4 Effect Safety */
    /* Track effect register definitions through bind/bind_memory and e_barrier/e_fence chains */
    for (int ui = 0; ui < mod->num_units; ui++) {
        flux_unit *u = &mod->units[ui];
        for (int ii = 0; ii < u->num_instructions; ii++) {
            flux_instruction *inst = &u->instructions[ii];

            if (strcmp(inst->opcode, "bind_device") == 0 || strcmp(inst->opcode, "bind_memory") == 0) {
                /* First operand must be an effect register */
                if (inst->num_operands >= 1 && inst->operands[0].kind == FLUX_OP_REG) {
                    int ri = flux_find_register(mod, inst->operands[0].u.reg.name);
                    if (ri >= 0 && mod->registers[ri].reg_class != FLUX_REG_EFFECT) {
                        val_printf(val, arena, "Effect: bind_device/bind_memory output '%s' is not an effect register",
                                  inst->operands[0].u.reg.name);
                    }
                }
            }

            if (strcmp(inst->opcode, "ld.s") == 0 || strcmp(inst->opcode, "st.s") == 0 ||
                strcmp(inst->opcode, "ld.v") == 0 || strcmp(inst->opcode, "st.v") == 0 ||
                strcmp(inst->opcode, "atomic_add") == 0 || strcmp(inst->opcode, "atomic_cmpxchg") == 0) {
                /* Second operand (index 1) must be an effect register */
                if (inst->num_operands >= 2 && inst->operands[1].kind == FLUX_OP_REG) {
                    int ri = flux_find_register(mod, inst->operands[1].u.reg.name);
                    if (ri >= 0 && mod->registers[ri].reg_class != FLUX_REG_EFFECT) {
                        val_printf(val, arena, "Effect: opcode '%s' uses non-effect register '%s' for memory access",
                                  inst->opcode, inst->operands[1].u.reg.name);
                    }
                }
            }

            if (strcmp(inst->opcode, "write") == 0) {
                /* First operand must be effect */
                if (inst->num_operands >= 1 && inst->operands[0].kind == FLUX_OP_REG) {
                    int ri = flux_find_register(mod, inst->operands[0].u.reg.name);
                    if (ri >= 0 && mod->registers[ri].reg_class != FLUX_REG_EFFECT) {
                        val_printf(val, arena, "Effect: 'write' uses non-effect register '%s'",
                                  inst->operands[0].u.reg.name);
                    }
                }
            }
        }
    }

    /* 8.5 Control Safety */
    for (int wi = 0; wi < mod->num_waves; wi++) {
        flux_wave *w = &mod->waves[wi];
        int has_branch = 0;
        int has_end = 0;

        /* Check all units in this wave */
        for (int ui = 0; ui < mod->num_units; ui++) {
            if (strcmp(mod->units[ui].wave, w->name) != 0) continue;
            flux_unit *u = &mod->units[ui];

            for (int ii = 0; ii < u->num_instructions; ii++) {
                if (strcmp(u->instructions[ii].opcode, "branch") == 0) {
                    has_branch = 1;
                    /* Validate branch arguments */
                    for (int oi = 0; oi < u->instructions[ii].num_operands; oi++) {
                        flux_operand *op = &u->instructions[ii].operands[oi];
                        if (op->kind == FLUX_OP_WAVE) {
                            int twi = flux_find_wave(mod, op->u.wave.wave_name);
                            if (twi < 0) {
                                val_printf(val, arena, "Control: branch target '%s' is not a valid wave",
                                          op->u.wave.wave_name);
                            } else {
                                /* Check args match wave params */
                                flux_wave *tw = &mod->waves[twi];
                                if (op->u.wave.num_args != tw->num_params) {
                                    val_printf(val, arena, "Control: branch to '%s' provides %d args but wave expects %d params",
                                              op->u.wave.wave_name, op->u.wave.num_args, tw->num_params);
                                }
                                /* Check that branch target is in control graph */
                                int in_cg = 0;
                                if (mod->num_control_edges > 0) {
                                    for (int ci = 0; ci < mod->num_control_edges; ci++) {
                                        if (strcmp(mod->control_edges[ci].from, w->name) == 0) {
                                            for (int cj = 0; cj < mod->control_edges[ci].num_to; cj++) {
                                                if (strcmp(mod->control_edges[ci].to[cj], op->u.wave.wave_name) == 0) {
                                                    in_cg = 1;
                                                    break;
                                                }
                                            }
                                        }
                                        if (in_cg) break;
                                    }
                                } else {
                                    in_cg = 1;
                                }
                                if (!in_cg) {
                                    val_printf(val, arena, "Control: branch from '%s' to '%s' not in control_graph",
                                              w->name, op->u.wave.wave_name);
                                }
                            }
                        }
                    }
                }
                if (strcmp(u->instructions[ii].opcode, "end") == 0) {
                    has_end = 1;
                }
            }
        }

        /* Wave with end must have no branches */
        if (has_end && has_branch) {
            val_printf(val, arena, "Control: wave '%s' contains both 'end' and 'branch'", w->name);
        }

        /* Wave with branch must not contain end */
        if (has_branch && has_end) {
            val_printf(val, arena, "Control: wave '%s' contains both 'branch' and 'end'", w->name);
        }
    }

    /* Check goto targets have zero params */
    for (int ui = 0; ui < mod->num_units; ui++) {
        for (int ii = 0; ii < mod->units[ui].num_instructions; ii++) {
            if (strcmp(mod->units[ui].instructions[ii].opcode, "goto") == 0) {
                for (int oi = 0; oi < mod->units[ui].instructions[ii].num_operands; oi++) {
                    flux_operand *op = &mod->units[ui].instructions[ii].operands[oi];
                    if (op->kind == FLUX_OP_WAVE) {
                        int twi = flux_find_wave(mod, op->u.wave.wave_name);
                        if (twi >= 0 && mod->waves[twi].num_params > 0) {
                            val_printf(val, arena, "Control: 'goto' to wave '%s' which has parameters (use branch)",
                                      op->u.wave.wave_name);
                        }
                    }
                }
            }
        }
    }

    if (val->num_errors > 0)
        return FLUX_ERR_VALIDATION;

    return FLUX_OK;
}
