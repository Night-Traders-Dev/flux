#include "flux_internal.h"

/* Forward declarations of JSON access helpers */
extern json_value* json_parse(const char *input, flux_arena *arena, flux_error *err);
extern char* json_serialize_value(json_value *v, flux_arena *arena);
extern json_value* json_object_get(json_value *obj, const char *key);
extern json_value* json_array_get(json_value *arr, int index);
extern int json_array_count(json_value *arr);
extern int json_object_count(json_value *obj);
extern const char* json_object_key(json_value *obj, int index);
extern json_value* json_object_value(json_value *obj, int index);
extern const char* json_as_string(json_value *v);
extern double json_as_number(json_value *v);
extern int json_as_bool(json_value *v);
extern int json_is_null(json_value *v);

/*===========================================================================
 * Internal: parse a single operand from JSON
 *===========================================================================*/
static int parse_operand(json_value *jop, flux_operand *op, flux_arena *arena, flux_error *err)
{
    memset(op, 0, sizeof(flux_operand));

    const char *kind = json_as_string(json_object_get(jop, "kind"));
    if (!kind) { flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "operand missing 'kind'"); return 0; }

    if (strcmp(kind, "reg") == 0) {
        op->kind = FLUX_OP_REG;
        const char *name = json_as_string(json_object_get(jop, "name"));
        if (!name) { flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "reg operand missing 'name'"); return 0; }
        op->u.reg.name = flux_arena_strdup(arena, name);
    } else if (strcmp(kind, "imm") == 0) {
        op->kind = FLUX_OP_IMM;
        json_value *jval = json_object_get(jop, "value");
        if (!jval) { flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "imm operand missing 'value'"); return 0; }
        if (jval->kind == JSON_STRING) {
            op->u.imm.str = flux_arena_strdup(arena, jval->u.string);
            op->u.imm.is_int = 0;
        } else if (jval->kind == JSON_NUMBER) {
            double intpart;
            op->u.imm.is_int = (modf(jval->u.number, &intpart) == 0.0);
            op->u.imm.ival = (int64_t)jval->u.number;
            op->u.imm.fval = jval->u.number;
            /* Also set str for generality */
            char buf[64];
            if (op->u.imm.is_int)
                snprintf(buf, sizeof(buf), "%" PRId64, op->u.imm.ival);
            else
                snprintf(buf, sizeof(buf), "%.17g", op->u.imm.fval);
            op->u.imm.str = flux_arena_strdup(arena, buf);
        } else {
            flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "imm value must be string or number");
            return 0;
        }
    } else if (strcmp(kind, "ref") == 0) {
        op->kind = FLUX_OP_REF;
        const char *type = json_as_string(json_object_get(jop, "type"));
        const char *name = json_as_string(json_object_get(jop, "name"));
        if (!type || !name) { flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "ref operand missing 'type' or 'name'"); return 0; }
        op->u.ref.type = flux_arena_strdup(arena, type);
        op->u.ref.name = flux_arena_strdup(arena, name);
    } else if (strcmp(kind, "wave") == 0) {
        op->kind = FLUX_OP_WAVE;
        const char *wname = json_as_string(json_object_get(jop, "name"));
        if (!wname) { flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "wave operand missing 'name'"); return 0; }
        op->u.wave.wave_name = flux_arena_strdup(arena, wname);
        json_value *jargs = json_object_get(jop, "args");
        if (jargs && jargs->kind == JSON_OBJECT) {
            op->u.wave.num_args = jargs->u.object.count;
            op->u.wave.args = (flux_wave_arg*)flux_arena_alloc(arena, op->u.wave.num_args * sizeof(flux_wave_arg));
            if (!op->u.wave.args) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
            for (int i = 0; i < op->u.wave.num_args; i++) {
                op->u.wave.args[i].param_reg = flux_arena_strdup(arena, jargs->u.object.pairs[i].key);
                const char *arg_reg = json_as_string(jargs->u.object.pairs[i].value);
                op->u.wave.args[i].arg_reg = arg_reg ? flux_arena_strdup(arena, arg_reg) : NULL;
            }
        }
    } else {
        flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "unknown operand kind '%s'", kind);
        return 0;
    }
    return 1;
}

/*===========================================================================
 * Internal: parse instructions array
 *===========================================================================*/
static int parse_instructions(json_value *jinsts, flux_instruction **out_insts,
                              int *out_count, flux_arena *arena, flux_error *err)
{
    *out_insts = NULL;
    *out_count = 0;

    if (!jinsts || jinsts->kind != JSON_ARRAY) return 1; /* empty is ok */

    int n = jinsts->u.array.count;
    if (n == 0) return 1;

    *out_insts = (flux_instruction*)flux_arena_alloc(arena, n * sizeof(flux_instruction));
    if (!*out_insts) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
    *out_count = n;

    for (int i = 0; i < n; i++) {
        json_value *jinst = jinsts->u.array.items[i];
        flux_instruction *inst = &(*out_insts)[i];
        memset(inst, 0, sizeof(flux_instruction));

        const char *opcode = json_as_string(json_object_get(jinst, "opcode"));
        if (!opcode) { flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "instruction %d missing 'opcode'", i); return 0; }
        inst->opcode = flux_arena_strdup(arena, opcode);

        json_value *jops = json_object_get(jinst, "operands");
        if (jops && jops->kind == JSON_ARRAY) {
            int nops = jops->u.array.count;
            inst->num_operands = nops;
            inst->operands = (flux_operand*)flux_arena_alloc(arena, nops * sizeof(flux_operand));
            if (!inst->operands && nops > 0) {
                flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory");
                return 0;
            }
            for (int j = 0; j < nops; j++) {
                if (!parse_operand(jops->u.array.items[j], &inst->operands[j], arena, err))
                    return 0;
            }
        }
    }
    return 1;
}

/*===========================================================================
 * Internal: parse uses/defs string arrays
 *===========================================================================*/
static int parse_string_array(json_value *jarr, char ***out, int *out_count, flux_arena *arena, flux_error *err)
{
    *out = NULL;
    *out_count = 0;
    if (!jarr || jarr->kind != JSON_ARRAY) return 1;

    int n = jarr->u.array.count;
    if (n == 0) return 1;

    *out = (char**)flux_arena_alloc(arena, n * sizeof(char*));
    if (!*out) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
    *out_count = n;

    for (int i = 0; i < n; i++) {
        const char *s = json_as_string(jarr->u.array.items[i]);
        if (!s) { flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "expected string in array at %d", i); return 0; }
        (*out)[i] = flux_arena_strdup(arena, s);
    }
    return 1;
}

/*===========================================================================
 * Parse entry points
 *===========================================================================*/
static int parse_entry_points(json_value *jent, flux_entry_point **out, int *out_count,
                               flux_arena *arena, flux_error *err)
{
    *out = NULL;
    *out_count = 0;
    if (!jent || jent->kind != JSON_ARRAY) return 1;

    int n = jent->u.array.count;
    if (n == 0) return 1;

    *out = (flux_entry_point*)flux_arena_alloc(arena, n * sizeof(flux_entry_point));
    if (!*out) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
    memset(*out, 0, n * sizeof(flux_entry_point));
    *out_count = n;

    for (int i = 0; i < n; i++) {
        flux_entry_point *ep = &(*out)[i];
        json_value *jep = jent->u.array.items[i];
        ep->name = flux_arena_strdup(arena, json_as_string(json_object_get(jep, "name")));

        /* Parse params */
        json_value *jparams = json_object_get(jep, "params");
        if (jparams && jparams->kind == JSON_ARRAY) {
            int np = jparams->u.array.count;
            ep->num_params = np;
            ep->param_regs = (char**)flux_arena_alloc(arena, np * sizeof(char*));
            ep->param_types = (flux_type*)flux_arena_alloc(arena, np * sizeof(flux_type));
            if (!ep->param_regs || !ep->param_types) {
                flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0;
            }
            for (int j = 0; j < np; j++) {
                json_value *jp = jparams->u.array.items[j];
                ep->param_regs[j] = flux_arena_strdup(arena, json_as_string(json_object_get(jp, "reg")));
                const char *tname = json_as_string(json_object_get(jp, "type"));
                if (!flux_type_parse(tname, &ep->param_types[j])) {
                    flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "invalid type '%s' in entry param", tname);
                    return 0;
                }
            }
        }

        /* Parse returns */
        json_value *jret = json_object_get(jep, "returns");
        if (jret && jret->kind == JSON_ARRAY) {
            int nr = jret->u.array.count;
            ep->num_returns = nr;
            ep->return_regs = (char**)flux_arena_alloc(arena, nr * sizeof(char*));
            ep->return_types = (flux_type*)flux_arena_alloc(arena, nr * sizeof(flux_type));
            if (!ep->return_regs || !ep->return_types) {
                flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0;
            }
            for (int j = 0; j < nr; j++) {
                json_value *jr = jret->u.array.items[j];
                ep->return_regs[j] = flux_arena_strdup(arena, json_as_string(json_object_get(jr, "reg")));
                const char *tname = json_as_string(json_object_get(jr, "type"));
                if (!flux_type_parse(tname, &ep->return_types[j])) {
                    flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "invalid type '%s' in entry return", tname);
                    return 0;
                }
            }
        }

        ep->start_wave = flux_arena_strdup(arena, json_as_string(json_object_get(jep, "start_wave")));
    }
    return 1;
}

/*===========================================================================
 * Parse a complete FluxIR module from JSON tree
 *===========================================================================*/
static int parse_module_from_json(json_value *jmod, flux_module *mod, flux_error *err)
{
    flux_arena *arena = mod->arena;

    const char *name = json_as_string(json_object_get(jmod, "name"));
    if (name) {
        mod->name = flux_arena_strdup(arena, name);
    } else {
        mod->name = NULL;
    }
    const char *ver = json_as_string(json_object_get(jmod, "version"));
    if (ver) mod->version = flux_arena_strdup(arena, ver);

    /* Entry points */
    if (!parse_entry_points(json_object_get(jmod, "entry_points"),
                            &mod->entry_points, &mod->num_entry_points, arena, err))
        return 0;

    /* Types */
    json_value *jtypes = json_object_get(jmod, "types");
    if (jtypes && jtypes->kind == JSON_ARRAY) {
        int nt = jtypes->u.array.count;
        mod->num_types = nt;
        mod->types = (flux_type*)flux_arena_alloc(arena, nt * sizeof(flux_type));
        mod->type_names = (char**)flux_arena_alloc(arena, nt * sizeof(char*));
        if (!mod->types || !mod->type_names) {
            flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0;
        }
        for (int i = 0; i < nt; i++) {
            json_value *jt = jtypes->u.array.items[i];
            const char *tname = json_as_string(json_object_get(jt, "name"));
            mod->type_names[i] = flux_arena_strdup(arena, tname);
            const char *kind_str = json_as_string(json_object_get(jt, "kind"));
            if (kind_str) {
                if (strcmp(kind_str, "scalar") == 0) {
                    int bits = (int)json_as_number(json_object_get(jt, "bits"));
                    flux_sign sign = FLUX_SIGN_SIGNED;
                    const char *sstr = json_as_string(json_object_get(jt, "sign"));
                    if (sstr && strcmp(sstr, "unsigned") == 0) sign = FLUX_SIGN_UNSIGNED;
                    mod->types[i] = flux_type_scalar(bits, sign, 0);
                } else if (strcmp(kind_str, "predicate") == 0) {
                    mod->types[i] = flux_type_pred();
                } else if (strcmp(kind_str, "effect") == 0) {
                    mod->types[i] = flux_type_effect();
                } else {
                    mod->types[i] = flux_type_scalar(32, FLUX_SIGN_SIGNED, 0);
                }
            } else {
                /* Parse from name */
                flux_type_parse(tname, &mod->types[i]);
            }
        }
    }

    /* Registers */
    json_value *jregs = json_object_get(jmod, "registers");
    if (jregs && jregs->kind == JSON_ARRAY) {
        int nr = jregs->u.array.count;
        mod->num_registers = nr;
        mod->registers = (flux_register*)flux_arena_alloc(arena, nr * sizeof(flux_register));
        if (!mod->registers) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
        for (int i = 0; i < nr; i++) {
            json_value *jr = jregs->u.array.items[i];
            flux_register *r = &mod->registers[i];
            r->name = flux_arena_strdup(arena, json_as_string(json_object_get(jr, "name")));
            const char *cls = json_as_string(json_object_get(jr, "class"));
            if (cls) {
                if (strcmp(cls, "scalar") == 0) r->reg_class = FLUX_REG_SCALAR;
                else if (strcmp(cls, "vector") == 0) r->reg_class = FLUX_REG_VECTOR;
                else if (strcmp(cls, "predicate") == 0) r->reg_class = FLUX_REG_PREDICATE;
                else if (strcmp(cls, "effect") == 0) r->reg_class = FLUX_REG_EFFECT;
            }
            const char *tname = json_as_string(json_object_get(jr, "type"));
            if (tname) {
                if (!flux_type_parse(tname, &r->type)) {
                    /* Try looking up in module types */
                    int ti = flux_find_type(mod, tname);
                    if (ti >= 0) r->type = mod->types[ti];
                    else r->type = flux_type_scalar(32, FLUX_SIGN_SIGNED, 0);
                }
            }
        }
    }

    /* Devices */
    json_value *jdevs = json_object_get(jmod, "devices");
    if (jdevs && jdevs->kind == JSON_ARRAY) {
        int nd = jdevs->u.array.count;
        mod->num_devices = nd;
        mod->devices = (flux_device*)flux_arena_alloc(arena, nd * sizeof(flux_device));
        if (!mod->devices) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
        memset(mod->devices, 0, nd * sizeof(flux_device));
        for (int i = 0; i < nd; i++) {
            json_value *jd = jdevs->u.array.items[i];
            flux_device *d = &mod->devices[i];
            d->name = flux_arena_strdup(arena, json_as_string(json_object_get(jd, "name")));
            d->kind = flux_arena_strdup(arena, json_as_string(json_object_get(jd, "kind")));
            json_value *jprops = json_object_get(jd, "properties");
            if (jprops && jprops->kind == JSON_OBJECT) {
                d->num_props = jprops->u.object.count;
                d->prop_keys = (char**)flux_arena_alloc(arena, d->num_props * sizeof(char*));
                d->prop_values = (char**)flux_arena_alloc(arena, d->num_props * sizeof(char*));
                if (d->prop_keys && d->prop_values) {
                    for (int j = 0; j < d->num_props; j++) {
                        d->prop_keys[j] = flux_arena_strdup(arena, jprops->u.object.pairs[j].key);
                        d->prop_values[j] = flux_arena_strdup(arena,
                            json_as_string(jprops->u.object.pairs[j].value));
                    }
                }
            }
        }
    }

    /* Memory regions */
    json_value *jmems = json_object_get(jmod, "memory_regions");
    if (jmems && jmems->kind == JSON_ARRAY) {
        int nm = jmems->u.array.count;
        mod->num_memory_regions = nm;
        mod->memory_regions = (flux_memory_region*)flux_arena_alloc(arena, nm * sizeof(flux_memory_region));
        if (!mod->memory_regions) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
        memset(mod->memory_regions, 0, nm * sizeof(flux_memory_region));
        for (int i = 0; i < nm; i++) {
            json_value *jm = jmems->u.array.items[i];
            flux_memory_region *mr = &mod->memory_regions[i];
            mr->name = flux_arena_strdup(arena, json_as_string(json_object_get(jm, "name")));
            mr->kind = flux_arena_strdup(arena, json_as_string(json_object_get(jm, "kind")));
            mr->size = (int)json_as_number(json_object_get(jm, "size"));
            const char *attrs = json_as_string(json_object_get(jm, "attributes"));
            if (!attrs) {
                /* Try JSON object */
                json_value *jattrs = json_object_get(jm, "attributes");
                if (jattrs) attrs = json_serialize_value(jattrs, arena);
            }
            if (attrs) mr->attributes = flux_arena_strdup(arena, attrs);
        }
    }

    /* Waves */
    json_value *jwaves = json_object_get(jmod, "waves");
    if (jwaves && jwaves->kind == JSON_ARRAY) {
        int nw = jwaves->u.array.count;
        mod->num_waves = nw;
        mod->waves = (flux_wave*)flux_arena_alloc(arena, nw * sizeof(flux_wave));
        if (!mod->waves) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
        memset(mod->waves, 0, nw * sizeof(flux_wave));
        for (int i = 0; i < nw; i++) {
            json_value *jw = jwaves->u.array.items[i];
            flux_wave *w = &mod->waves[i];
            w->name = flux_arena_strdup(arena, json_as_string(json_object_get(jw, "name")));

            /* Parameters */
            json_value *jparams = json_object_get(jw, "params");
            if (jparams && jparams->kind == JSON_ARRAY) {
                int np = jparams->u.array.count;
                w->num_params = np;
                w->params = (flux_wave_param*)flux_arena_alloc(arena, np * sizeof(flux_wave_param));
                if (!w->params) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
                memset(w->params, 0, np * sizeof(flux_wave_param));
                for (int j = 0; j < np; j++) {
                    json_value *jp = jparams->u.array.items[j];
                    w->params[j].reg = flux_arena_strdup(arena, json_as_string(json_object_get(jp, "reg")));
                    const char *tname = json_as_string(json_object_get(jp, "type"));
                    if (tname && !flux_type_parse(tname, &w->params[j].type)) {
                        int ti = flux_find_type(mod, tname);
                        if (ti >= 0) w->params[j].type = mod->types[ti];
                    }
                }
            }

            /* Units */
            if (!parse_string_array(json_object_get(jw, "units"), &w->units, &w->num_units, arena, err))
                return 0;
        }
    }

    /* Units */
    json_value *junits = json_object_get(jmod, "units");
    if (junits && junits->kind == JSON_ARRAY) {
        int nu = junits->u.array.count;
        mod->num_units = nu;
        mod->units = (flux_unit*)flux_arena_alloc(arena, nu * sizeof(flux_unit));
        if (!mod->units) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
        memset(mod->units, 0, nu * sizeof(flux_unit));
        for (int i = 0; i < nu; i++) {
            json_value *ju = junits->u.array.items[i];
            flux_unit *u = &mod->units[i];
            u->name = flux_arena_strdup(arena, json_as_string(json_object_get(ju, "name")));
            u->wave = flux_arena_strdup(arena, json_as_string(json_object_get(ju, "wave")));

            if (!parse_string_array(json_object_get(ju, "uses"), &u->uses, &u->num_uses, arena, err))
                return 0;
            if (!parse_string_array(json_object_get(ju, "defs"), &u->defs, &u->num_defs, arena, err))
                return 0;

            u->latency = (int)json_as_number(json_object_get(ju, "latency"));
            const char *pred = json_as_string(json_object_get(ju, "predicate"));
            if (pred) u->predicate = flux_arena_strdup(arena, pred);

            if (!parse_instructions(json_object_get(ju, "instructions"),
                                    &u->instructions, &u->num_instructions, arena, err))
                return 0;
        }
    }

    /* Dependencies */
    json_value *jdeps = json_object_get(jmod, "deps");
    if (jdeps && jdeps->kind == JSON_ARRAY) {
        int nd = jdeps->u.array.count;
        mod->num_deps = nd;
        mod->deps = (flux_dep*)flux_arena_alloc(arena, nd * sizeof(flux_dep));
        if (!mod->deps) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
        memset(mod->deps, 0, nd * sizeof(flux_dep));
        for (int i = 0; i < nd; i++) {
            json_value *jd = jdeps->u.array.items[i];
            flux_dep *d = &mod->deps[i];
            d->from = flux_arena_strdup(arena, json_as_string(json_object_get(jd, "from")));
            d->to = flux_arena_strdup(arena, json_as_string(json_object_get(jd, "to")));
            if (!parse_string_array(json_object_get(jd, "regs"), &d->regs, &d->num_regs, arena, err))
                return 0;
        }
    }

    /* Control graph */
    json_value *jcg = json_object_get(jmod, "control_graph");
    if (jcg && jcg->kind == JSON_ARRAY) {
        int nc = jcg->u.array.count;
        mod->num_control_edges = nc;
        mod->control_edges = (flux_control_edge*)flux_arena_alloc(arena, nc * sizeof(flux_control_edge));
        if (!mod->control_edges) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return 0; }
        memset(mod->control_edges, 0, nc * sizeof(flux_control_edge));
        for (int i = 0; i < nc; i++) {
            json_value *je = jcg->u.array.items[i];
            flux_control_edge *e = &mod->control_edges[i];
            e->from = flux_arena_strdup(arena, json_as_string(json_object_get(je, "from")));
            if (!parse_string_array(json_object_get(je, "to"), &e->to, &e->num_to, arena, err))
                return 0;
        }
    }

    return 1;
}

/*===========================================================================
 * Public API: parse FluxIR from JSON text
 *===========================================================================*/
flux_status flux_ir_parse(const char *json_text, flux_module *mod, flux_error *err)
{
    if (!json_text || !mod) {
        flux_error_set(err, FLUX_ERR_INTERNAL, 0, 0, "null argument");
        return FLUX_ERR_INTERNAL;
    }

    flux_error_init(err);

    json_value *root = json_parse(json_text, mod->arena, err);
    if (!root) {
        if (err->status == FLUX_OK) err->status = FLUX_ERR_PARSE;
        return err->status;
    }

    /* Expect { "module": { ... } } */
    json_value *jmod = json_object_get(root, "module");
    if (!jmod) {
        flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "missing top-level 'module' key");
        return FLUX_ERR_PARSE;
    }

    if (!parse_module_from_json(jmod, mod, err))
        return FLUX_ERR_PARSE;

    return FLUX_OK;
}

/*===========================================================================
 * Public API: parse FluxIR from file
 *===========================================================================*/
flux_status flux_ir_parse_file(const char *path, flux_module *mod, flux_error *err)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        flux_error_set(err, FLUX_ERR_PARSE, 0, 0, "cannot open file '%s'", path);
        return FLUX_ERR_PARSE;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char*)flux_arena_alloc(mod->arena, (size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory");
        return FLUX_ERR_OOM;
    }

    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[nread] = '\0';

    return flux_ir_parse(buf, mod, err);
}

/*===========================================================================
 * Serializer helpers - build JSON tree from module
 *===========================================================================*/
static json_value* make_json_str(flux_arena *a, const char *s)
{
    json_value *v = (json_value*)flux_arena_alloc(a, sizeof(json_value));
    if (!v) return NULL;
    v->kind = JSON_STRING;
    v->u.string = s ? (char*)s : "";
    return v;
}

static json_value* make_json_num(flux_arena *a, double n)
{
    json_value *v = (json_value*)flux_arena_alloc(a, sizeof(json_value));
    if (!v) return NULL;
    v->kind = JSON_NUMBER;
    v->u.number = n;
    return v;
}

static json_value* make_json_obj(flux_arena *a)
{
    json_value *v = (json_value*)flux_arena_alloc(a, sizeof(json_value));
    if (!v) return NULL;
    v->kind = JSON_OBJECT;
    v->u.object.pairs = NULL;
    v->u.object.count = 0;
    return v;
}

static void json_obj_add(json_value *obj, flux_arena *a, const char *key, json_value *val)
{
    if (!obj || !val) return;
    int idx = obj->u.object.count++;
    /* Reallocate pairs array */
    json_pair *new_pairs = (json_pair*)flux_arena_alloc(a, obj->u.object.count * sizeof(json_pair));
    if (obj->u.object.pairs && obj->u.object.count > 1) {
        memcpy(new_pairs, obj->u.object.pairs, (obj->u.object.count - 1) * sizeof(json_pair));
    }
    obj->u.object.pairs = new_pairs;
    obj->u.object.pairs[idx].key = (char*)key;
    obj->u.object.pairs[idx].value = val;
}

static json_value* make_json_arr(flux_arena *a)
{
    json_value *v = (json_value*)flux_arena_alloc(a, sizeof(json_value));
    if (!v) return NULL;
    v->kind = JSON_ARRAY;
    v->u.array.items = NULL;
    v->u.array.count = 0;
    return v;
}

static void json_arr_add(json_value *arr, flux_arena *a, json_value *val)
{
    if (!arr || !val) return;
    int idx = arr->u.array.count++;
    json_value **new_items = (json_value**)flux_arena_alloc(a, arr->u.array.count * sizeof(json_value*));
    if (arr->u.array.items && arr->u.array.count > 1) {
        memcpy(new_items, arr->u.array.items, (arr->u.array.count - 1) * sizeof(json_value*));
    }
    arr->u.array.items = new_items;
    arr->u.array.items[idx] = val;
}

/*===========================================================================
 * Serialize a flux_module to JSON tree
 *===========================================================================*/
static json_value* serialize_module_to_json(const flux_module *mod, flux_arena *arena)
{
    json_value *jmod = make_json_obj(arena);

    json_obj_add(jmod, arena, "name", make_json_str(arena, mod->name));
    json_obj_add(jmod, arena, "version", make_json_str(arena, mod->version));

    /* Entry points */
    json_value *jent = make_json_arr(arena);
    for (int i = 0; i < mod->num_entry_points; i++) {
        json_value *jep = make_json_obj(arena);
        flux_entry_point *ep = &mod->entry_points[i];
        json_obj_add(jep, arena, "name", make_json_str(arena, ep->name));

        json_value *jparams = make_json_arr(arena);
        for (int j = 0; j < ep->num_params; j++) {
            json_value *jp = make_json_obj(arena);
            json_obj_add(jp, arena, "reg", make_json_str(arena, ep->param_regs[j]));
            char tbuf[32];
            flux_type_format(&ep->param_types[j], tbuf, sizeof(tbuf));
            json_obj_add(jp, arena, "type", make_json_str(arena, flux_arena_strdup(arena, tbuf)));
            json_arr_add(jparams, arena, jp);
        }
        json_obj_add(jep, arena, "params", jparams);

        json_value *jret = make_json_arr(arena);
        for (int j = 0; j < ep->num_returns; j++) {
            json_value *jr = make_json_obj(arena);
            json_obj_add(jr, arena, "reg", make_json_str(arena, ep->return_regs[j]));
            char tbuf[32];
            flux_type_format(&ep->return_types[j], tbuf, sizeof(tbuf));
            json_obj_add(jr, arena, "type", make_json_str(arena, flux_arena_strdup(arena, tbuf)));
            json_arr_add(jret, arena, jr);
        }
        json_obj_add(jep, arena, "returns", jret);
        json_obj_add(jep, arena, "start_wave", make_json_str(arena, ep->start_wave));
        json_arr_add(jent, arena, jep);
    }
    json_obj_add(jmod, arena, "entry_points", jent);

    /* Types */
    json_value *jtypes = make_json_arr(arena);
    for (int i = 0; i < mod->num_types; i++) {
        json_value *jt = make_json_obj(arena);
        json_obj_add(jt, arena, "name", make_json_str(arena, mod->type_names[i]));
        switch (mod->types[i].kind) {
        case FLUX_KIND_SCALAR:
            json_obj_add(jt, arena, "kind", make_json_str(arena, "scalar"));
            json_obj_add(jt, arena, "bits", make_json_num(arena, mod->types[i].bits));
            json_obj_add(jt, arena, "sign", make_json_str(arena,
                mod->types[i].sign == FLUX_SIGN_UNSIGNED ? "unsigned" : "signed"));
            break;
        case FLUX_KIND_VECTOR:
            json_obj_add(jt, arena, "kind", make_json_str(arena, "vector"));
            json_obj_add(jt, arena, "lanes", make_json_num(arena, mod->types[i].lanes));
            json_obj_add(jt, arena, "element_type",
                make_json_str(arena, flux_arena_strdup(arena, mod->type_names[i])));
            break;
        case FLUX_KIND_PREDICATE:
            json_obj_add(jt, arena, "kind", make_json_str(arena, "predicate"));
            break;
        case FLUX_KIND_EFFECT:
            json_obj_add(jt, arena, "kind", make_json_str(arena, "effect"));
            break;
        }
        json_arr_add(jtypes, arena, jt);
    }
    json_obj_add(jmod, arena, "types", jtypes);

    /* Registers */
    json_value *jregs = make_json_arr(arena);
    for (int i = 0; i < mod->num_registers; i++) {
        json_value *jr = make_json_obj(arena);
        flux_register *r = &mod->registers[i];
        json_obj_add(jr, arena, "name", make_json_str(arena, r->name));
        const char *cls = "scalar";
        switch (r->reg_class) {
        case FLUX_REG_SCALAR: cls = "scalar"; break;
        case FLUX_REG_VECTOR: cls = "vector"; break;
        case FLUX_REG_PREDICATE: cls = "predicate"; break;
        case FLUX_REG_EFFECT: cls = "effect"; break;
        }
        json_obj_add(jr, arena, "class", make_json_str(arena, cls));
        char tbuf[32];
        flux_type_format(&r->type, tbuf, sizeof(tbuf));
        json_obj_add(jr, arena, "type", make_json_str(arena, flux_arena_strdup(arena, tbuf)));
        json_arr_add(jregs, arena, jr);
    }
    json_obj_add(jmod, arena, "registers", jregs);

    /* Devices */
    json_value *jdevs = make_json_arr(arena);
    for (int i = 0; i < mod->num_devices; i++) {
        json_value *jd = make_json_obj(arena);
        json_obj_add(jd, arena, "name", make_json_str(arena, mod->devices[i].name));
        json_obj_add(jd, arena, "kind", make_json_str(arena, mod->devices[i].kind));
        json_value *jprops = make_json_obj(arena);
        for (int j = 0; j < mod->devices[i].num_props; j++) {
            json_obj_add(jprops, arena, mod->devices[i].prop_keys[j],
                         make_json_str(arena, mod->devices[i].prop_values[j]));
        }
        json_obj_add(jd, arena, "properties", jprops);
        json_arr_add(jdevs, arena, jd);
    }
    json_obj_add(jmod, arena, "devices", jdevs);

    /* Memory Regions */
    json_value *jmems = make_json_arr(arena);
    for (int i = 0; i < mod->num_memory_regions; i++) {
        json_value *jm = make_json_obj(arena);
        json_obj_add(jm, arena, "name", make_json_str(arena, mod->memory_regions[i].name));
        json_obj_add(jm, arena, "kind", make_json_str(arena, mod->memory_regions[i].kind));
        json_obj_add(jm, arena, "size", make_json_num(arena, mod->memory_regions[i].size));
        if (mod->memory_regions[i].attributes) {
            json_obj_add(jm, arena, "attributes", make_json_str(arena, mod->memory_regions[i].attributes));
        } else {
            json_obj_add(jm, arena, "attributes", make_json_obj(arena));
        }
        json_arr_add(jmems, arena, jm);
    }
    json_obj_add(jmod, arena, "memory_regions", jmems);

    /* Waves */
    json_value *jwaves = make_json_arr(arena);
    for (int i = 0; i < mod->num_waves; i++) {
        json_value *jw = make_json_obj(arena);
        flux_wave *w = &mod->waves[i];
        json_obj_add(jw, arena, "name", make_json_str(arena, w->name));

        json_value *jparams = make_json_arr(arena);
        for (int j = 0; j < w->num_params; j++) {
            json_value *jp = make_json_obj(arena);
            json_obj_add(jp, arena, "reg", make_json_str(arena, w->params[j].reg));
            char tbuf[32];
            flux_type_format(&w->params[j].type, tbuf, sizeof(tbuf));
            json_obj_add(jp, arena, "type", make_json_str(arena, flux_arena_strdup(arena, tbuf)));
            json_arr_add(jparams, arena, jp);
        }
        json_obj_add(jw, arena, "params", jparams);

        json_value *junits = make_json_arr(arena);
        for (int j = 0; j < w->num_units; j++) {
            json_arr_add(junits, arena, make_json_str(arena, w->units[j]));
        }
        json_obj_add(jw, arena, "units", junits);
        json_arr_add(jwaves, arena, jw);
    }
    json_obj_add(jmod, arena, "waves", jwaves);

    /* Units */
    json_value *junits = make_json_arr(arena);
    for (int i = 0; i < mod->num_units; i++) {
        json_value *ju = make_json_obj(arena);
        flux_unit *u = &mod->units[i];
        json_obj_add(ju, arena, "name", make_json_str(arena, u->name));
        json_obj_add(ju, arena, "wave", make_json_str(arena, u->wave));

        json_value *juses = make_json_arr(arena);
        for (int j = 0; j < u->num_uses; j++)
            json_arr_add(juses, arena, make_json_str(arena, u->uses[j]));
        json_obj_add(ju, arena, "uses", juses);

        json_value *jdefs = make_json_arr(arena);
        for (int j = 0; j < u->num_defs; j++)
            json_arr_add(jdefs, arena, make_json_str(arena, u->defs[j]));
        json_obj_add(ju, arena, "defs", jdefs);

        if (u->latency > 0)
            json_obj_add(ju, arena, "latency", make_json_num(arena, u->latency));
        if (u->predicate)
            json_obj_add(ju, arena, "predicate", make_json_str(arena, u->predicate));

        json_value *jinsts = make_json_arr(arena);
        for (int j = 0; j < u->num_instructions; j++) {
            json_value *ji = make_json_obj(arena);
            json_obj_add(ji, arena, "opcode", make_json_str(arena, u->instructions[j].opcode));

            json_value *jops = make_json_arr(arena);
            for (int k = 0; k < u->instructions[j].num_operands; k++) {
                flux_operand *op = &u->instructions[j].operands[k];
                json_value *jo = make_json_obj(arena);
                switch (op->kind) {
                case FLUX_OP_REG:
                    json_obj_add(jo, arena, "kind", make_json_str(arena, "reg"));
                    json_obj_add(jo, arena, "name", make_json_str(arena, op->u.reg.name));
                    break;
                case FLUX_OP_IMM:
                    json_obj_add(jo, arena, "kind", make_json_str(arena, "imm"));
                    if (op->u.imm.is_int)
                        json_obj_add(jo, arena, "value", make_json_num(arena, (double)op->u.imm.ival));
                    else if (op->u.imm.str) {
                        /* Try to parse as number */
                        char *end;
                        double dval = strtod(op->u.imm.str, &end);
                        if (*end == '\0')
                            json_obj_add(jo, arena, "value", make_json_num(arena, dval));
                        else
                            json_obj_add(jo, arena, "value", make_json_str(arena, op->u.imm.str));
                    }
                    break;
                case FLUX_OP_REF:
                    json_obj_add(jo, arena, "kind", make_json_str(arena, "ref"));
                    json_obj_add(jo, arena, "type", make_json_str(arena, op->u.ref.type));
                    json_obj_add(jo, arena, "name", make_json_str(arena, op->u.ref.name));
                    break;
                case FLUX_OP_WAVE: {
                    json_obj_add(jo, arena, "kind", make_json_str(arena, "wave"));
                    json_obj_add(jo, arena, "name", make_json_str(arena, op->u.wave.wave_name));
                    json_value *jargs = make_json_obj(arena);
                    for (int a = 0; a < op->u.wave.num_args; a++) {
                        json_obj_add(jargs, arena, op->u.wave.args[a].param_reg,
                                     make_json_str(arena, op->u.wave.args[a].arg_reg));
                    }
                    json_obj_add(jo, arena, "args", jargs);
                    break;
                }
                }
                json_arr_add(jops, arena, jo);
            }
            json_obj_add(ji, arena, "operands", jops);
            json_arr_add(jinsts, arena, ji);
        }
        json_obj_add(ju, arena, "instructions", jinsts);
        json_arr_add(junits, arena, ju);
    }
    json_obj_add(jmod, arena, "units", junits);

    /* Dependencies */
    json_value *jdeps = make_json_arr(arena);
    for (int i = 0; i < mod->num_deps; i++) {
        json_value *jd = make_json_obj(arena);
        json_obj_add(jd, arena, "from", make_json_str(arena, mod->deps[i].from));
        json_obj_add(jd, arena, "to", make_json_str(arena, mod->deps[i].to));
        json_value *jregs = make_json_arr(arena);
        for (int j = 0; j < mod->deps[i].num_regs; j++)
            json_arr_add(jregs, arena, make_json_str(arena, mod->deps[i].regs[j]));
        json_obj_add(jd, arena, "regs", jregs);
        json_arr_add(jdeps, arena, jd);
    }
    json_obj_add(jmod, arena, "deps", jdeps);

    /* Control graph */
    json_value *jcg = make_json_arr(arena);
    for (int i = 0; i < mod->num_control_edges; i++) {
        json_value *je = make_json_obj(arena);
        json_obj_add(je, arena, "from", make_json_str(arena, mod->control_edges[i].from));
        json_value *jto = make_json_arr(arena);
        for (int j = 0; j < mod->control_edges[i].num_to; j++)
            json_arr_add(jto, arena, make_json_str(arena, mod->control_edges[i].to[j]));
        json_obj_add(je, arena, "to", jto);
        json_arr_add(jcg, arena, je);
    }
    json_obj_add(jmod, arena, "control_graph", jcg);

    /* Wrap in top-level { "module": ... } */
    json_value *root = make_json_obj(arena);
    json_obj_add(root, arena, "module", jmod);

    return root;
}

/*===========================================================================
 * Public API: serialize module to JSON string
 *===========================================================================*/
char* flux_ir_serialize(const flux_module *mod, flux_error *err)
{
    if (!mod) {
        flux_error_set(err, FLUX_ERR_INTERNAL, 0, 0, "null module");
        return NULL;
    }
    flux_error_init(err);

    /* Use a temporary arena for JSON tree */
    flux_arena *tmp = flux_arena_create(65536);
    if (!tmp) {
        flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory");
        return NULL;
    }

    json_value *root = serialize_module_to_json(mod, tmp);
    if (!root) {
        flux_arena_destroy(tmp);
        flux_error_set(err, FLUX_ERR_INTERNAL, 0, 0, "serialization failed");
        return NULL;
    }

    char *result = json_serialize_value(root, tmp);

    /* Copy to module's arena for lifetime safety */
    char *perm = flux_arena_strdup(mod->arena, result);
    flux_arena_destroy(tmp);
    return perm;
}
