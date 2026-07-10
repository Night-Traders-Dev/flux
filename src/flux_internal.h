#ifndef FLUX_INTERNAL_H
#define FLUX_INTERNAL_H

#include "flux.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <inttypes.h>

/*===========================================================================
 * Dynamic array helpers (arena-backed)
 *===========================================================================*/
#define DA_INIT(cap) do { (cap) = 0; } while(0)
#define DA_APPEND(a, arena, elem) do {                     \
    unsigned long _da_idx__ = (a).count++;                  \
    if ((a).count > (a).capacity) {                         \
        size_t _newcap = (a).capacity ? (a).capacity * 2 : 8;\
        (a).items = flux_arena_alloc(arena,                 \
                      _newcap * sizeof((a).items[0]));      \
        memcpy((a).items, (a).items,                       \
               (a).capacity * sizeof((a).items[0]));       \
        (a).capacity = _newcap;                             \
    }                                                       \
    /* Actually simpler to pre-allocate and track count */   \
} while(0)

/* We use a simpler approach: arrays with count + arena allocation */

/*===========================================================================
 * JSON internal types
 *===========================================================================*/
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} json_value_kind;

typedef struct json_value json_value;

typedef struct json_pair {
    char       *key;
    json_value *value;
} json_pair;

struct json_value {
    json_value_kind kind;
    union {
        int           boolean;
        double        number;
        char         *string;
        struct { json_value **items; int count; } array;
        struct { json_pair *pairs; int count; } object;
    } u;
};

/*===========================================================================
 * JSON parser state
 *===========================================================================*/
typedef struct json_parser {
    const char *input;
    const char *pos;
    int         line;
    int         column;
    flux_arena *arena;
    flux_error *error;
} json_parser;

/*===========================================================================
 * JSON internal API
 *===========================================================================*/
json_value* json_parse_value(json_parser *p);
json_value* json_parse_array(json_parser *p);
json_value* json_parse_object(json_parser *p);
char*       json_parse_string(json_parser *p);
json_value* json_parse_number(json_parser *p);

/* Skip whitespace */
void json_skip_ws(json_parser *p);

/* Match and consume expected character */
int json_expect(json_parser *p, char c);

/* Report error */
void json_error(json_parser *p, const char *fmt, ...);

/* Serialize JSON value to string (arena-allocated) */
char* json_serialize_value(json_value *v, flux_arena *arena);

/* Serialize inline */
json_value* json_parse(const char *input, flux_arena *arena, flux_error *err);
void json_serialize_value_buf(json_value *v, char **buf, size_t *len, size_t *cap, flux_arena *arena);

/*===========================================================================
 * Arena implementation detail - opaque, defined in flux_arena.c
 *===========================================================================*/

#endif /* FLUX_INTERNAL_H */
