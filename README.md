# Flux — FluxASM / FluxIR Runtime

Pure C99 implementation of the FluxASM assembly language and FluxIR intermediate
representation, with zero external dependencies. Fully hardware/architecture
agnostic — runs on anything with a C99 compiler.

## What is Flux?

Flux is a **dataflow-oriented intermediate representation** designed for
heterogeneous compute pipelines. A Flux program is a set of *waves* (execution
stages) containing *units* (basic blocks of dataflow instructions), connected by
a *dependency graph* that describes how data moves between them.

- **FluxASM** — human-readable assembly syntax
- **FluxIR** — canonical JSON representation (lossless, round-trips with ASM)
- **Reference simulator** — executes Flux programs in the host process
- **Validator** — structural, type, dependency, effect, and control-safety checks

## Current Status

- **P0 Memory Safety & Parser Safety**: Complete. Arena-backed `flux_vec_grow()`
  with overflow checks. Removed artificial 64-unit limit. All dynamic arrays in
  FluxASM parser use safe growth.
- **P1 Semantic Validation**: Mostly complete. Type equality, dependency
  validation, and cycle detection implemented. 6 remaining test failures in
  JSON parsing of multi-string literal test inputs.
- **P2-P4 API & Simulator**: Core API stable. Reference simulator implemented.
  JSON parser column tracking fixed.
- **Test Suite**: 22/28 tests passing.

## Quick Start

```sh
make
```

Produces `build/libflux.a` (library), `build/flux` (CLI), and `build/test_flux`
(test runner).

### CLI Usage

```sh
# Convert FluxASM → FluxIR (JSON)
./build/flux asm2json program.flux

# Convert FluxIR → FluxIR (pretty-print)
./build/flux ir2json program.json

# Round-trip FluxASM → FluxASM (parse and print)
./build/flux asm2asm program.flux

# Validate a FluxIR module
./build/flux validate program.json

# Run a FluxIR module in the reference simulator
./build/flux run program.json
```

### Test Suite

```sh
./build/test_flux
```

## Project Structure

```
├── src/
│   ├── flux.h           # Public API and type definitions
│   ├── flux_internal.h  # Internal types (JSON value tree, parser state)
│   ├── flux_arena.c     # Page-list arena allocator, type system, module lifecycle
│   ├── flux_json.c      # JSON parser / serializer
│   ├── flux_ir.c        # FluxIR (JSON) ⇄ flux_module conversion
│   ├── flux_asm.c       # FluxASM parser / pretty-printer
│   ├── flux_val.c       # Structural, type, dependency, effect, control validator
│   ├── flux_exec.c      # Reference dataflow simulator
│   └── flux_vec.h       # Dynamic array helpers (arena-backed)
├── testing/
│   ├── main.c           # CLI entry point
│   └── test_flux.c      # Test suite (arena, type, JSON, IR, ASM, validation)
├── docs/
│   └── FluxASM_FluxIR_Technical_Specification_v1.1_Minimalist.pdf
├── Makefile
└── README.md
```

## Design Goals

- **Zero dependencies** — no libc beyond standard C99 math, no external
  libraries, single `libflux.a`
- **Portable** — strict C99, no platform-specific code, no architecture-specific
  assumptions
- **Arena allocation** — page-list arena for all dynamic memory; fast
  allocation, no per-object free, bulk reset
- **Lossless round-trip** — FluxASM ⇒ FluxIR ⇒ FluxASM produces semantically
  identical output

## Building

Requires a C99 compiler (`gcc`, `clang`, `tcc`, etc.) and `make`.

```
make          # build library, CLI, and tests
make clean    # remove build artifacts
```

## License

MIT
