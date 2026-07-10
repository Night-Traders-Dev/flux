/* Flux test suite */
#include "flux.h"
#include "flux_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  TEST: %s ... ", name); \
    tests_run++; \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("  ASSERT FAIL: %s at line %d\n", msg, __LINE__); FAIL(msg); return; } \
} while(0)

/*===========================================================================
 * Test: Type parsing
 *===========================================================================*/
static void test_type_parse(void)
{
    flux_type t;

    ASSERT(flux_type_parse("i32", &t), "i32 should parse");
    ASSERT(t.kind == FLUX_KIND_SCALAR, "i32 kind");
    ASSERT(t.bits == 32, "i32 bits");
    ASSERT(t.sign == FLUX_SIGN_SIGNED, "i32 sign");
    ASSERT(t.is_float == 0, "i32 not float");

    ASSERT(flux_type_parse("u8", &t), "u8 should parse");
    ASSERT(t.kind == FLUX_KIND_SCALAR, "u8 kind");
    ASSERT(t.bits == 8, "u8 bits");
    ASSERT(t.sign == FLUX_SIGN_UNSIGNED, "u8 sign");

    ASSERT(flux_type_parse("f64", &t), "f64 should parse");
    ASSERT(t.kind == FLUX_KIND_SCALAR, "f64 kind");
    ASSERT(t.bits == 64, "f64 bits");
    ASSERT(t.is_float == 1, "f64 float");

    ASSERT(flux_type_parse("pred", &t), "pred should parse");
    ASSERT(t.kind == FLUX_KIND_PREDICATE, "pred kind");

    ASSERT(flux_type_parse("effect", &t), "effect should parse");
    ASSERT(t.kind == FLUX_KIND_EFFECT, "effect kind");

    ASSERT(flux_type_parse("v4<f32>", &t), "v4<f32> should parse");
    ASSERT(t.kind == FLUX_KIND_VECTOR, "v4<f32> kind");
    ASSERT(t.lanes == 4, "v4<f32> lanes");
    ASSERT(t.bits == 32, "v4<f32> elem bits");
    ASSERT(t.is_float == 1, "v4<f32> float");

    PASS();
}

/*===========================================================================
 * Test: Type formatting
 *===========================================================================*/
static void test_type_format(void)
{
    char buf[64];
    flux_type t;

    t = flux_type_scalar(32, FLUX_SIGN_SIGNED, 0);
    flux_type_format(&t, buf, sizeof(buf));
    ASSERT(strcmp(buf, "i32") == 0, "format i32");

    t = flux_type_scalar(8, FLUX_SIGN_UNSIGNED, 0);
    flux_type_format(&t, buf, sizeof(buf));
    ASSERT(strcmp(buf, "u8") == 0, "format u8");

    t = flux_type_scalar(64, FLUX_SIGN_SIGNED, 1);
    flux_type_format(&t, buf, sizeof(buf));
    ASSERT(strcmp(buf, "f64") == 0, "format f64");

    t = flux_type_pred();
    flux_type_format(&t, buf, sizeof(buf));
    ASSERT(strcmp(buf, "pred") == 0, "format pred");

    t = flux_type_effect();
    flux_type_format(&t, buf, sizeof(buf));
    ASSERT(strcmp(buf, "effect") == 0, "format effect");

    t = flux_type_vector(4, 32, FLUX_SIGN_SIGNED, 1);
    flux_type_format(&t, buf, sizeof(buf));
    ASSERT(strcmp(buf, "v4<f32>") == 0, "format v4<f32>");

    PASS();
}

/*===========================================================================
 * Test: JSON parser basics
 *===========================================================================*/
static void test_json_parse(void)
{
    flux_arena *a = flux_arena_create(65536);
    ASSERT(a != NULL, "arena created");
    flux_error err;
    flux_error_init(&err);

    /* Parse null */
    json_value *v = json_parse("null", a, &err);
    ASSERT(v != NULL, "null parse");
    ASSERT(v->kind == JSON_NULL, "null kind");

    /* Parse boolean */
    v = json_parse("true", a, &err);
    ASSERT(v != NULL, "true parse");
    ASSERT(v->kind == JSON_BOOL, "true kind");
    ASSERT(v->u.boolean == 1, "true value");

    v = json_parse("false", a, &err);
    ASSERT(v != NULL, "false parse");
    ASSERT(v->kind == JSON_BOOL, "false kind");
    ASSERT(v->u.boolean == 0, "false value");

    /* Parse number */
    v = json_parse("42", a, &err);
    ASSERT(v != NULL, "number parse");
    ASSERT(v->kind == JSON_NUMBER, "number kind");
    ASSERT(v->u.number == 42.0, "number value");

    v = json_parse("-3.14", a, &err);
    ASSERT(v != NULL, "negative float parse");
    ASSERT(v->u.number < -3.0, "negative float value");

    /* Parse string */
    v = json_parse("\"hello\"", a, &err);
    ASSERT(v != NULL, "string parse");
    ASSERT(v->kind == JSON_STRING, "string kind");
    ASSERT(strcmp(v->u.string, "hello") == 0, "string value");

    /* Parse array */
    v = json_parse("[1, 2, 3]", a, &err);
    ASSERT(v != NULL, "array parse");
    ASSERT(v->kind == JSON_ARRAY, "array kind");
    ASSERT(v->u.array.count == 3, "array count");

    /* Parse object */
    v = json_parse("{\"a\": 1, \"b\": \"two\"}", a, &err);
    ASSERT(v != NULL, "object parse");
    ASSERT(v->kind == JSON_OBJECT, "object kind");
    ASSERT(v->u.object.count == 2, "object count");

    /* Serialize back */
    char *s = json_serialize_value(v, a);
    ASSERT(s != NULL, "serialize");

    flux_arena_destroy(a);
    PASS();
}

/*===========================================================================
 * Test: FluxIR round-trip with array_sum example from spec
 *===========================================================================*/
static void test_ir_roundtrip(void)
{
    /* The array_sum example from the spec */
    const char *json_input =
        "{"
        "\"module\": {"
        "\"name\": \"array_sum\","
        "\"version\": \"1.1\","
        "\"entry_points\": [{"
            "\"name\": \"main\","
            "\"params\": ["
                "{\"reg\": \"s_ptr\", \"type\": \"i32\"},"
                "{\"reg\": \"s_len\", \"type\": \"i32\"}"
            "],"
            "\"returns\": [],"
            "\"start_wave\": \"W0\""
        "}],"
        "\"types\": ["
            "{\"name\": \"i32\", \"kind\": \"scalar\", \"bits\": 32, \"sign\": \"signed\"},"
            "{\"name\": \"pred\", \"kind\": \"predicate\"},"
            "{\"name\": \"effect\", \"kind\": \"effect\"}"
        "],"
        "\"registers\": ["
            "{\"name\": \"s_ptr\", \"class\": \"scalar\", \"type\": \"i32\"},"
            "{\"name\": \"s_len\", \"class\": \"scalar\", \"type\": \"i32\"},"
            "{\"name\": \"s_acc\", \"class\": \"scalar\", \"type\": \"i32\"},"
            "{\"name\": \"s_index\", \"class\": \"scalar\", \"type\": \"i32\"},"
            "{\"name\": \"s_val\", \"class\": \"scalar\", \"type\": \"i32\"},"
            "{\"name\": \"s_newacc\", \"class\": \"scalar\", \"type\": \"i32\"},"
            "{\"name\": \"s_newindex\", \"class\": \"scalar\", \"type\": \"i32\"},"
            "{\"name\": \"p_cond\", \"class\": \"predicate\", \"type\": \"pred\"},"
            "{\"name\": \"e_mem\", \"class\": \"effect\", \"type\": \"effect\"},"
            "{\"name\": \"e_log\", \"class\": \"effect\", \"type\": \"effect\"}"
        "],"
        "\"devices\": ["
            "{\"name\": \"log\", \"kind\": \"generic\", \"properties\": {\"category\": \"logging\"}}"
        "],"
        "\"memory_regions\": ["
            "{\"name\": \"data_region\", \"kind\": \"global\", \"size\": 0, \"attributes\": {}}"
        "],"
        "\"waves\": ["
            "{\"name\": \"W0\", \"params\": [], \"units\": [\"U_bind\"]},"
            "{\"name\": \"Wloop\", \"params\": ["
                "{\"reg\": \"s_acc\", \"type\": \"i32\"},"
                "{\"reg\": \"s_index\", \"type\": \"i32\"}"
            "], \"units\": [\"U_body\", \"U_check\", \"U_branch\"]},"
            "{\"name\": \"Wend\", \"params\": [], \"units\": [\"U_log\"]}"
        "],"
        "\"units\": ["
            "{"
                "\"name\": \"U_bind\", \"wave\": \"W0\","
                "\"uses\": [\"s_ptr\", \"s_len\"],"
                "\"defs\": [\"e_mem\", \"e_log\", \"s_acc\", \"s_index\"],"
                "\"instructions\": ["
                    "{\"opcode\": \"bind_memory\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"e_mem\"},"
                        "{\"kind\": \"ref\", \"type\": \"memory_region\", \"name\": \"data_region\"}"
                    "]},"
                    "{\"opcode\": \"bind_device\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"e_log\"},"
                        "{\"kind\": \"ref\", \"type\": \"device\", \"name\": \"log\"}"
                    "]},"
                    "{\"opcode\": \"mov.s\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"s_acc\"},"
                        "{\"kind\": \"imm\", \"value\": 0}"
                    "]},"
                    "{\"opcode\": \"mov.s\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"s_index\"},"
                        "{\"kind\": \"imm\", \"value\": 0}"
                    "]}"
                "]"
            "},"
            "{"
                "\"name\": \"U_body\", \"wave\": \"Wloop\","
                "\"uses\": [\"s_acc\", \"s_index\", \"e_mem\", \"s_ptr\"],"
                "\"defs\": [\"s_val\", \"s_newacc\", \"s_newindex\"],"
                "\"instructions\": ["
                    "{\"opcode\": \"ld.s\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"s_val\"},"
                        "{\"kind\": \"reg\", \"name\": \"e_mem\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_ptr\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_index\"}"
                    "]},"
                    "{\"opcode\": \"add.s\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"s_newacc\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_acc\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_val\"}"
                    "]},"
                    "{\"opcode\": \"add.s\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"s_newindex\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_index\"},"
                        "{\"kind\": \"imm\", \"value\": 1}"
                    "]}"
                "]"
            "},"
            "{"
                "\"name\": \"U_check\", \"wave\": \"Wloop\","
                "\"uses\": [\"s_newindex\", \"s_len\"],"
                "\"defs\": [\"p_cond\"],"
                "\"instructions\": ["
                    "{\"opcode\": \"cmp.s\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"p_cond\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_newindex\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_len\"},"
                        "{\"kind\": \"imm\", \"value\": \"LT\"}"
                    "]}"
                "]"
            "},"
            "{"
                "\"name\": \"U_branch\", \"wave\": \"Wloop\","
                "\"uses\": [\"p_cond\", \"s_newacc\", \"s_newindex\"],"
                "\"defs\": [],"
                "\"instructions\": ["
                    "{\"opcode\": \"branch\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"p_cond\"},"
                        "{\"kind\": \"wave\", \"name\": \"Wloop\", \"args\": {"
                            "\"s_acc\": \"s_newacc\", \"s_index\": \"s_newindex\""
                        "}},"
                        "{\"kind\": \"wave\", \"name\": \"Wend\", \"args\": {}}"
                    "]}"
                "]"
            "},"
            "{"
                "\"name\": \"U_log\", \"wave\": \"Wend\","
                "\"uses\": [\"s_acc\", \"e_log\"],"
                "\"defs\": [],"
                "\"instructions\": ["
                    "{\"opcode\": \"write\", \"operands\": ["
                        "{\"kind\": \"reg\", \"name\": \"e_log\"},"
                        "{\"kind\": \"reg\", \"name\": \"s_acc\"}"
                    "]},"
                    "{\"opcode\": \"end\", \"operands\": []}"
                "]"
            "}"
        "],"
        "\"deps\": ["
            "{\"from\": \"U_bind\", \"to\": \"U_body\", \"regs\": [\"e_mem\", \"s_ptr\", \"s_len\"]},"
            "{\"from\": \"U_body\", \"to\": \"U_check\", \"regs\": [\"s_newindex\"]},"
            "{\"from\": \"U_body\", \"to\": \"U_branch\", \"regs\": [\"s_newacc\", \"s_newindex\"]},"
            "{\"from\": \"U_check\", \"to\": \"U_branch\", \"regs\": [\"p_cond\"]}"
        "],"
        "\"control_graph\": ["
            "{\"from\": \"W0\", \"to\": [\"Wloop\"]},"
            "{\"from\": \"Wloop\", \"to\": [\"Wloop\", \"Wend\"]}"
        "]"
        "}}";

    flux_module *mod = flux_module_create("test");
    ASSERT(mod != NULL, "module created");

    flux_error err;
    flux_status status = flux_ir_parse(json_input, mod, &err);
    ASSERT(status == FLUX_OK, "parse array_sum");

    /* Check parsed data */
    ASSERT(mod->num_entry_points == 1, "one entry point");
    ASSERT(strcmp(mod->entry_points[0].name, "main") == 0, "entry name");
    ASSERT(strcmp(mod->entry_points[0].start_wave, "W0") == 0, "start wave");

    ASSERT(mod->num_types == 3, "three types");
    ASSERT(mod->num_registers == 10, "ten registers");
    ASSERT(mod->num_devices == 1, "one device");
    ASSERT(mod->num_memory_regions == 1, "one memory region");
    ASSERT(mod->num_waves == 3, "three waves");
    ASSERT(mod->num_units == 5, "five units");
    ASSERT(mod->num_deps == 4, "four deps");
    ASSERT(mod->num_control_edges == 2, "two control edges");

    /* Check waves */
    ASSERT(strcmp(mod->waves[0].name, "W0") == 0, "wave 0 name");
    ASSERT(mod->waves[0].num_params == 0, "W0 no params");
    ASSERT(strcmp(mod->waves[1].name, "Wloop") == 0, "wave 1 name");
    ASSERT(mod->waves[1].num_params == 2, "Wloop two params");
    ASSERT(strcmp(mod->waves[1].params[0].reg, "s_acc") == 0, "Wloop param s_acc");
    ASSERT(strcmp(mod->waves[1].params[1].reg, "s_index") == 0, "Wloop param s_index");

    /* Check units */
    ASSERT(strcmp(mod->units[0].name, "U_bind") == 0, "unit 0 name");
    ASSERT(mod->units[0].num_instructions == 4, "U_bind 4 instructions");
    ASSERT(mod->units[0].num_defs == 4, "U_bind 4 defs");

    ASSERT(strcmp(mod->units[3].name, "U_branch") == 0, "unit 3 name");
    ASSERT(mod->units[3].num_instructions == 1, "U_branch 1 instruction");
    ASSERT(mod->units[3].instructions[0].num_operands == 3, "branch 3 operands");

    /* Check branch operands */
    flux_instruction *br = &mod->units[3].instructions[0];
    ASSERT(br->operands[1].kind == FLUX_OP_WAVE, "branch target 1 is wave");
    ASSERT(strcmp(br->operands[1].u.wave.wave_name, "Wloop") == 0, "branch to Wloop");
    ASSERT(br->operands[1].u.wave.num_args == 2, "Wloop branch has 2 args");
    ASSERT(br->operands[2].kind == FLUX_OP_WAVE, "branch target 2 is wave");
    ASSERT(strcmp(br->operands[2].u.wave.wave_name, "Wend") == 0, "branch to Wend");

    /* Check deps */
    ASSERT(strcmp(mod->deps[0].from, "U_bind") == 0, "dep 0 from");
    ASSERT(strcmp(mod->deps[0].to, "U_body") == 0, "dep 0 to");
    ASSERT(mod->deps[0].num_regs == 3, "dep 0 regs count");

    /* Round-trip serialize */
    char *json_out = flux_ir_serialize(mod, &err);
    ASSERT(json_out != NULL, "serialize success");
    ASSERT(strlen(json_out) > 0, "serialized output non-empty");

    /* Validate */
    flux_validation val;
    val.num_errors = 0;
    val.descriptions = NULL;
    status = flux_validate(mod, &val, &err);
    ASSERT(status == FLUX_OK || val.num_errors == 0, "validation passed");

    /* Simulate */
    flux_arena *sim_arena = flux_arena_create(65536);
    ASSERT(sim_arena != NULL, "sim arena created");
    flux_sim_state *sim = flux_sim_create(mod, sim_arena);
    ASSERT(sim != NULL, "sim created");

    int64_t sim_args[2] = {100, 5};
    status = flux_sim_run(sim, "W0", 2, sim_args, &err);
    ASSERT(status == FLUX_OK, "simulation run");

    flux_sim_destroy(sim);
    flux_arena_destroy(sim_arena);
    flux_module_destroy(mod);
    PASS();
}

/*===========================================================================
 * Test: FluxASM parse
 *===========================================================================*/
static void test_asm_parse(void)
{
    const char *asm_input =
        ".wave W0\n"
        "  U_bind:\n"
        "    bind_memory e_mem, memory_region:data_region\n"
        "    mov.s s_acc, 0\n"
        "    mov.s s_index, 0\n"
        ".wave Wloop (s_acc:i32, s_index:i32)\n"
        "  U_body: uses: s_acc, s_index defs: s_val, s_newacc, s_newindex\n"
        "    add.s s_newacc, s_acc, s_val\n"
        "    add.s s_newindex, s_index, 1\n"
        ".dep U_bind -> U_body (e_mem, s_ptr, s_len)\n";

    flux_module *mod = flux_module_create("test_asm");
    ASSERT(mod != NULL, "module created");

    flux_error err;
    flux_status status = flux_asm_parse(asm_input, mod, &err);
    if (status != FLUX_OK) {
        printf("ASM parse error: %s (line %d, col %d)\n", err.message, err.line, err.column);
        ASSERT(0, "asm parse");
    }
    ASSERT(status == FLUX_OK, "asm parse");

    /* Serialize back to ASM */
    char *asm_out = flux_asm_print(mod, &err);
    ASSERT(asm_out != NULL, "asm print");
    ASSERT(strlen(asm_out) > 0, "asm printed");

    flux_module_destroy(mod);
    PASS();
}

/*===========================================================================
 * Test: Validation with errors
 *===========================================================================*/
static void test_validation_errors(void)
{
    const char *bad_json =
        "{"
        "\"module\": {"
        "\"name\": \"bad\","
        "\"version\": \"1.1\","
        "\"entry_points\": [{"
            "\"name\": \"main\", \"params\": [], \"returns\": [], \"start_wave\": \"MissingWave\""
        "}],"
        "\"waves\": ["
            "{\"name\": \"W0\", \"params\": [], \"units\": [\"U_bad\"]}"
        "],"
        "\"units\": []"
        "}}";

    flux_module *mod = flux_module_create("bad_test");
    ASSERT(mod != NULL, "module created");

    flux_error err;
    flux_status status = flux_ir_parse(bad_json, mod, &err);
    ASSERT(status == FLUX_OK, "parse bad json");

    flux_validation val;
    val.num_errors = 0;
    val.descriptions = NULL;
    status = flux_validate(mod, &val, &err);
    ASSERT(status == FLUX_ERR_VALIDATION || val.num_errors > 0, "validation should find errors");
    if (val.num_errors > 0) {
        ASSERT(val.descriptions != NULL, "validation has descriptions");
    }

    flux_module_destroy(mod);
    PASS();
}

/*===========================================================================
 * Test: Arena allocator
 *===========================================================================*/
static void test_arena(void)
{
    flux_arena *a = flux_arena_create(256);
    ASSERT(a != NULL, "arena created");

    void *p1 = flux_arena_alloc(a, 32);
    ASSERT(p1 != NULL, "alloc 32 bytes");

    char *s1 = flux_arena_strdup(a, "hello world");
    ASSERT(s1 != NULL, "strdup");
    ASSERT(strcmp(s1, "hello world") == 0, "strdup value");

    size_t used = flux_arena_used(a);
    ASSERT(used > 0, "arena used > 0");

    flux_arena_reset(a);
    ASSERT(flux_arena_used(a) == 0, "arena reset to 0");

    /* After reset, we can allocate again */
    void *p2 = flux_arena_alloc(a, 64);
    ASSERT(p2 != NULL, "alloc after reset");

    flux_arena_destroy(a);
    PASS();
}

/*===========================================================================
 * Run all tests
 *===========================================================================*/
int main(void)
{
    printf("Flux Test Suite\n");
    printf("================\n\n");

    printf("Arena:\n");
    test_arena();

    printf("Type System:\n");
    test_type_parse();
    test_type_format();

    printf("JSON:\n");
    test_json_parse();

    printf("FluxIR:\n");
    test_ir_roundtrip();

    printf("FluxASM:\n");
    test_asm_parse();

    printf("Validation:\n");
    test_validation_errors();

    printf("\n================\n");
    printf("Results: %d/%d passed, %d failed, %d total\n",
           tests_passed, tests_run, tests_failed, tests_run);
    printf("================\n");

    return tests_failed > 0 ? 1 : 0;
}
