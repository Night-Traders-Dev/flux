#include "flux_internal.h"

/*===========================================================================
 * FluxASM Parser
 * A line-oriented parser for FluxASM human-readable assembly.
 *===========================================================================*/

typedef struct {
    const char *input;
    const char *pos;
    int         line;
    int         column;
    flux_arena *arena;
    flux_error *error;
    flux_module *mod;

    /* Current parsing context */
    flux_wave *current_wave;
    flux_unit *current_unit;
} asm_parser;

static void asm_skip_ws(asm_parser *p)
{
    while (*p->pos) {
        char c = *p->pos;
        if (c == ' ' || c == '\t') { p->pos++; p->column++; }
        else break;
    }
}

static void asm_skip_line(asm_parser *p)
{
    while (*p->pos && *p->pos != '\n') {
        if (*p->pos == '\r') { p->pos++; continue; }
        p->pos++;
        p->column++;
    }
    if (*p->pos == '\n') {
        p->pos++;
        p->line++;
        p->column = 1;
    }
}

static void asm_error(asm_parser *p, const char *fmt, ...)
{
    if (p->error) {
        p->error->line = p->line;
        p->error->column = p->column;
        p->error->status = FLUX_ERR_PARSE;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(p->error->message, sizeof(p->error->message), fmt, ap);
        va_end(ap);
    }
}

static char* asm_parse_ident(asm_parser *p)
{
    asm_skip_ws(p);
    const char *start = p->pos;
    if (!*p->pos || (!isalpha((unsigned char)*p->pos) && *p->pos != '_' && *p->pos != '.')) {
        return NULL;
    }
    while (*p->pos && (isalnum((unsigned char)*p->pos) || *p->pos == '_' || *p->pos == '.')) {
        p->pos++;
        p->column++;
    }
    size_t len = (size_t)(p->pos - start);
    if (len == 0) return NULL;
    return flux_arena_strndup(p->arena, start, len);
}

static char* asm_parse_string(asm_parser *p)
{
    asm_skip_ws(p);
    if (*p->pos != '"') return NULL;
    p->pos++; p->column++;
    const char *start = p->pos;
    size_t len = 0;
    while (*p->pos && *p->pos != '"') {
        if (*p->pos == '\\') { p->pos++; if (*p->pos) p->pos++; }
        else p->pos++;
        len++;
        p->column++;
    }
    if (*p->pos != '"') { asm_error(p, "unterminated string"); return NULL; }
    p->pos++; p->column++;

    /* Now copy with escape processing */
    char *result = (char*)flux_arena_alloc(p->arena, len + 1);
    if (!result) return NULL;
    const char *src = start;
    size_t i = 0;
    while ((size_t)(src - start) < (size_t)(p->pos - 1 - start)) {
        if (*src == '\\') {
            src++;
            switch (*src) {
            case 'n': result[i++] = '\n'; break;
            case 't': result[i++] = '\t'; break;
            case '"': result[i++] = '"'; break;
            case '\\': result[i++] = '\\'; break;
            default: result[i++] = *src; break;
            }
            src++;
        } else {
            result[i++] = *src++;
        }
    }
    result[i] = '\0';
    return result;
}

static int asm_expect(asm_parser *p, char c)
{
    asm_skip_ws(p);
    if (*p->pos == c) {
        p->pos++;
        p->column++;
        return 1;
    }
    return 0;
}

/* Parse a type reference (e.g., "i32", "pred", "effect") */
static int asm_parse_type_ref(asm_parser *p, flux_type *out)
{
    asm_skip_ws(p);
    const char *start = p->pos;
    int len = 0;
    while (*p->pos && (isalnum((unsigned char)*p->pos) || *p->pos == '<' || *p->pos == '>' ||
                       *p->pos == 'v' || *p->pos == '_')) {
        p->pos++;
        p->column++;
        len++;
    }
    if (len == 0) return 0;
    char buf[64];
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    p->pos = start + len;
    return flux_type_parse(buf, out);
}

/* Find or create register in module */
static int asm_find_or_add_reg(asm_parser *p, const char *name, flux_type *type)
{
    int idx = flux_find_register(p->mod, name);
    if (idx >= 0) return idx;

    /* Add new register */
    int nr = p->mod->num_registers++;
    flux_register *new_regs = (flux_register*)flux_arena_alloc(p->arena,
                                p->mod->num_registers * sizeof(flux_register));
    if (p->mod->registers) {
        memcpy(new_regs, p->mod->registers, nr * sizeof(flux_register));
    }
    p->mod->registers = new_regs;
    p->mod->registers[nr].name = flux_arena_strdup(p->arena, name);
    p->mod->registers[nr].type = type ? *type : flux_type_scalar(32, FLUX_SIGN_SIGNED, 0);

    /* Determine class from name prefix */
    if (name[0] == 's') p->mod->registers[nr].reg_class = FLUX_REG_SCALAR;
    else if (name[0] == 'v') p->mod->registers[nr].reg_class = FLUX_REG_VECTOR;
    else if (name[0] == 'p') p->mod->registers[nr].reg_class = FLUX_REG_PREDICATE;
    else if (name[0] == 'e') p->mod->registers[nr].reg_class = FLUX_REG_EFFECT;
    else p->mod->registers[nr].reg_class = FLUX_REG_SCALAR;

    return nr;
}

/*===========================================================================
 * Parse an operand from assembly text
 *===========================================================================*/
static int asm_parse_operand(asm_parser *p, flux_operand *op)
{
    memset(op, 0, sizeof(flux_operand));
    asm_skip_ws(p);

    /* String literal */
    if (*p->pos == '"') {
        op->kind = FLUX_OP_IMM;
        op->u.imm.str = asm_parse_string(p);
        op->u.imm.is_int = 0;
        return 1;
    }

    /* Check for immediate number (starts with digit, -, or +) */
    if (isdigit((unsigned char)*p->pos) || *p->pos == '-' || *p->pos == '+') {
        const char *start = p->pos;
        if (*p->pos == '-' || *p->pos == '+') { p->pos++; p->column++; }
        int is_float = 0;
        while (isdigit((unsigned char)*p->pos)) { p->pos++; p->column++; }
        if (*p->pos == '.') { is_float = 1; p->pos++; p->column++; while (isdigit((unsigned char)*p->pos)) { p->pos++; p->column++; } }
        if (*p->pos == 'e' || *p->pos == 'E') {
            is_float = 1; p->pos++; p->column++;
            if (*p->pos == '+' || *p->pos == '-') { p->pos++; p->column++; }
            while (isdigit((unsigned char)*p->pos)) { p->pos++; p->column++; }
        }
        size_t len = (size_t)(p->pos - start);
        char buf[64];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, start, len);
        buf[len] = '\0';

        op->kind = FLUX_OP_IMM;
        op->u.imm.str = flux_arena_strdup(p->arena, buf);
        if (is_float) {
            op->u.imm.is_int = 0;
            op->u.imm.fval = atof(buf);
        } else {
            op->u.imm.is_int = 1;
            op->u.imm.ival = (int64_t)atoll(buf);
            op->u.imm.fval = (double)op->u.imm.ival;
        }
        return 1;
    }

    /* Identifier - could be register, or keyword */
    const char *start = p->pos;
    while (*p->pos && (isalnum((unsigned char)*p->pos) || *p->pos == '_' || *p->pos == '.')) {
        p->pos++;
        p->column++;
    }
    size_t len = (size_t)(p->pos - start);
    if (len == 0) { asm_error(p, "expected operand"); return 0; }

    char *ident = flux_arena_strndup(p->arena, start, len);

    /* Check if it starts with register prefix */
    if (ident[0] == 's' || ident[0] == 'v' || ident[0] == 'p' || ident[0] == 'e') {
        op->kind = FLUX_OP_REG;
        op->u.reg.name = ident;
        /* Ensure register exists in module */
        asm_find_or_add_reg(p, ident, NULL);
        return 1;
    }

    /* Otherwise treat as immediate string */
    op->kind = FLUX_OP_IMM;
    op->u.imm.str = ident;
    op->u.imm.is_int = 0;
    return 1;
}

/*===========================================================================
 * Parse a .wave directive
 * .wave Wname (param1:type1, param2:type2)
 *===========================================================================*/
static int asm_parse_wave(asm_parser *p)
{
    asm_skip_ws(p);
    char *name = asm_parse_ident(p);
    if (!name) { asm_error(p, "expected wave name"); return 0; }

    int wi = p->mod->num_waves++;
    flux_wave *new_waves = (flux_wave*)flux_arena_alloc(p->arena,
                            p->mod->num_waves * sizeof(flux_wave));
    if (p->mod->waves) {
        memcpy(new_waves, p->mod->waves, wi * sizeof(flux_wave));
    }
    p->mod->waves = new_waves;
    flux_wave *w = &p->mod->waves[wi];
    memset(w, 0, sizeof(flux_wave));
    w->name = name;

    p->current_wave = w;

    /* Parse optional parameters */
    asm_skip_ws(p);
    if (*p->pos == '(') {
        p->pos++; p->column++;
        asm_skip_ws(p);
        if (*p->pos != ')') {
            int np = 0;
            flux_wave_param params[16];
            while (1) {
                char *pname = asm_parse_ident(p);
                if (!pname) { asm_error(p, "expected parameter name"); return 0; }
                if (!asm_expect(p, ':')) { asm_error(p, "expected ':' after parameter name"); return 0; }
                flux_type pt;
                if (!asm_parse_type_ref(p, &pt)) { asm_error(p, "expected parameter type"); return 0; }
                params[np].reg = pname;
                params[np].type = pt;
                np++;

                /* Add register if not exists */
                asm_find_or_add_reg(p, pname, &pt);

                asm_skip_ws(p);
                if (*p->pos == ',') { p->pos++; p->column++; asm_skip_ws(p); }
                else break;
            }
            w->num_params = np;
            w->params = (flux_wave_param*)flux_arena_alloc(p->arena, np * sizeof(flux_wave_param));
            if (w->params) memcpy(w->params, params, np * sizeof(flux_wave_param));
        }
        if (!asm_expect(p, ')')) { asm_error(p, "expected ')'"); return 0; }
    }
    return 1;
}

/*===========================================================================
 * Parse instruction line
 *===========================================================================*/
static int asm_parse_instruction(asm_parser *p, flux_instruction *inst)
{
    memset(inst, 0, sizeof(flux_instruction));
    asm_skip_ws(p);

    /* Check for end of line/comment */
    if (*p->pos == '\n' || *p->pos == '#' || *p->pos == ';' || !*p->pos)
        return 0;

    char *opcode = asm_parse_ident(p);
    if (!opcode) { asm_error(p, "expected opcode"); return 0; }
    inst->opcode = opcode;

    /* Parse comma-separated operands */
    int cap = 8;
    int count = 0;
    flux_operand *ops = (flux_operand*)flux_arena_alloc(p->arena, cap * sizeof(flux_operand));
    if (!ops) { asm_error(p, "out of memory"); return 0; }

    asm_skip_ws(p);
    while (*p->pos && *p->pos != '\n' && *p->pos != '#' && *p->pos != ';') {
        if (count >= cap) {
            cap *= 2;
            /* Just skip if too many operands */
            break;
        }
        if (!asm_parse_operand(p, &ops[count])) break;
        count++;
        asm_skip_ws(p);
        if (*p->pos == ',') { p->pos++; p->column++; asm_skip_ws(p); }
        else break;
    }

    inst->num_operands = count;
    inst->operands = ops;
    return 1;
}

/*===========================================================================
 * Main FluxASM parse function
 *===========================================================================*/
flux_status flux_asm_parse(const char *asm_text, flux_module *mod, flux_error *err)
{
    if (!asm_text || !mod) {
        flux_error_set(err, FLUX_ERR_INTERNAL, 0, 0, "null argument");
        return FLUX_ERR_INTERNAL;
    }
    flux_error_init(err);

    asm_parser p;
    p.input = asm_text;
    p.pos = asm_text;
    p.line = 1;
    p.column = 1;
    p.arena = mod->arena;
    p.error = err;
    p.mod = mod;
    p.current_wave = NULL;
    p.current_unit = NULL;

    /* Allocate reasonable starting capacity for units */
    mod->units = (flux_unit*)flux_arena_alloc(p.arena, 64 * sizeof(flux_unit));
    if (!mod->units) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return FLUX_ERR_OOM; }
    memset(mod->units, 0, 64 * sizeof(flux_unit));

    while (*p.pos) {
        asm_skip_ws(&p);
        if (!*p.pos) break;

        /* Comment */
        if (*p.pos == '#' || *p.pos == ';') {
            asm_skip_line(&p);
            continue;
        }

        /* Empty line */
        if (*p.pos == '\n') { p.pos++; p.line++; p.column = 1; continue; }

        /* Directive */
        if (*p.pos == '.') {
            p.pos++; p.column++;
            char *dir = asm_parse_ident(&p);
            if (!dir) { asm_error(&p, "expected directive name"); return FLUX_ERR_PARSE; }

            if (strcmp(dir, "wave") == 0) {
                if (!asm_parse_wave(&p)) return FLUX_ERR_PARSE;
            } else if (strcmp(dir, "unit") == 0) {
                char *uname = asm_parse_ident(&p);
                if (!uname) { asm_error(&p, "expected unit name"); return FLUX_ERR_PARSE; }

                int ui = mod->num_units++;
                if (ui >= 64) { asm_error(&p, "too many units"); return FLUX_ERR_PARSE; }
                flux_unit *u = &mod->units[ui];
                memset(u, 0, sizeof(flux_unit));
                u->name = uname;
                u->wave = p.current_wave ? flux_arena_strdup(p.arena, p.current_wave->name) : NULL;
                p.current_unit = u;

                /* Parse uses/defs/latency/predicate */
                asm_skip_ws(&p);
                while (*p.pos && *p.pos != '\n') {
                    if (*p.pos == '#') break;
                    char *keyword = asm_parse_ident(&p);
                    if (!keyword) break;
                    if (strcmp(keyword, "uses") == 0 || strcmp(keyword, "defs") == 0) {
                        int is_uses = (strcmp(keyword, "uses") == 0);
                        asm_skip_ws(&p);
                        if (!asm_expect(&p, ':')) break;
                        char ***target = is_uses ? &u->uses : &u->defs;
                        int *tcount = is_uses ? &u->num_uses : &u->num_defs;
                        /* Parse register list */
                        int cap = 8;
                        int cnt = 0;
                        *target = (char**)flux_arena_alloc(p.arena, cap * sizeof(char*));
                        while (1) {
                            asm_skip_ws(&p);
                            if (*p.pos == ',' || *p.pos == ':' || *p.pos == '\n' || *p.pos == '#') break;
                            char *rname = asm_parse_ident(&p);
                            if (!rname) break;
                            if (cnt >= cap) { cap *= 2; }
                            (*target)[cnt++] = rname;
                            asm_skip_ws(&p);
                            if (*p.pos == ',') { p.pos++; p.column++; }
                            else break;
                        }
                        *tcount = cnt;
                    } else if (strcmp(keyword, "latency") == 0) {
                        asm_skip_ws(&p);
                        if (!asm_expect(&p, ':')) break;
                        asm_skip_ws(&p);
                        const char *start = p.pos;
                        while (isdigit((unsigned char)*p.pos)) p.pos++;
                        char num[16];
                        size_t nl = (size_t)(p.pos - start);
                        if (nl < sizeof(num)) {
                            memcpy(num, start, nl);
                            num[nl] = '\0';
                            u->latency = atoi(num);
                        }
                    } else if (strcmp(keyword, "predicate") == 0) {
                        asm_skip_ws(&p);
                        if (!asm_expect(&p, ':')) break;
                        u->predicate = asm_parse_ident(&p);
                    } else break;
                    asm_skip_ws(&p);
                }
                /* End of .unit line */
                asm_skip_line(&p);

                /* Parse instruction lines until next directive or blank line */
                while (*p.pos) {
                    asm_skip_ws(&p);
                    if (!*p.pos || *p.pos == '.' || *p.pos == '\n') break;
                    if (*p.pos == '#' || *p.pos == ';') { asm_skip_line(&p); continue; }

                    int ii = u->num_instructions++;
                    flux_instruction *new_insts = (flux_instruction*)flux_arena_alloc(p.arena,
                            u->num_instructions * sizeof(flux_instruction));
                    if (u->instructions && u->num_instructions > 1) {
                        memcpy(new_insts, u->instructions, (u->num_instructions - 1) * sizeof(flux_instruction));
                    }
                    u->instructions = new_insts;
                    if (!asm_parse_instruction(&p, &u->instructions[ii])) {
                        u->num_instructions--;
                        break;
                    }
                    asm_skip_line(&p);
                }
                continue;
            } else if (strcmp(dir, "dep") == 0) {
                /* .dep U0 -> U1 (v3) */
                char *from = asm_parse_ident(&p);
                if (!from) { asm_error(&p, "expected source unit in .dep"); return FLUX_ERR_PARSE; }
                asm_skip_ws(&p);
                if (!asm_expect(&p, '-') || !asm_expect(&p, '>')) {
                    asm_error(&p, "expected '->' in .dep"); return FLUX_ERR_PARSE;
                }
                char *to = asm_parse_ident(&p);
                if (!to) { asm_error(&p, "expected target unit in .dep"); return FLUX_ERR_PARSE; }

                int nd = mod->num_deps++;
                flux_dep *new_deps = (flux_dep*)flux_arena_alloc(p.arena,
                        mod->num_deps * sizeof(flux_dep));
                if (mod->deps) {
                    memcpy(new_deps, mod->deps, (mod->num_deps - 1) * sizeof(flux_dep));
                }
                mod->deps = new_deps;
                flux_dep *d = &mod->deps[nd];
                memset(d, 0, sizeof(flux_dep));
                d->from = from;
                d->to = to;

                /* Parse optional register list in parens */
                asm_skip_ws(&p);
                if (*p.pos == '(') {
                    p.pos++; p.column++;
                    int cap = 8;
                    int cnt = 0;
                    d->regs = (char**)flux_arena_alloc(p.arena, cap * sizeof(char*));
                    while (1) {
                        asm_skip_ws(&p);
                        if (*p.pos == ')') break;
                        char *rn = asm_parse_ident(&p);
                        if (!rn) break;
                        if (cnt >= cap) cap *= 2;
                        d->regs[cnt++] = rn;
                        asm_skip_ws(&p);
                        if (*p.pos == ',') { p.pos++; p.column++; }
                    }
                    d->num_regs = cnt;
                    if (*p.pos == ')') { p.pos++; p.column++; }
                }
                asm_skip_line(&p);
                continue;
            } else if (strcmp(dir, "mem") == 0) {
                /* .mem e1, MEM_REGION("shared0") */
                char *ereg = asm_parse_ident(&p);
                if (ereg) asm_find_or_add_reg(&p, ereg, NULL);
                asm_skip_ws(&p);
                if (*p.pos == ',') { p.pos++; p.column++; }
                asm_skip_ws(&p);
                if (*p.pos == 'M') {
                    /* MEM_REGION("name") */
                    if (strncmp(p.pos, "MEM_REGION", 10) == 0) {
                        p.pos += 10; p.column += 10;
                        asm_skip_ws(&p);
                        if (*p.pos == '(') { p.pos++; p.column++; }
                        char *rname = asm_parse_string(&p);
                        if (*p.pos == ')') { p.pos++; p.column++; }
                        if (rname) {
                            int nm = mod->num_memory_regions++;
                            flux_memory_region *new_mrs = (flux_memory_region*)flux_arena_alloc(p.arena,
                                    mod->num_memory_regions * sizeof(flux_memory_region));
                            if (mod->memory_regions) {
                                memcpy(new_mrs, mod->memory_regions,
                                       (mod->num_memory_regions - 1) * sizeof(flux_memory_region));
                            }
                            mod->memory_regions = new_mrs;
                            flux_memory_region *mr = &mod->memory_regions[nm];
                            memset(mr, 0, sizeof(flux_memory_region));
                            mr->name = rname;
                            mr->kind = flux_arena_strdup(p.arena, "shared");
                        }
                    }
                }
                asm_skip_line(&p);
                continue;
            } else if (strcmp(dir, "io") == 0) {
                /* .io e0, DEV_GENERIC("log") */
                char *ereg = asm_parse_ident(&p);
                if (ereg) asm_find_or_add_reg(&p, ereg, NULL);
                asm_skip_ws(&p);
                if (*p.pos == ',') { p.pos++; p.column++; }
                asm_skip_ws(&p);
                asm_skip_line(&p);
                continue;
            } else {
                /* Unknown directive, skip line */
                asm_skip_line(&p);
                continue;
            }
            asm_skip_line(&p);
            continue;
        }

        /* Plain identifier - could be unit name or assembly instruction */
        /* Check if this is a unit label (ends with colon) */
        char *ident = asm_parse_ident(&p);
        if (ident) {
            asm_skip_ws(&p);
            if (*p.pos == ':') {
                /* It's a unit label */
                p.pos++; p.column++;
                int ui = mod->num_units++;
                if (ui >= 64) { asm_error(&p, "too many units"); return FLUX_ERR_PARSE; }
                flux_unit *u = &mod->units[ui];
                memset(u, 0, sizeof(flux_unit));
                u->name = ident;
                u->wave = p.current_wave ? flux_arena_strdup(p.arena, p.current_wave->name) : NULL;
                if (p.current_wave) {
                    int wui = p.current_wave->num_units++;
                    char **new_units = (char**)flux_arena_alloc(p.arena,
                        p.current_wave->num_units * sizeof(char*));
                    if (p.current_wave->units && p.current_wave->num_units > 1) {
                        memcpy(new_units, p.current_wave->units,
                            (p.current_wave->num_units - 1) * sizeof(char*));
                    }
                    p.current_wave->units = new_units;
                    p.current_wave->units[wui] = ident;
                }
                p.current_unit = u;
                asm_skip_line(&p);

                /* Parse instruction lines */
                while (*p.pos) {
                    asm_skip_ws(&p);
                    if (!*p.pos || *p.pos == '.' || *p.pos == '\n') break;
                    if (*p.pos == '#' || *p.pos == ';') { asm_skip_line(&p); continue; }

                    int ii = u->num_instructions++;
                    flux_instruction *new_insts = (flux_instruction*)flux_arena_alloc(p.arena,
                            u->num_instructions * sizeof(flux_instruction));
                    if (u->instructions && u->num_instructions > 1) {
                        memcpy(new_insts, u->instructions, (u->num_instructions - 1) * sizeof(flux_instruction));
                    }
                    u->instructions = new_insts;
                    if (!asm_parse_instruction(&p, &u->instructions[ii])) {
                        u->num_instructions--;
                        break;
                    }
                    asm_skip_line(&p);
                }
                continue;
            }
        }
        /* Not a directive or label - skip line */
        asm_skip_line(&p);
    }

    /* Add undefined registers that were auto-created */
    return FLUX_OK;
}

flux_status flux_asm_parse_file(const char *path, flux_module *mod, flux_error *err)
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
    if (!buf) { fclose(f); flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return FLUX_ERR_OOM; }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[nread] = '\0';
    return flux_asm_parse(buf, mod, err);
}

/*===========================================================================
 * FluxASM Printer
 *===========================================================================*/
char* flux_asm_print(const flux_module *mod, flux_error *err)
{
    if (!mod) { flux_error_set(err, FLUX_ERR_INTERNAL, 0, 0, "null module"); return NULL; }
    flux_error_init(err);

    /* Use a simple string builder approach - count then build */
    /* For simplicity, allocate a large buffer in arena */
    char *buf = (char*)flux_arena_alloc(mod->arena, 65536);
    if (!buf) { flux_error_set(err, FLUX_ERR_OOM, 0, 0, "out of memory"); return NULL; }
    size_t len = 0;
    size_t cap = 65536;

#define APPEND(fmt, ...) do { \
    int _n = snprintf(buf + len, cap - len, fmt, __VA_ARGS__); \
    if (_n > 0) len += _n; \
    if (len >= cap) { /* truncated */ } \
} while(0)
#define APPEND0(s) do { \
    int _n = snprintf(buf + len, cap - len, "%s", s); \
    if (_n > 0) len += _n; \
} while(0)

    APPEND("# FluxASM module: %s", mod->name);
    APPEND0("\n");
    APPEND("# Version: %s", mod->version);
    APPEND0("\n\n");

    /* Types */
    if (mod->num_types > 0) {
        APPEND0("# Types:\n");
        for (int i = 0; i < mod->num_types; i++) {
            char tbuf[32];
            flux_type_format(&mod->types[i], tbuf, sizeof(tbuf));
            APPEND("#   %s = %s", mod->type_names[i], tbuf);
            APPEND0("\n");
        }
        APPEND0("\n");
    }

    /* Registers */
    if (mod->num_registers > 0) {
        APPEND0("# Registers:\n");
        for (int i = 0; i < mod->num_registers; i++) {
            char tbuf[32];
            flux_type_format(&mod->registers[i].type, tbuf, sizeof(tbuf));
            const char *cls = "?";
            switch (mod->registers[i].reg_class) {
            case FLUX_REG_SCALAR: cls = "scalar"; break;
            case FLUX_REG_VECTOR: cls = "vector"; break;
            case FLUX_REG_PREDICATE: cls = "predicate"; break;
            case FLUX_REG_EFFECT: cls = "effect"; break;
            }
            APPEND("#   %s: %s<%s>", mod->registers[i].name, cls, tbuf);
            APPEND0("\n");
        }
        APPEND0("\n");
    }

    /* Devices */
    for (int i = 0; i < mod->num_devices; i++) {
        APPEND(".io e?, DEV_GENERIC(\"%s\")  # device: %s",
               mod->devices[i].name, mod->devices[i].name);
        APPEND0("\n");
    }
    if (mod->num_devices > 0) APPEND0("\n");

    /* Memory Regions */
    for (int i = 0; i < mod->num_memory_regions; i++) {
        APPEND(".mem e?, MEM_REGION(\"%s\")  # memory region: %s, kind: %s",
               mod->memory_regions[i].name, mod->memory_regions[i].name,
               mod->memory_regions[i].kind);
        APPEND0("\n");
    }
    if (mod->num_memory_regions > 0) APPEND0("\n");

    /* Waves and their units */
    for (int wi = 0; wi < mod->num_waves; wi++) {
        flux_wave *w = &mod->waves[wi];
        APPEND(".wave %s", w->name);
        if (w->num_params > 0) {
            APPEND0(" (");
            for (int j = 0; j < w->num_params; j++) {
                char tbuf[32];
                flux_type_format(&w->params[j].type, tbuf, sizeof(tbuf));
                APPEND("%s:%s", w->params[j].reg, tbuf);
                if (j + 1 < w->num_params) APPEND0(", ");
            }
            APPEND0(")");
        }
        APPEND0("\n");

        /* Units for this wave */
        for (int ui = 0; ui < mod->num_units; ui++) {
            if (strcmp(mod->units[ui].wave, w->name) != 0) continue;
            flux_unit *u = &mod->units[ui];
            APPEND("  %s", u->name);
            APPEND0(":");

            if (u->num_uses > 0) {
                APPEND0(" uses: ");
                for (int j = 0; j < u->num_uses; j++) {
                    APPEND("%s", u->uses[j]);
                    if (j + 1 < u->num_uses) APPEND0(", ");
                }
            }
            if (u->num_defs > 0) {
                APPEND0(" defs: ");
                for (int j = 0; j < u->num_defs; j++) {
                    APPEND("%s", u->defs[j]);
                    if (j + 1 < u->num_defs) APPEND0(", ");
                }
            }
            if (u->latency > 0) APPEND(" latency: %d", u->latency);
            if (u->predicate) APPEND(" predicate: %s", u->predicate);
            APPEND0("\n");

            /* Instructions */
            for (int ii = 0; ii < u->num_instructions; ii++) {
                flux_instruction *inst = &u->instructions[ii];
                APPEND("    %s", inst->opcode);
                for (int oi = 0; oi < inst->num_operands; oi++) {
                    flux_operand *op = &inst->operands[oi];
                    APPEND0(oi == 0 ? " " : ", ");
                    switch (op->kind) {
                    case FLUX_OP_REG:
                        APPEND("%s", op->u.reg.name);
                        break;
                    case FLUX_OP_IMM:
                        if (op->u.imm.str) APPEND("%s", op->u.imm.str);
                        else if (op->u.imm.is_int) APPEND("%" PRId64, op->u.imm.ival);
                        else APPEND("%g", op->u.imm.fval);
                        break;
                    case FLUX_OP_REF:
                        APPEND("%s:%s", op->u.ref.type, op->u.ref.name);
                        break;
                    case FLUX_OP_WAVE:
                        APPEND("%s(", op->u.wave.wave_name);
                        for (int a = 0; a < op->u.wave.num_args; a++) {
                            APPEND("%s=%s", op->u.wave.args[a].param_reg,
                                   op->u.wave.args[a].arg_reg ? op->u.wave.args[a].arg_reg : "?");
                            if (a + 1 < op->u.wave.num_args) APPEND0(", ");
                        }
                        APPEND0(")");
                        break;
                    }
                }
                APPEND0("\n");
            }
        }
        APPEND0("\n");
    }

    /* Dependencies */
    if (mod->num_deps > 0) {
        APPEND0("# Dependencies:\n");
        for (int i = 0; i < mod->num_deps; i++) {
            APPEND(".dep %s -> %s", mod->deps[i].from, mod->deps[i].to);
            if (mod->deps[i].num_regs > 0) {
                APPEND0(" (");
                for (int j = 0; j < mod->deps[i].num_regs; j++) {
                    APPEND("%s", mod->deps[i].regs[j]);
                    if (j + 1 < mod->deps[i].num_regs) APPEND0(", ");
                }
                APPEND0(")");
            }
            APPEND0("\n");
        }
        APPEND0("\n");
    }

    /* Control graph */
    if (mod->num_control_edges > 0) {
        APPEND0("# Control flow:\n");
        for (int i = 0; i < mod->num_control_edges; i++) {
            APPEND("# .ctrl %s ->", mod->control_edges[i].from);
            for (int j = 0; j < mod->control_edges[i].num_to; j++) {
                APPEND(" %s", mod->control_edges[i].to[j]);
                if (j + 1 < mod->control_edges[i].num_to) APPEND0(",");
            }
            APPEND0("\n");
        }
    }

    buf[len] = '\0';
    return buf;

#undef APPEND
#undef APPEND0
}
