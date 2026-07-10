# Flux Documentation

This directory contains detailed documentation for each component of the Flux runtime.

## Components

### [Arena Allocator](arena.md)

The region-based memory allocator that backs all Flux objects. Uses a linked list of fixed-size pages to provide fast, bulk allocation without per-object free operations.

### [Type System](type-system.md)

The Flux type system, covering scalar types, vector types, predicates, and effect tokens. Includes parsing, formatting, and type equality rules.

### [JSON Parser and Serializer](json.md)

The hand-written recursive descent JSON parser and serializer used for FluxIR interchange. Covers the JSON value tree, parsing functions, error handling, and serialization.

### [FluxIR — Intermediate Representation](fluxir.md)

The canonical JSON representation of Flux modules. Documents the full module schema including entry points, types, registers, devices, memory regions, waves, units, instructions, dependencies, and control graphs.

### [FluxASM — Assembly Language](fluxasm.md)

The human-readable assembly language for Flux. Documents directives, instructions, operands, register naming conventions, and the pretty-print format.

### [Validator](validator.md)

The structural, type, dependency, effect, and control-safety validation engine. Documents all validation rules, error messages, and the validation API.

### [Reference Simulator](simulator.md)

The host-process interpreter for Flux programs. Documents the execution model, supported opcodes, branch semantics, API, and current limitations.
