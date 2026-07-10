# Arena Allocator

## Overview

The Flux arena allocator is a region-based memory allocator that backs all dynamic memory in the Flux runtime. It uses a linked list of fixed-size pages to provide fast, bulk allocation without per-object free operations. All memory associated with a `flux_module` is owned by its arena and is automatically reclaimed when the module is destroyed.

## Design

The arena is implemented as a singly-linked list of pages. Each page is a contiguous block of memory with a `used` offset. Allocation always proceeds from the current page. When the current page is exhausted, a new page is allocated and linked into the list. This design avoids pointer invalidation that would occur with `realloc`-based arenas, which is critical because Flux data structures store pointers into arena memory.

### Page Size

Pages are 512 KB by default (`FLUX_ARENA_PAGE_SIZE`). When a new page is requested, the allocator rounds up to the next power-of-two size that can accommodate the requested allocation plus page header overhead. This ensures large allocations (e.g., big instruction arrays) can fit in a single page.

### Alignment

All allocations are rounded up to 8-byte alignment via:

```c
size = (size + 7) & ~7;
```

This guarantees that any data type (including SIMD vectors) is properly aligned on all platforms supported by C99.

## API

### `flux_arena* flux_arena_create(size_t capacity)`

Creates a new arena with an initial page. The `capacity` parameter is currently unused (the initial page is always 4 KB), but is reserved for future configuration.

- **Returns:** Pointer to a new arena, or `NULL` on allocation failure.

### `void flux_arena_destroy(flux_arena *a)`

Destroys the arena and frees all pages. Any pointers returned by previous allocations from this arena become invalid.

- **Parameters:** `a` - Arena to destroy. Safe to pass `NULL`.

### `void* flux_arena_alloc(flux_arena *a, size_t size)`

Allocates `size` bytes from the arena. The memory is zero-initialized by virtue of being fresh page space, but callers should not rely on this.

- **Returns:** Pointer to allocated memory, or `NULL` if the arena is out of memory.
- **Behavior:** If the current page has insufficient room, a new page is allocated and linked. Allocation never fails due to fragmentation.

### `char* flux_arena_strdup(flux_arena *a, const char *s)`

Duplicates a null-terminated string into the arena.

- **Returns:** Pointer to the duplicated string, or `NULL` on allocation failure.

### `char* flux_arena_strndup(flux_arena *a, const char *s, size_t n)`

Duplicates at most `n` characters from `s` into the arena, null-terminating the result.

- **Returns:** Pointer to the duplicated string, or `NULL` on allocation failure.

### `void flux_arena_reset(flux_arena *a)`

Resets the arena to its initial state by freeing all pages except the first and resetting the `used` offset of the first page to zero. The current page pointer is reset to the head page.

- **Use case:** Reuse an arena for a new operation without destroying and recreating it.
- **Note:** All previous pointers into the arena become invalid after reset.

### `size_t flux_arena_used(flux_arena *a)`

Returns the total number of bytes currently allocated across all pages in the arena.

## Implementation Details

### Data Structures

```c
typedef struct flux_arena_page {
    struct flux_arena_page *next;
    size_t                  used;
    size_t                  capacity;
    char                    data[];  // Flexible array member
} flux_arena_page;

struct flux_arena {
    flux_arena_page *head;
    flux_arena_page *current;
};
```

Each page stores its own `used` offset and `capacity`. The `data` member is a flexible array member that holds the actual memory block.

### Allocation Strategy

When `flux_arena_alloc` is called:

1. Round `size` up to 8-byte alignment.
2. Check if the current page has `used + size <= capacity`.
3. If yes, return `data + used` and increment `used`.
4. If no, allocate a new page via `page_create(size)`, link it, and allocate from it.

### `page_create`

```c
static flux_arena_page* page_create(size_t min_capacity)
{
    size_t cap = FLUX_ARENA_PAGE_SIZE;
    while (cap < min_capacity + sizeof(flux_arena_page))
        cap *= 2;
    flux_arena_page *p = (flux_arena_page*)malloc(sizeof(flux_arena_page) + cap);
    ...
}
```

The new page size starts at 512 KB and doubles until it can hold `min_capacity` plus the page header. This ensures that even unusually large allocations succeed without creating many tiny pages.

## Usage in Flux

Every `flux_module` owns a single arena. All strings, arrays, structures, and JSON trees associated with the module are allocated from this arena. When `flux_module_destroy` is called, the arena is destroyed in one operation. This eliminates the need for individual free operations and prevents memory leaks.

```c
flux_module *mod = flux_module_create("my_module");
// ... all allocations happen in mod->arena ...
flux_module_destroy(mod);  // frees everything
```

## Performance Characteristics

- **Allocation:** O(1) amortized. No free, no coalescing, no fragmentation.
- **Reset:** O(number of pages). Only frees pages after the first.
- **Memory overhead:** One pointer per page (`next`), plus `used`/`capacity` bookkeeping.
- **Cache behavior:** Sequential allocations within a page are cache-friendly.

## Constraints

- No per-object free. Memory is reclaimed only by reset or destroy.
- Pointer stability: pointers into the arena remain valid until the arena is reset or destroyed, because pages are never moved or reallocated.
- Not thread-safe. A single arena should be used by one thread at a time.
