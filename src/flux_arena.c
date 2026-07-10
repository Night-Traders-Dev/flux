#include "flux_internal.h"

/*===========================================================================
 * Arena allocator - uses linked list of fixed-size blocks
 * This avoids pointer invalidation that occurs with realloc-based arenas.
 *===========================================================================*/

#define FLUX_ARENA_PAGE_SIZE (1024 * 512)  /* 512KB per page */

typedef struct flux_arena_page {
    struct flux_arena_page *next;
    size_t                  used;
    size_t                  capacity;
    char                    data[];
} flux_arena_page;

struct flux_arena {
    flux_arena_page *head;
    flux_arena_page *current;
};

static flux_arena_page* page_create(size_t min_capacity)
{
    size_t cap = FLUX_ARENA_PAGE_SIZE;
    while (cap < min_capacity + sizeof(flux_arena_page))
        cap *= 2;
    flux_arena_page *p = (flux_arena_page*)malloc(sizeof(flux_arena_page) + cap);
    if (!p) return NULL;
    p->next = NULL;
    p->used = 0;
    p->capacity = cap;
    return p;
}

flux_arena* flux_arena_create(size_t capacity)
{
    (void)capacity;
    flux_arena *a = (flux_arena*)malloc(sizeof(flux_arena));
    if (!a) return NULL;

    a->head = page_create(4096);
    if (!a->head) {
        free(a);
        return NULL;
    }
    a->current = a->head;
    return a;
}

void flux_arena_destroy(flux_arena *a)
{
    if (!a) return;
    flux_arena_page *p = a->head;
    while (p) {
        flux_arena_page *next = p->next;
        free(p);
        p = next;
    }
    free(a);
}

void* flux_arena_alloc(flux_arena *a, size_t size)
{
    /* Round up to 8-byte alignment */
    size = (size + 7) & ~7;

    flux_arena_page *p = a->current;

    /* Check if current page has room */
    if (p->used + size > p->capacity) {
        /* Need a new page */
        flux_arena_page *np = page_create(size);
        if (!np) return NULL;
        p->next = np;
        a->current = np;
        p = np;
    }

    void *ptr = p->data + p->used;
    p->used += size;
    return ptr;
}

char* flux_arena_strdup(flux_arena *a, const char *s)
{
    size_t len = strlen(s);
    char *dup = (char*)flux_arena_alloc(a, len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

char* flux_arena_strndup(flux_arena *a, const char *s, size_t n)
{
    size_t slen = strlen(s);
    if (n > slen) n = slen;
    char *dup = (char*)flux_arena_alloc(a, n + 1);
    if (!dup) return NULL;
    memcpy(dup, s, n);
    dup[n] = '\0';
    return dup;
}

void flux_arena_reset(flux_arena *a)
{
    /* Free all pages except the first, reset the first */
    flux_arena_page *p = a->head->next;
    while (p) {
        flux_arena_page *next = p->next;
        free(p);
        p = next;
    }
    a->head->next = NULL;
    a->head->used = 0;
    a->current = a->head;
}

size_t flux_arena_used(flux_arena *a)
{
    size_t total = 0;
    flux_arena_page *p = a->head;
    while (p) {
        total += p->used;
        p = p->next;
    }
    return total;
}

/*===========================================================================
 * Type system implementation
 *===========================================================================*/
flux_type flux_type_scalar(int bits, flux_sign sign, int is_float)
{
    flux_type t;
    t.kind = FLUX_KIND_SCALAR;
    t.bits = bits;
    t.lanes = 0;
    t.sign = sign;
    t.is_float = is_float;
    return t;
}

flux_type flux_type_vector(int lanes, int elem_bits, flux_sign sign, int is_float)
{
    flux_type t;
    t.kind = FLUX_KIND_VECTOR;
    t.bits = elem_bits;
    t.lanes = lanes;
    t.sign = sign;
    t.is_float = is_float;
    return t;
}

flux_type flux_type_pred(void)
{
    flux_type t;
    t.kind = FLUX_KIND_PREDICATE;
    t.bits = 0;
    t.lanes = 0;
    t.sign = 0;
    t.is_float = 0;
    return t;
}

flux_type flux_type_effect(void)
{
    flux_type t;
    t.kind = FLUX_KIND_EFFECT;
    t.bits = 0;
    t.lanes = 0;
    t.sign = 0;
    t.is_float = 0;
    return t;
}

int flux_type_parse(const char *name, flux_type *out)
{
    if (!name || !out) return 0;

    /* pred */
    if (strcmp(name, "pred") == 0) {
        *out = flux_type_pred();
        return 1;
    }
    /* effect */
    if (strcmp(name, "effect") == 0) {
        *out = flux_type_effect();
        return 1;
    }

    /* Vector: vN<T> */
    if (name[0] == 'v' && isdigit((unsigned char)name[1])) {
        int lanes = 0;
        const char *p = name + 1;
        while (isdigit((unsigned char)*p)) {
            lanes = lanes * 10 + (*p - '0');
            p++;
        }
        if (*p != '<') return 0;
        p++;
        /* Copy element type name into a temporary buffer (up to '>') */
        char elem_name[32];
        int ei = 0;
        while (*p && *p != '>' && ei < (int)sizeof(elem_name) - 1) {
            elem_name[ei++] = *p++;
        }
        elem_name[ei] = '\0';
        if (*p != '>') return 0;
        flux_type elem;
        if (!flux_type_parse(elem_name, &elem)) return 0;
        if (elem.kind != FLUX_KIND_SCALAR) return 0;
        *out = flux_type_vector(lanes, elem.bits, elem.sign, elem.is_float);
        return 1;
    }

    /* Scalar: i8,i16,i32,i64,u8,u16,u32,u64,f16,f32,f64 */
    if (name[0] == 'i' || name[0] == 'u' || name[0] == 'f') {
        int is_float = (name[0] == 'f');
        flux_sign sign = (name[0] == 'u') ? FLUX_SIGN_UNSIGNED : FLUX_SIGN_SIGNED;
        int bits = 0;
        const char *p = name + 1;
        while (*p) {
            if (!isdigit((unsigned char)*p)) return 0;
            bits = bits * 10 + (*p - '0');
            p++;
        }
        if (bits <= 0) return 0;
        *out = flux_type_scalar(bits, sign, is_float);
        return 1;
    }

    return 0;
}

int flux_type_format(const flux_type *t, char *buf, size_t cap)
{
    if (!t || !buf || cap == 0) return 0;

    switch (t->kind) {
    case FLUX_KIND_SCALAR: {
        char prefix = t->is_float ? 'f' : (t->sign == FLUX_SIGN_UNSIGNED ? 'u' : 'i');
        return snprintf(buf, cap, "%c%d", prefix, t->bits);
    }
    case FLUX_KIND_VECTOR:
        if (t->lanes > 0) {
            char elem_buf[32];
            flux_type elem = flux_type_scalar(t->bits, t->sign, t->is_float);
            flux_type_format(&elem, elem_buf, sizeof(elem_buf));
            return snprintf(buf, cap, "v%d<%s>", t->lanes, elem_buf);
        }
        return snprintf(buf, cap, "vector");
    case FLUX_KIND_PREDICATE:
        return snprintf(buf, cap, "pred");
    case FLUX_KIND_EFFECT:
        return snprintf(buf, cap, "effect");
    default:
        return snprintf(buf, cap, "unknown");
    }
}

/*===========================================================================
 * Module lifecycle
 *===========================================================================*/
flux_module* flux_module_create(const char *name)
{
    flux_arena *arena = flux_arena_create(65536);
    if (!arena) return NULL;

    flux_module *mod = (flux_module*)flux_arena_alloc(arena, sizeof(flux_module));
    if (!mod) { flux_arena_destroy(arena); return NULL; }

    memset(mod, 0, sizeof(flux_module));
    mod->arena = arena;
    mod->name = name ? flux_arena_strdup(arena, name) : flux_arena_strdup(arena, "unnamed");
    mod->version = flux_arena_strdup(arena, FLUX_VERSION);

    return mod;
}

void flux_module_destroy(flux_module *m)
{
    if (m) {
        flux_arena_destroy(m->arena);
    }
}

/*===========================================================================
 * Error handling
 *===========================================================================*/
void flux_error_init(flux_error *e)
{
    if (e) {
        e->line = 0;
        e->column = 0;
        e->status = FLUX_OK;
        e->message[0] = '\0';
    }
}

void flux_error_set(flux_error *e, int status, int line, int col, const char *fmt, ...)
{
    if (!e) return;
    e->status = status;
    e->line = line;
    e->column = col;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
}

/*===========================================================================
 * Lookup helpers
 *===========================================================================*/
int flux_find_register(const flux_module *mod, const char *name)
{
    if (!mod || !name) return -1;
    for (int i = 0; i < mod->num_registers; i++) {
        if (strcmp(mod->registers[i].name, name) == 0)
            return i;
    }
    return -1;
}

int flux_find_wave(const flux_module *mod, const char *name)
{
    if (!mod || !name) return -1;
    for (int i = 0; i < mod->num_waves; i++) {
        if (strcmp(mod->waves[i].name, name) == 0)
            return i;
    }
    return -1;
}

int flux_find_unit(const flux_module *mod, const char *name)
{
    if (!mod || !name) return -1;
    for (int i = 0; i < mod->num_units; i++) {
        if (strcmp(mod->units[i].name, name) == 0)
            return i;
    }
    return -1;
}

int flux_find_device(const flux_module *mod, const char *name)
{
    if (!mod || !name) return -1;
    for (int i = 0; i < mod->num_devices; i++) {
        if (strcmp(mod->devices[i].name, name) == 0)
            return i;
    }
    return -1;
}

int flux_find_memory_region(const flux_module *mod, const char *name)
{
    if (!mod || !name) return -1;
    for (int i = 0; i < mod->num_memory_regions; i++) {
        if (strcmp(mod->memory_regions[i].name, name) == 0)
            return i;
    }
    return -1;
}

int flux_find_type(const flux_module *mod, const char *name)
{
    if (!mod || !name) return -1;
    for (int i = 0; i < mod->num_types; i++) {
        if (strcmp(mod->type_names[i], name) == 0)
            return i;
    }
    return -1;
}
