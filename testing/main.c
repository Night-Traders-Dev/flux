/* Flux main CLI - parse, validate, and execute FluxIR/FluxASM programs */
#include "flux.h"
#include <stdio.h>
#include <string.h>

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <command> [args]\n\n", prog);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  ir2json <file>     Parse FluxIR JSON, validate, re-serialize\n");
    fprintf(stderr, "  asm2json <file>    Parse FluxASM assembly, convert to JSON\n");
    fprintf(stderr, "  asm2asm  <file>    Parse FluxASM, validate, re-print\n");
    fprintf(stderr, "  validate <file>    Parse and validate a FluxIR file\n");
    fprintf(stderr, "  run      <file>    Parse and simulate a FluxIR program\n");
    fprintf(stderr, "  help               Show this help\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc < 3) {
        fprintf(stderr, "Error: missing file argument\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *filepath = argv[2];
    flux_module *mod = flux_module_create("flux_cli");
    if (!mod) {
        fprintf(stderr, "Error: failed to create module\n");
        return 1;
    }

    flux_error err;
    flux_status status;

    if (strcmp(cmd, "ir2json") == 0 || strcmp(cmd, "validate") == 0 || strcmp(cmd, "run") == 0) {
        status = flux_ir_parse_file(filepath, mod, &err);
        if (status != FLUX_OK) {
            fprintf(stderr, "Error parsing IR: %s (line %d, col %d)\n",
                    err.message, err.line, err.column);
            flux_module_destroy(mod);
            return 1;
        }
    } else if (strcmp(cmd, "asm2json") == 0 || strcmp(cmd, "asm2asm") == 0) {
        status = flux_asm_parse_file(filepath, mod, &err);
        if (status != FLUX_OK) {
            fprintf(stderr, "Error parsing ASM: %s (line %d, col %d)\n",
                    err.message, err.line, err.column);
            flux_module_destroy(mod);
            return 1;
        }
    } else {
        fprintf(stderr, "Error: unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        flux_module_destroy(mod);
        return 1;
    }

    /* Validate */
    flux_validation val;
    val.num_errors = 0;
    val.descriptions = NULL;

    status = flux_validate(mod, &val, &err);
    if (status == FLUX_ERR_VALIDATION || val.num_errors > 0) {
        fprintf(stderr, "Validation found %d error(s):\n", val.num_errors);
        if (val.descriptions) {
            fprintf(stderr, "%s", val.descriptions);
        }
        if (strcmp(cmd, "validate") == 0) {
            flux_module_destroy(mod);
            return (val.num_errors > 0) ? 1 : 0;
        }
        /* For other commands, continue with warnings */
    } else if (strcmp(cmd, "validate") == 0) {
        printf("Validation passed: no errors\n");
        flux_module_destroy(mod);
        return 0;
    }

    if (strcmp(cmd, "ir2json") == 0) {
        char *json = flux_ir_serialize(mod, &err);
        if (json) {
            printf("%s\n", json);
        } else {
            fprintf(stderr, "Error serializing: %s\n", err.message);
            flux_module_destroy(mod);
            return 1;
        }
    } else if (strcmp(cmd, "asm2json") == 0) {
        char *json = flux_ir_serialize(mod, &err);
        if (json) {
            printf("%s\n", json);
        } else {
            fprintf(stderr, "Error serializing: %s\n", err.message);
            flux_module_destroy(mod);
            return 1;
        }
    } else if (strcmp(cmd, "asm2asm") == 0) {
        char *asm_out = flux_asm_print(mod, &err);
        if (asm_out) {
            printf("%s", asm_out);
        } else {
            fprintf(stderr, "Error printing: %s\n", err.message);
            flux_module_destroy(mod);
            return 1;
        }
    } else if (strcmp(cmd, "run") == 0) {
        /* Find first entry point */
        if (mod->num_entry_points == 0) {
            fprintf(stderr, "Error: no entry points defined\n");
            flux_module_destroy(mod);
            return 1;
        }

        flux_arena *sim_arena = flux_arena_create(65536);
        if (!sim_arena) {
            fprintf(stderr, "Error: out of memory\n");
            flux_module_destroy(mod);
            return 1;
        }

        flux_sim_state *sim = flux_sim_create(mod, sim_arena);
        if (!sim) {
            fprintf(stderr, "Error: failed to create simulator\n");
            flux_arena_destroy(sim_arena);
            flux_module_destroy(mod);
            return 1;
        }

        status = flux_sim_run(sim, mod->entry_points[0].start_wave, 0, NULL, &err);
        if (status != FLUX_OK) {
            fprintf(stderr, "Error running simulation: %s\n", err.message);
        } else {
            printf("Simulation completed successfully\n");
        }

        flux_sim_destroy(sim);
        flux_arena_destroy(sim_arena);
    }

    flux_module_destroy(mod);
    return 0;
}
