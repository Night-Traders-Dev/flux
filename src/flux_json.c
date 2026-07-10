#include "flux_internal.h"

/*===========================================================================
 * JSON Parser
 *===========================================================================*/

static int json_hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void json_skip_ws(json_parser *p)
{
    while (*p->pos) {
        char c = *p->pos;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (c == '\n') { p->line++; p->column = 1; }
            else { p->column++; }
            p->pos++;
        } else {
            break;
        }
    }
}

int json_expect(json_parser *p, char c)
{
    json_skip_ws(p);
    if (*p->pos == c) {
        p->pos++;
        if (c == '\n') { p->line++; p->column = 1; }
        else { p->column++; }
        return 1;
    }
    return 0;
}

void json_error(json_parser *p, const char *fmt, ...)
{
    if (!p->error) return;
    p->error->line = p->line;
    p->error->column = p->column;
    p->error->status = FLUX_ERR_PARSE;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error->message, sizeof(p->error->message), fmt, ap);
    va_end(ap);
}

char* json_parse_string(json_parser *p)
{
    json_skip_ws(p);
    if (*p->pos != '"') {
        json_error(p, "Expected '\"' at line %d, col %d", p->line, p->column);
        return NULL;
    }
    p->pos++; p->column++;

    /* First pass: find length for allocation */
    const char *start = p->pos;
    size_t len = 0;
    int escape = 0;
    while (*p->pos) {
        if (escape) {
            escape = 0;
            len++;
            p->pos++;
        } else if (*p->pos == '\\') {
            escape = 1;
            p->pos++;
        } else if (*p->pos == '"') {
            break;
        } else {
            len++;
            p->pos++;
        }
    }
    if (*p->pos != '"') {
        json_error(p, "Unterminated string");
        return NULL;
    }

    /* Allocate */
    char *result = (char*)flux_arena_alloc(p->arena, len + 1);
    if (!result) {
        json_error(p, "Out of memory");
        return NULL;
    }

    /* Second pass: decode */
    p->pos = start;
    size_t i = 0;
    while (*p->pos && *p->pos != '"') {
        if (*p->pos == '\\') {
            p->pos++;
            switch (*p->pos) {
            case '"':  result[i++] = '"';  break;
            case '\\': result[i++] = '\\'; break;
            case '/':  result[i++] = '/';  break;
            case 'b':  result[i++] = '\b'; break;
            case 'f':  result[i++] = '\f'; break;
            case 'n':  result[i++] = '\n'; break;
            case 'r':  result[i++] = '\r'; break;
            case 't':  result[i++] = '\t'; break;
            case 'u': {
                /* Unicode escape - simple pass-through for now */
                if (p->pos[1] && p->pos[2] && p->pos[3] && p->pos[4]) {
                    int h1 = json_hex_val(p->pos[1]);
                    int h2 = json_hex_val(p->pos[2]);
                    int h3 = json_hex_val(p->pos[3]);
                    int h4 = json_hex_val(p->pos[4]);
                    if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                        int cp = (h1 << 12) | (h2 << 8) | (h3 << 4) | h4;
                        if (cp < 128) {
                            result[i++] = (char)cp;
                        } else if (cp < 2048) {
                            result[i++] = 0xC0 | (cp >> 6);
                            result[i++] = 0x80 | (cp & 0x3F);
                        } else {
                            result[i++] = 0xE0 | (cp >> 12);
                            result[i++] = 0x80 | ((cp >> 6) & 0x3F);
                            result[i++] = 0x80 | (cp & 0x3F);
                        }
                        p->pos += 4;
                    }
                }
                break;
            }
            default:
                result[i++] = *p->pos;
                break;
            }
            p->pos++;
        } else {
            result[i++] = *p->pos;
            p->pos++;
        }
    }
    result[i] = '\0';
    p->pos++; /* skip closing quote */
    p->column += (int)(p->pos - start) + 2;
    return result;
}

json_value* json_parse_number(json_parser *p)
{
    json_skip_ws(p);
    const char *start = p->pos;

    if (*p->pos == '-') { p->pos++; }

    while (isdigit((unsigned char)*p->pos)) p->pos++;
    if (*p->pos == '.') { p->pos++; while (isdigit((unsigned char)*p->pos)) p->pos++; }
    if (*p->pos == 'e' || *p->pos == 'E') {
        p->pos++;
        if (*p->pos == '+' || *p->pos == '-') p->pos++;
        while (isdigit((unsigned char)*p->pos)) p->pos++;
    }

    size_t len = (size_t)(p->pos - start);
    char *buf = (char*)flux_arena_alloc(p->arena, len + 1);
    if (!buf) return NULL;
    memcpy(buf, start, len);
    buf[len] = '\0';

    json_value *v = (json_value*)flux_arena_alloc(p->arena, sizeof(json_value));
    if (!v) return NULL;
    v->kind = JSON_NUMBER;
    v->u.number = atof(buf);
    p->column += (int)len;
    return v;
}

json_value* json_parse_value(json_parser *p);

json_value* json_parse_array(json_parser *p)
{
    json_value *v = (json_value*)flux_arena_alloc(p->arena, sizeof(json_value));
    if (!v) return NULL;
    v->kind = JSON_ARRAY;
    v->u.array.items = NULL;
    v->u.array.count = 0;

    p->pos++; p->column++; /* skip '[' */
    json_skip_ws(p);

    if (*p->pos == ']') {
        p->pos++; p->column++;
        return v;
    }

    /* Count elements */
    int cap = 64;
    v->u.array.items = (json_value**)flux_arena_alloc(p->arena, cap * sizeof(json_value*));
    if (!v->u.array.items) return NULL;

    while (1) {
        if (v->u.array.count >= cap) {
            /* Exceeded capacity, stop reading more elements */
            break;
        }
        json_value *elem = json_parse_value(p);
        if (!elem) return NULL;
        v->u.array.items[v->u.array.count++] = elem;
        json_skip_ws(p);
        if (*p->pos == ',') { p->pos++; p->column++; json_skip_ws(p); }
        else if (*p->pos == ']') { p->pos++; p->column++; break; }
        else { json_error(p, "Expected ',' or ']' in array"); return NULL; }
    }
    return v;
}

json_value* json_parse_object(json_parser *p)
{
    json_value *v = (json_value*)flux_arena_alloc(p->arena, sizeof(json_value));
    if (!v) return NULL;
    v->kind = JSON_OBJECT;
    v->u.object.pairs = NULL;
    v->u.object.count = 0;

    p->pos++; p->column++; /* skip '{' */
    json_skip_ws(p);

    if (*p->pos == '}') {
        p->pos++; p->column++;
        return v;
    }

    int cap = 64;
    v->u.object.pairs = (json_pair*)flux_arena_alloc(p->arena, cap * sizeof(json_pair));
    if (!v->u.object.pairs) return NULL;

    while (1) {
        if (v->u.object.count >= cap) {
            /* Exceeded capacity, stop reading more pairs */
            break;
        }
        char *key = json_parse_string(p);
        if (!key) return NULL;
        json_skip_ws(p);
        if (!json_expect(p, ':')) {
            json_error(p, "Expected ':' in object");
            return NULL;
        }
        json_value *val = json_parse_value(p);
        if (!val) return NULL;
        v->u.object.pairs[v->u.object.count].key = key;
        v->u.object.pairs[v->u.object.count].value = val;
        v->u.object.count++;
        json_skip_ws(p);
        if (*p->pos == ',') { p->pos++; p->column++; json_skip_ws(p); }
        else if (*p->pos == '}') { p->pos++; p->column++; break; }
        else { json_error(p, "Expected ',' or '}' in object"); return NULL; }
    }
    return v;
}

json_value* json_parse_value(json_parser *p)
{
    json_skip_ws(p);
    if (!*p->pos) {
        json_error(p, "Unexpected end of input");
        return NULL;
    }

    switch (*p->pos) {
    case '"':
        {
            json_value *v = (json_value*)flux_arena_alloc(p->arena, sizeof(json_value));
            if (!v) return NULL;
            v->kind = JSON_STRING;
            v->u.string = json_parse_string(p);
            return v;
        }
    case '{': return json_parse_object(p);
    case '[': return json_parse_array(p);
    case 't':
        if (strncmp(p->pos, "true", 4) == 0) {
            json_value *v = (json_value*)flux_arena_alloc(p->arena, sizeof(json_value));
            if (!v) return NULL;
            v->kind = JSON_BOOL;
            v->u.boolean = 1;
            p->pos += 4; p->column += 4;
            return v;
        }
        json_error(p, "Unexpected token at line %d", p->line);
        return NULL;
    case 'f':
        if (strncmp(p->pos, "false", 5) == 0) {
            json_value *v = (json_value*)flux_arena_alloc(p->arena, sizeof(json_value));
            if (!v) return NULL;
            v->kind = JSON_BOOL;
            v->u.boolean = 0;
            p->pos += 5; p->column += 5;
            return v;
        }
        json_error(p, "Unexpected token at line %d", p->line);
        return NULL;
    case 'n':
        if (strncmp(p->pos, "null", 4) == 0) {
            json_value *v = (json_value*)flux_arena_alloc(p->arena, sizeof(json_value));
            if (!v) return NULL;
            v->kind = JSON_NULL;
            p->pos += 4; p->column += 4;
            return v;
        }
        json_error(p, "Unexpected token at line %d", p->line);
        return NULL;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return json_parse_number(p);
    default:
        json_error(p, "Unexpected character '%c' at line %d, col %d", *p->pos, p->line, p->column);
        return NULL;
    }
}

/*===========================================================================
 * Public parse entry
 *===========================================================================*/
json_value* json_parse(const char *input, flux_arena *arena, flux_error *err)
{
    json_parser p;
    p.input = input;
    p.pos = input;
    p.line = 1;
    p.column = 1;
    p.arena = arena;
    p.error = err;

    json_value *v = json_parse_value(&p);
    if (v && p.error && p.error->status != FLUX_OK) {
        return NULL;
    }
    return v;
}

/*===========================================================================
 * JSON Serializer
 *===========================================================================*/

static void json_escape_string(const char *s, char **buf, size_t *len, size_t *cap, flux_arena *arena)
{
    (void)arena;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        size_t need = 1;
        const char *repl = NULL;
        switch (c) {
        case '"':  repl = "\\\""; need = 2; break;
        case '\\': repl = "\\\\"; need = 2; break;
        case '\b': repl = "\\b";  need = 2; break;
        case '\f': repl = "\\f";  need = 2; break;
        case '\n': repl = "\\n";  need = 2; break;
        case '\r': repl = "\\r";  need = 2; break;
        case '\t': repl = "\\t";  need = 2; break;
        default:
            if (c < 0x20) need = 6; /* \uXXXX */
            break;
        }

        if (*len + need + 1 > *cap) {
            *cap = *cap ? *cap * 2 : 256;
        }

        if (repl) {
            while (*repl) {
                (*buf)[(*len)++] = *repl++;
            }
        } else if (c < 0x20) {
            int written = snprintf(*buf + *len, *cap - *len, "\\u%04x", c);
            if (written > 0) *len += written;
        } else {
            (*buf)[(*len)++] = c;
        }
        s++;
    }
}

void json_serialize_value_buf(json_value *v, char **buf, size_t *len, size_t *cap, flux_arena *arena)
{
    if (!v) {
        if (*len + 5 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        memcpy(*buf + *len, "null", 4); *len += 4;
        return;
    }

    switch (v->kind) {
    case JSON_NULL:
        if (*len + 5 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        memcpy(*buf + *len, "null", 4); *len += 4;
        break;
    case JSON_BOOL:
        if (v->u.boolean) {
            if (*len + 5 > *cap) { *cap = *cap ? *cap * 2 : 256; }
            memcpy(*buf + *len, "true", 4); *len += 4;
        } else {
            if (*len + 6 > *cap) { *cap = *cap ? *cap * 2 : 256; }
            memcpy(*buf + *len, "false", 5); *len += 5;
        }
        break;
    case JSON_NUMBER: {
        char num_buf[64];
        /* Check if it's an integer */
        double intpart;
        if (modf(v->u.number, &intpart) == 0.0 && v->u.number >= -9007199254740992.0 && v->u.number <= 9007199254740992.0) {
            snprintf(num_buf, sizeof(num_buf), "%" PRId64, (int64_t)v->u.number);
        } else {
            snprintf(num_buf, sizeof(num_buf), "%.17g", v->u.number);
        }
        size_t nlen = strlen(num_buf);
        if (*len + nlen + 1 > *cap) {
            while (*len + nlen + 1 > *cap) *cap = *cap ? *cap * 2 : 256;
        }
        memcpy(*buf + *len, num_buf, nlen); *len += nlen;
        break;
    }
    case JSON_STRING: {
        if (*len + 3 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        (*buf)[(*len)++] = '"';
        if (v->u.string) json_escape_string(v->u.string, buf, len, cap, arena);
        if (*len + 2 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        (*buf)[(*len)++] = '"';
        break;
    }
    case JSON_ARRAY: {
        if (*len + 2 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        (*buf)[(*len)++] = '[';
        for (int i = 0; i < v->u.array.count; i++) {
            if (i > 0) {
                if (*len + 2 > *cap) { *cap = *cap ? *cap * 2 : 256; }
                (*buf)[(*len)++] = ',';
            }
            json_serialize_value_buf(v->u.array.items[i], buf, len, cap, arena);
        }
        if (*len + 2 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        (*buf)[(*len)++] = ']';
        break;
    }
    case JSON_OBJECT: {
        if (*len + 2 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        (*buf)[(*len)++] = '{';
        for (int i = 0; i < v->u.object.count; i++) {
            if (i > 0) {
                if (*len + 2 > *cap) { *cap = *cap ? *cap * 2 : 256; }
                (*buf)[(*len)++] = ',';
            }
            /* Write key */
            if (*len + 3 > *cap) { *cap = *cap ? *cap * 2 : 256; }
            (*buf)[(*len)++] = '"';
            json_escape_string(v->u.object.pairs[i].key, buf, len, cap, arena);
            if (*len + 4 > *cap) { *cap = *cap ? *cap * 2 : 256; }
            (*buf)[(*len)++] = '"';
            (*buf)[(*len)++] = ':';
            json_serialize_value_buf(v->u.object.pairs[i].value, buf, len, cap, arena);
        }
        if (*len + 2 > *cap) { *cap = *cap ? *cap * 2 : 256; }
        (*buf)[(*len)++] = '}';
        break;
    }
    }
}

char* json_serialize_value(json_value *v, flux_arena *arena)
{
    size_t cap = 1024;
    size_t len = 0;
    char *buf = (char*)flux_arena_alloc(arena, cap);
    if (!buf) return NULL;
    json_serialize_value_buf(v, &buf, &len, &cap, arena);
    buf[len] = '\0';
    return buf;
}

/*===========================================================================
 * JSON value access helpers (for FluxIR parser)
 *===========================================================================*/

const char* json_as_string(json_value *v)
{
    if (!v || v->kind != JSON_STRING) return NULL;
    return v->u.string;
}

double json_as_number(json_value *v)
{
    if (!v || v->kind != JSON_NUMBER) return 0.0;
    return v->u.number;
}

int json_as_bool(json_value *v)
{
    if (!v || v->kind != JSON_BOOL) return 0;
    return v->u.boolean;
}

int json_is_null(json_value *v)
{
    return !v || v->kind == JSON_NULL;
}

json_value* json_object_get(json_value *obj, const char *key)
{
    if (!obj || obj->kind != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.pairs[i].key, key) == 0)
            return obj->u.object.pairs[i].value;
    }
    return NULL;
}

json_value* json_array_get(json_value *arr, int index)
{
    if (!arr || arr->kind != JSON_ARRAY) return NULL;
    if (index < 0 || index >= arr->u.array.count) return NULL;
    return arr->u.array.items[index];
}

int json_array_count(json_value *arr)
{
    if (!arr || arr->kind != JSON_ARRAY) return 0;
    return arr->u.array.count;
}

int json_object_count(json_value *obj)
{
    if (!obj || obj->kind != JSON_OBJECT) return 0;
    return obj->u.object.count;
}

const char* json_object_key(json_value *obj, int index)
{
    if (!obj || obj->kind != JSON_OBJECT) return NULL;
    if (index < 0 || index >= obj->u.object.count) return NULL;
    return obj->u.object.pairs[index].key;
}

json_value* json_object_value(json_value *obj, int index)
{
    if (!obj || obj->kind != JSON_OBJECT) return NULL;
    if (index < 0 || index >= obj->u.object.count) return NULL;
    return obj->u.object.pairs[index].value;
}
