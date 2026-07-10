# JSON Parser and Serializer

## Overview

The Flux JSON parser and serializer provide a complete implementation of JSON parsing and serialization, used internally for FluxIR (the canonical JSON representation of Flux modules). The parser is a hand-written recursive descent parser that operates directly on a null-terminated input string, producing an arena-allocated JSON value tree.

## Design Goals

- **Zero dependencies**: Uses only the C standard library (`string.h`, `stdio.h`, `stdlib.h`, `math.h`, `ctype.h`).
- **Arena-backed allocation**: All JSON values, strings, and arrays are allocated from a `flux_arena`, ensuring pointer stability and bulk deallocation.
- **Error reporting**: Tracks line and column numbers for parse errors, with descriptive messages.
- **Complete JSON support**: Handles objects, arrays, strings (with escape sequences including Unicode), numbers, booleans, and null.

## JSON Value Tree

### Value Kinds

```c
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} json_value_kind;
```

### Value Structure

```c
struct json_value {
    json_value_kind kind;
    union {
        int           boolean;
        double        number;
        char         *string;
        struct { json_value **items; int count; } array;
        struct { json_pair  *pairs; int count; } object;
    } u;
};
```

- **`JSON_NULL`**: Represents JSON `null`. The union is unused.
- **`JSON_BOOL`**: Represents `true` or `false`. `u.boolean` is 1 or 0.
- **`JSON_NUMBER`**: Represents a JSON number. `u.number` is a `double`. The parser preserves the exact textual representation by copying the number string into the arena before calling `atof`.
- **`JSON_STRING`**: Represents a JSON string. `u.string` is a null-terminated, arena-allocated copy with escape sequences decoded.
- **`JSON_ARRAY`**: Represents a JSON array. `u.array.items` is a dynamically-sized array of `json_value*`. `u.array.count` is the number of elements. The array grows using `flux_vec_grow` starting from a capacity of 64.
- **`JSON_OBJECT`**: Represents a JSON object. `u.object.pairs` is an array of `json_pair`. `u.object.count` is the number of key-value pairs. The pairs array grows using `flux_vec_grow` starting from a capacity of 64.

### Key-Value Pair

```c
typedef struct json_pair {
    char       *key;
    json_value *value;
} json_pair;
```

Keys are always null-terminated strings allocated from the arena.

## Parser

### Parser State

```c
typedef struct json_parser {
    const char *input;    // original input string
    const char *pos;      // current parse position
    int         line;     // current line (1-based)
    int         column;   // current column (1-based)
    flux_arena *arena;    // arena for all allocations
    flux_error *error;    // error output
} json_parser;
```

### Parsing Functions

#### `json_value* json_parse(const char *input, flux_arena *arena, flux_error *err)`

Top-level parse entry point. Parses a complete JSON value from `input` and returns the root `json_value`. After parsing, the parser skips trailing whitespace and rejects any trailing non-whitespace data.

- **Returns:** Root JSON value, or `NULL` on parse error.
- **Error handling:** On error, `err` is populated with line, column, status (`FLUX_ERR_PARSE`), and a descriptive message.

#### `json_value* json_parse_value(json_parser *p)`

Parses any JSON value (object, array, string, number, boolean, or null) from the current parser position.

#### `json_value* json_parse_object(json_parser *p)`

Parses a JSON object `{ ... }`. Expects a sequence of `"key": value` pairs separated by commas.

#### `json_value* json_parse_array(json_parser *p)`

Parses a JSON array `[ ... ]`. Expects a sequence of JSON values separated by commas.

#### `char* json_parse_string(json_parser *p)`

Parses a JSON string (including the surrounding double quotes). Returns a newly allocated, null-terminated string with escape sequences decoded.

Supported escape sequences:
- `\"` -> `"`
- `\\` -> `\`
- `\/` -> `/`
- `\b` -> backspace
- `\f` -> form feed
- `\n` -> newline
- `\r` -> carriage return
- `\t` -> tab
- `\uXXXX` -> Unicode code point (codepoints < 128 are decoded to ASCII; codepoints < 2048 are decoded to 2-byte UTF-8; larger codepoints are decoded to 3-byte UTF-8)

The parser performs a two-pass decode:
1. First pass: scan the string to compute the decoded length.
2. Second pass: copy and decode characters into the allocated buffer.

#### `json_value* json_parse_number(json_parser *p)`

Parses a JSON number. Supports optional leading `-`, decimal point, and scientific notation (`e`/`E` with optional `+`/`-`). The number text is copied into the arena, then converted via `atof`.

### Whitespace and Error Handling

#### `void json_skip_ws(json_parser *p)`

Advances past all whitespace characters (space, tab, newline, carriage return). Newlines increment the line counter and reset the column to 1.

#### `int json_expect(json_parser *p, char c)`

Skips whitespace and checks for an expected character. Returns 1 if found, 0 otherwise.

#### `void json_error(json_parser *p, const char *fmt, ...)`

Records a parse error. Sets `err->line`, `err->column`, `err->status` (`FLUX_ERR_PARSE`), and formats the message into `err->message`.

## Serializer

The serializer converts a `json_value` tree back into a null-terminated JSON string.

### `char* json_serialize_value(json_value *v, flux_arena *arena)`

Serializes a JSON value tree into a compact JSON string allocated from `arena`.

- **Returns:** Null-terminated JSON string, or `NULL` on allocation failure.

### `void json_serialize_value_buf(json_value *v, char **buf, size_t *len, size_t *cap, flux_arena *arena)`

Serializes into a growable buffer. This is the recursive workhorse used by `json_serialize_value`. The buffer grows by doubling its capacity when needed.

### String Escaping

`json_escape_string` handles all necessary JSON string escaping:
- `"` -> `\"`
- `\` -> `\\`
- Control characters (`\b`, `\f`, `\n`, `\r`, `\t`) are escaped to their short forms.
- Other characters with codepoint < 0x20 are escaped as `\uXXXX`.

Numbers are serialized without quotes. Integer-valued doubles (within safe integer range) are formatted as integers; all others use `%.17g` to preserve precision.

## Helper Accessors

These functions provide typed access to JSON values, primarily used by the FluxIR parser:

| Function | Description |
|----------|-------------|
| `const char* json_as_string(json_value *v)` | Returns string value, or `NULL` if not a string. |
| `double json_as_number(json_value *v)` | Returns number value, or 0.0 if not a number. |
| `int json_as_bool(json_value *v)` | Returns boolean value (0 or 1), or 0 if not a boolean. |
| `int json_is_null(json_value *v)` | Returns non-zero if `v` is `NULL` or `JSON_NULL`. |
| `json_value* json_object_get(json_value *obj, const char *key)` | Looks up a key in an object. Returns `NULL` if not found or not an object. |
| `json_value* json_array_get(json_value *arr, int index)` | Returns element at `index`, or `NULL` on bounds error or if not an array. |
| `int json_array_count(json_value *arr)` | Returns number of elements, or 0 if not an array. |
| `int json_object_count(json_value *obj)` | Returns number of pairs, or 0 if not an object. |
| `const char* json_object_key(json_value *obj, int index)` | Returns key at `index`, or `NULL` on bounds error. |
| `json_value* json_object_value(json_value *obj, int index)` | Returns value at `index`, or `NULL` on bounds error. |

## Memory Model

All JSON values, strings, arrays, and object pairs are allocated from the provided `flux_arena`. There is no per-value free operation. The entire JSON tree is reclaimed when the arena is reset or destroyed. This makes the JSON API safe for long-running processes that parse many modules sequentially.

## Limitations

- **No streaming**: The parser requires the entire JSON document in memory.
- **No incremental parsing**: A complete value must be parsed in one call.
- **Limited Unicode**: `\uXXXX` escapes are decoded to UTF-8 only for codepoints up to 0xFFFF. Surrogate pairs are not combined.
- **No number precision tracking**: All numbers are stored as `double`. For exact integer parsing (e.g., for 64-bit integers), the caller should parse the number string directly if the JSON parser is used outside of FluxIR.
