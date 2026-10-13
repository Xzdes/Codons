#include "internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static char directory[128];

#define REQUIRE(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static int32_t run0(BioProgram *p) {
    BioError e = {{0}};
    int32_t result;
    REQUIRE(bio_program_run(p, NULL, 0, &result, &e));
    return result;
}

static int32_t interpret0(BioProgram *p) {
    BioError e = {{0}};
    int32_t result;
    REQUIRE(bio_program_interpret(p, NULL, 0, &result, &e));
    return result;
}

static int32_t runtime0(BioRuntime *runtime) {
    BioError e = {{0}};
    int32_t result;
    REQUIRE(bio_runtime_run(runtime, NULL, 0, &result, &e));
    return result;
}

static void path_for(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s\\%s", directory, name);
}

static void write_source(const char *name, const char *source) {
    char path[256];
    FILE *f;
    path_for(path, sizeof(path), name);
    f = fopen(path, "wb");
    REQUIRE(f != NULL);
    REQUIRE(fwrite(source, 1, strlen(source), f) == strlen(source));
    REQUIRE(fclose(f) == 0);
}

static BioProgram *compile_name(const char *name, int repair, BioError *e) {
    char path[256];
    path_for(path, sizeof(path), name);
    return bio_compile(path, repair, e);
}

static void expect_value(const char *source, int32_t expected) {
    BioError e = {{0}};
    BioProgram *p;
    MEMORY_BASIC_INFORMATION memory;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION table;
    CONTEXT context = {0};
    DWORD64 fake_stack[BIO_STACK_SIZE / 8u + 3u] = {0}; /* frame + saved rsi/rbx + return */
    DWORD64 frame = 0;
    PVOID handler_data = NULL;
    write_source("case.dna", source);
    p = compile_name("case.dna", 0, &e);
    if (!p) fprintf(stderr, "%s\n", e.message);
    REQUIRE(p != NULL);
    REQUIRE(run0(p) == expected);
    REQUIRE(interpret0(p) == expected);
    REQUIRE(VirtualQuery(p->code, &memory, sizeof(memory)) == sizeof(memory));
    REQUIRE(memory.Protect == PAGE_EXECUTE_READ);
    table = RtlLookupFunctionEntry((DWORD64)(uintptr_t)(p->code + p->genes[p->main_gene].code_offset + 4), &base, NULL);
    REQUIRE(table != NULL);
    REQUIRE(base == (DWORD64)(uintptr_t)p->code);
    REQUIRE(table->BeginAddress == p->genes[p->main_gene].code_offset);
    /* Exercise Windows' actual unwinder after the JIT's prolog (6 bytes:
       push rbx; push rsi; sub rsp,104). The fake stack holds saved rsi and
       rbx above the frame, then the return address. */
    fake_stack[BIO_STACK_SIZE / 8u + 2u] = 0x12345678u;
    context.ContextFlags = CONTEXT_FULL;
    context.Rip = base + table->BeginAddress + 6;
    context.Rsp = (DWORD64)(uintptr_t)fake_stack;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, table, &context,
                     &handler_data, &frame, NULL);
    REQUIRE(context.Rip == fake_stack[BIO_STACK_SIZE / 8u + 2u]);
    REQUIRE(context.Rsp == (DWORD64)(uintptr_t)fake_stack + sizeof(fake_stack));
    bio_program_release(p);
}

static void expect_error(const char *source, const char *message) {
    BioError e = {{0}};
    BioProgram *p;
    write_source("bad.dna", source);
    p = compile_name("bad.dna", 0, &e);
    if (p || !strstr(e.message, message)) fprintf(stderr, "Expected '%s', got '%s'\n", message, e.message);
    REQUIRE(p == NULL);
    REQUIRE(strstr(e.message, message) != NULL);
}

static void language_tests(void) {
    expect_value("GENE main i32\nLOAD r0 30\nRET r0 _\nEND _ _\n", 30);
    expect_value("\xef\xbb\xbf# BOM and CRLF\r\n\r\nGENE\tmain\ti32/0\r\nLOAD r7 -2147483648 # min\r\nRET r7 _\r\nEND _ _", INT32_MIN);
    expect_value("GENE main i32\nLOAD r1 10\nLOAD r2 20\nCALL add r1,r2\nRET r0 _\nEND _ _\nGENE add i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n", 30);
    expect_value("GENE helper i32/1\nLOAD r7 3\nMUL r0 r7\nRET r0 _\nEND _ _\nGENE main i32\nLOAD r7 9\nCALL helper r7\nADD r0 r7\nRET r0 _\nEND _ _\n", 36);
    expect_value("GENE main i32\nCALL a _\nRET r0 _\nEND _ _\nGENE a i32\nCALL b _\nRET r0 _\nEND _ _\nGENE b i32\nLOAD r0 -7\nRET r0 _\nEND _ _\n", -7);
    expect_value("GENE main i32\nLOAD r0 2147483647\nLOAD r1 1\nADD r0 r1\nRET r0 _\nEND _ _\n", INT32_MIN);
    expect_value("GENE main i32\nLOAD r0 -2147483648\nLOAD r1 1\nSUB r0 r1\nRET r0 _\nEND _ _\n", INT32_MAX);
    expect_value("GENE main i32\nLOAD r7 -2147483648\nLOAD r6 -1\nMUL r7 r6\nMOV r2 r7\nRET r2 _\nEND _ _\n", INT32_MIN);
    expect_value("GENE main i32\nLOAD r0 10\nLOAD r1 3\nCALL diff r1,r0\nADD r0 r1\nRET r0 _\nEND _ _\nGENE diff i32/2\nSUB r0 r1\nRET r0 _\nEND _ _\n", -4);
    expect_error("GENE main i32\nLOAD r0 1\nRET r0\nEND _ _\n", "exactly 3 fields");
    expect_error("GENE main i32\nCALL f r0, r1\n", "exactly 3 fields");
    expect_error("GENE main i32\nRET r0 _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32\nLOAD r8 1\n", "r0..r7");
    expect_error("GENE main i32\nLOAD r0 2147483648\n", "i32 literal");
    expect_error("GENE main i32\nLOAD r0 -2147483649\n", "i32 literal");
    expect_error("GENE main i32\nLOAD r0 12x\n", "i32 literal");
    expect_error("GENE main f64\n", "signature");
    expect_error("GENE main i32/1\nRET r1 _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32\nCALL missing _\nRET r0 _\nEND _ _\n", "undefined gene");
    expect_error("GENE main i32\nCALL main r0\nRET r0 _\nEND _ _\n", "expects 0 arguments");
    expect_error("GENE main i32\nCALL main _\nRET r0 _\nEND _ _\n", "recursive call cycle");
    expect_error("GENE main i32\nCALL a _\nRET r0 _\nEND _ _\nGENE a i32\nCALL main _\nRET r0 _\nEND _ _\n", "recursive call cycle");
    expect_error("GENE main i32\nLOAD r0 1\nRET r0 _\nLOAD r1 1\nEND _ _\n", "after RET");
    expect_error("GENE main i32\nLOAD r0 1\nEND _ _\n", "must end with RET");
    expect_error("GENE main i32\nLOAD r0 1\nRET r0 _\n", "missing END");
    expect_error("LOAD r0 1\n", "outside a GENE");
    expect_error("# empty program\n", "missing entry");
    expect_error("GENE main i32\nWAT r0 r0\n", "unknown opcode");
    expect_error("GENE main i32\nLOAD r0 1\nRET r0 _\nEND _ _\nGENE main i32\n", "duplicate gene");
    expect_error("GENE main i32\nGENE f i32\n", "nested GENE");
    expect_error("GENE main i32\nLOAD r0 1\nRET r0 0\n", "unused '_'");
    expect_error("GENE main i32\nCALL f r0,r1,r2,r3,r4\n", "at most four register arguments");
    expect_error("GENE main i32\nMOV r0 r1\nRET r0 _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32\nIMPORT f.dna _\n", "outside a GENE");
}

static void arithmetic_slot_tests(void) {
    static const struct { const char *name; int unary; int32_t separate, alias; } ops[] = {
        {"DIV", 0, 7, 1}, {"MOD", 0, 0, 0}, {"AND", 0, 1, 3},
        {"OR", 0, 23, 3}, {"XOR", 0, 22, 0}, {"NOT", 1, -22, -22},
        {"LAND", 0, 1, 1}, {"LOR", 0, 1, 1}, {"LXOR", 0, 0, 0},
        {"LNOT", 1, 0, 0}, {"SHL", 0, 168, 24}, {"SHR", 0, 2, 0}, {"SAR", 0, 2, 0}
    };
    unsigned op, dst, src;
    char text[512];
    BioError e = {{0}};
    for (op = 0; op < sizeof(ops) / sizeof(ops[0]); ++op) {
        for (dst = 0; dst < 8; ++dst) for (src = 0; src < (ops[op].unary ? 1u : 8u); ++src) {
            BioProgram *p;
            int32_t expected = ops[op].separate;
            if (ops[op].unary)
                snprintf(text, sizeof(text), "GENE main i32\nLOAD r%u 21\n%s r%u _\nRET r%u _\nEND _ _\n",
                         dst, ops[op].name, dst, dst);
            else {
                snprintf(text, sizeof(text), "GENE main i32\nLOAD r%u 21\nLOAD r%u 3\n%s r%u r%u\nRET r%u _\nEND _ _\n",
                         dst, src, ops[op].name, dst, src, dst);
                if (dst == src) expected = ops[op].alias;
            }
            write_source("arithmetic.dna", text);
            p = compile_name("arithmetic.dna", 0, &e); REQUIRE(p != NULL);
            REQUIRE(run0(p) == expected);
            REQUIRE(interpret0(p) == expected);
            bio_program_release(p);
        }
        if (ops[op].unary)
            snprintf(text, sizeof(text), "GENE main i32\n%s r7 _\nRET r7 _\nEND _ _\n", ops[op].name);
        else
            snprintf(text, sizeof(text), "GENE main i32/1\n%s r0 r7\nRET r0 _\nEND _ _\n", ops[op].name);
        expect_error(text, "uninitialized register");
    }
    expect_error("GENE main i32/1\nNOT r0 r1\nRET r0 _\nEND _ _\n", "unused '_'");
    expect_error("GENE main i32/1\nLNOT r0\nRET r0 _\nEND _ _\n", "exactly 3 fields");
    expect_error("GENE main i32/1\nDIV r0 3\nRET r0 _\nEND _ _\n", "second operand");
    expect_error("GENE main i32/1\nSHR r0 _\nRET r0 _\nEND _ _\n", "second operand");
}

static void expect_run_error(BioProgram *p, const int32_t *args, size_t argc, const char *message) {
    BioError native = {{0}}, interpreted = {{0}};
    int32_t result = 9876;
    REQUIRE(!bio_program_run(p, args, argc, &result, &native));
    REQUIRE(result == 9876);
    REQUIRE(!bio_program_interpret(p, args, argc, &result, &interpreted));
    REQUIRE(result == 9876);
    REQUIRE(strstr(native.message, message) != NULL);
    REQUIRE(!strcmp(native.message, interpreted.message));
}

typedef struct { BioProgram *program; volatile LONG failures; } ArithmeticWorkers;

static DWORD WINAPI arithmetic_reader(LPVOID arg) {
    ArithmeticWorkers *workers = arg;
    unsigned i;
    for (i = 0; i < 120; ++i) {
        int32_t args[2] = {i % 3 == 0 ? INT32_MIN : 12, i % 3 == 0 ? -1 : i % 3 == 1 ? 0 : 3};
        int32_t result = 99;
        BioError e = {{0}};
        int ok = bio_program_run(workers->program, args, 2, &result, &e);
        if (i % 3 == 2 ? (!ok || result != 4 || *e.message) :
            (ok || result != 99 || !strstr(e.message, i % 3 == 0 ? "division overflow" : "division by zero")))
            InterlockedIncrement(&workers->failures);
    }
    return 0;
}

static void arithmetic_error_tests(void) {
    const char *ops[] = {"DIV", "MOD", "SHL", "SHR", "SAR"};
    BioError e = {{0}};
    BioProgram *p;
    char source[768];
    int32_t args[2], result;
    unsigned i;
    for (i = 0; i < 5; ++i) {
        snprintf(source, sizeof(source), "GENE main i32/2\nCALL middle r0,r1\nRET r0 _\nEND _ _\n"
                 "GENE middle i32/2\nCALL work r0,r1\nRET r0 _\nEND _ _\n"
                 "GENE work i32/2\n%s r0 r1\nRET r0 _\nEND _ _\n", ops[i]);
        write_source("fault.dna", source);
        p = compile_name("fault.dna", 0, &e); REQUIRE(p != NULL);
        args[0] = 8; args[1] = i < 2 ? 0 : 32;
        expect_run_error(p, args, 2, i < 2 ? "division by zero" : "shift count must be in 0..31");
        if (i == 0) { args[0] = INT32_MIN; args[1] = -1; expect_run_error(p, args, 2, "division overflow"); }
        if (i >= 2) { args[1] = -1; expect_run_error(p, args, 2, "shift count"); }
        args[0] = 8; args[1] = 2;
        REQUIRE(bio_program_run(p, args, 2, &result, &e));
        REQUIRE(result == (i == 0 ? 4 : i == 1 ? 0 : i == 2 ? 32 : 2));
        REQUIRE(!*e.message);
        REQUIRE(bio_program_interpret(p, args, 2, &result, &e));
        REQUIRE(!*e.message);
        bio_program_release(p);
    }
    /* A spent budget takes precedence: DIV must not execute at all. */
    write_source("fault.dna", "GENE main i32/1\nLOAD r1 1\nLABEL loop _\nSUB r0 r1\nJNZ r0 loop\n"
                 "LOAD r2 0\nDIV r0 r2\nRET r0 _\nEND _ _\n");
    p = compile_name("fault.dna", 0, &e); REQUIRE(p != NULL);
    args[0] = 499999; expect_run_error(p, args, 1, "execution budget exhausted");
    args[0] = 2; expect_run_error(p, args, 1, "division by zero");
    bio_program_release(p);
    {
        ArithmeticWorkers workers = {0};
        HANDLE threads[4];
        write_source("fault.dna", "GENE main i32/2\nDIV r0 r1\nRET r0 _\nEND _ _\n");
        workers.program = compile_name("fault.dna", 0, &e); REQUIRE(workers.program != NULL);
        for (i = 0; i < 4; ++i) { threads[i] = CreateThread(NULL, 0, arithmetic_reader, &workers, 0, NULL); REQUIRE(threads[i] != NULL); }
        REQUIRE(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
        for (i = 0; i < 4; ++i) CloseHandle(threads[i]);
        REQUIRE(workers.failures == 0);
        bio_program_release(workers.program);
    }
}

static void memory_slot_tests(void) {
    unsigned index, value, dst;
    char source[768];
    for (index = 0; index < 8; ++index) for (value = 0; value < 8; ++value) for (dst = 0; dst < 8; ++dst) {
        /* Exercise SIB addressing with all register aliases and a nonzero local offset. */
        snprintf(source, sizeof(source), "GENE main i32\nVAR before 99\nARRAY items 8\nVAR after 77\n"
                 "LOAD r%u 5\nLOAD r%u 3\nASET items r%u,r%u\nAGET r%u items,r%u\nRET r%u _\nEND _ _\n",
                 value, index, index, value, dst, index, dst);
        expect_value(source, index == value ? 3 : 5);
    }
    for (dst = 0; dst < 8; ++dst) {
        snprintf(source, sizeof(source), "GENE main i32\nVAR x -2147483648\nARRAY items 17\nVAR y 0\n"
                 "GET r%u x\nSET y r%u\nLEN r%u items\nGET r%u y\nRET r%u _\nEND _ _\n", dst, dst, dst, dst, dst);
        expect_value(source, INT32_MIN);
        snprintf(source, sizeof(source), "GENE main i32\nARRAY items 17\nLEN r%u items\nRET r%u _\nEND _ _\n", dst, dst);
        expect_value(source, 17);
    }
    expect_value("GENE main i32\nVAR before 99\nARRAY a 2\nARRAY b 2\nVAR after 77\n"
                 "LOAD r1 1\nLOAD r2 -1\nASET a r1,r2\nLOAD r1 0\nAGET r0 b,r1\n"
                 "GET r3 before\nADD r0 r3\nGET r3 after\nADD r0 r3\nRET r0 _\nEND _ _\n", 176);
    expect_value("GENE main i32\nARRAY items 65536\nLOAD r7 65535\nLOAD r6 2147483647\n"
                 "ASET items r7,r6\nAGET r7 items,r7\nRET r7 _\nEND _ _\n", INT32_MAX);
}

static void memory_error_tests(void) {
    static const struct { const char *body, *message; } invalid[] = {
        {"VAR x 0\nVAR x 1\n", "duplicate local"}, {"VAR x 0\nARRAY x 2\n", "duplicate local"},
        {"ARRAY a 0\n", "length must be"}, {"ARRAY a -1\n", "length must be"},
        {"ARRAY a 65537\n", "length must be"}, {"ARRAY a r0\n", "i32 literal"},
        {"VAR x 2147483648\n", "i32 literal"}, {"VAR x -2147483649\n", "i32 literal"},
        {"VAR r0 0\n", "invalid local name"}, {"VAR _ 0\n", "invalid local name"},
        {"VAR 2x 0\n", "invalid local name"}, {"LOAD r0 0\nVAR x 0\n", "must precede"},
        {"LABEL here _\nARRAY a 1\n", "must precede"}, {"VAR x\n", "exactly 3 fields"},
        {"GET r0 missing\n", "undefined local"}, {"ARRAY a 1\nGET r0 a\n", "requires a variable"},
        {"ARRAY a 1\nSET a r0\n", "requires a variable"},
        {"VAR x 0\nLEN r0 x\n", "requires an array"}, {"VAR x 0\nAGET r0 x,r0\n", "requires an array"},
        {"VAR x 0\nASET x r0,r0\n", "requires an array"},
        {"ARRAY a 1\nAGET r0 a,r8\n", "invalid AGET operands"},
        {"ARRAY a 1\nASET a r0,r1,r2\n", "invalid ASET operands"},
        {"ARRAY a 1\nAGET r0 a\n", "comma-separated"},
        {"ARRAY a 1\nASET a r0, r1\n", "exactly 3 fields"},
        {"VAR x 0\nSET x r7\n", "uninitialized register r7"},
        {"ARRAY a 1\nAGET r7 a,r7\n", "uninitialized register r7"},
        {"ARRAY a 1\nLOAD r0 0\nASET a r0,r7\n", "uninitialized register r7"},
        {"ARRAY a 1\nLOAD r0 0\nASET a r7,r0\n", "uninitialized register r7"},
        {"ARRAY a 65536\nVAR x 0\n", "local storage limit"}
    };
    static const int32_t bad_indices[] = {-1, INT32_MIN, 3, INT32_MAX};
    BioError e = {{0}};
    unsigned i, store;
    char source[1024];
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        snprintf(source, sizeof(source), "GENE main i32\n%sLOAD r0 0\nRET r0 _\nEND _ _\n", invalid[i].body);
        expect_error(source, invalid[i].message);
    }
    expect_error("VAR x 0\n", "outside a GENE");
    expect_error("GENE main i32\nVAR x 0\nCALL other _\nRET r0 _\nEND _ _\n"
                 "GENE other i32\nGET r0 x\nRET r0 _\nEND _ _\n", "undefined local x");
    for (store = 0; store < 2; ++store) {
        BioProgram *p;
        int32_t arg, result;
        snprintf(source, sizeof(source), "GENE main i32/1\nVAR x 99\nCALL child r0\nRET r0 _\nEND _ _\n"
                 "GENE child i32/1\nARRAY a 3\nLOAD r1 42\n%s\nRET r0 _\nEND _ _\n",
                 store ? "ASET a r0,r1" : "AGET r0 a,r0");
        write_source("bounds.dna", source); p = compile_name("bounds.dna", 0, &e); REQUIRE(p != NULL);
        for (i = 0; i < sizeof(bad_indices) / sizeof(bad_indices[0]); ++i) {
            char message[80];
            arg = bad_indices[i];
            snprintf(message, sizeof(message), "array index %ld out of bounds", (long)arg);
            expect_run_error(p, &arg, 1, message);
        }
        arg = 2;
        REQUIRE(bio_program_run(p, &arg, 1, &result, &e) && result == (store ? 2 : 0));
        REQUIRE(!*e.message);
        bio_program_release(p);
    }
}

static void memory_call_and_budget_tests(void) {
    char source[8192], name[32];
    size_t length;
    unsigned i, count;
    BioError e = {{0}};
    BioProgram *p;
    int32_t arg, result;
    expect_value("GENE main i32\nVAR x 100\nARRAY a 1\nLOAD r1 0\nLOAD r2 9\nASET a r1,r2\n"
                 "CALL child _\nMOV r7 r0\nCALL child _\nADD r7 r0\nGET r0 x\nADD r7 r0\n"
                 "AGET r0 a,r1\nADD r0 r7\nRET r0 _\nEND _ _\n"
                 "GENE child i32\nVAR x -5\nARRAY a 1\nLOAD r1 0\nAGET r0 a,r1\n"
                 "GET r2 x\nADD r0 r2\nLOAD r3 99\nSET x r3\nASET a r1,r3\nRET r0 _\nEND _ _\n", 99);
    for (count = 64; count <= 65; ++count) {
        length = (size_t)snprintf(source, sizeof(source), "GENE main i32\n");
        for (i = 0; i < count; ++i) length += (size_t)snprintf(source + length, sizeof(source) - length, "VAR v%u %u\n", i, i);
        snprintf(source + length, sizeof(source) - length, "GET r0 v63\nRET r0 _\nEND _ _\n");
        if (count == 64) expect_value(source, 63); else expect_error(source, "local declaration limit");
    }
    /* Four full-size frames fit exactly; the fifth exceeds the invocation limit. */
    for (count = 4; count <= 5; ++count) {
        length = 0;
        for (i = 0; i < count; ++i) {
            if (!i) strcpy(name, "main"); else snprintf(name, sizeof(name), "f%u", i);
            length += (size_t)snprintf(source + length, sizeof(source) - length, "GENE %s i32\nARRAY a 65536\n", name);
            if (i + 1 < count) length += (size_t)snprintf(source + length, sizeof(source) - length, "CALL f%u _\n", i + 1);
            else length += (size_t)snprintf(source + length, sizeof(source) - length, "LEN r0 a\n");
            length += (size_t)snprintf(source + length, sizeof(source) - length, "RET r0 _\nEND _ _\n");
        }
        if (count == 4) expect_value(source, 65536); else expect_error(source, "memory budget exceeded");
    }
    /* Sibling calls reuse storage, but each initialization still consumes steps. */
    for (count = 15; count <= 16; ++count) {
        length = (size_t)snprintf(source, sizeof(source), "GENE main i32\n");
        for (i = 0; i < count; ++i) length += (size_t)snprintf(source + length, sizeof(source) - length, "CALL child _\n");
        snprintf(source + length, sizeof(source) - length, "RET r0 _\nEND _ _\nGENE child i32\nARRAY a 65536\nLEN r0 a\nRET r0 _\nEND _ _\n");
        if (count == 15) expect_value(source, 65536); else expect_error(source, "execution budget exceeded");
    }
    write_source("init-budget.dna", "GENE main i32/1\nMOV r3 r0\nLOAD r4 1\nLABEL loop _\n"
                 "CALL child _\nSUB r3 r4\nJNZ r3 loop\nRET r0 _\nEND _ _\n"
                 "GENE child i32\nARRAY a 65536\nLEN r0 a\nRET r0 _\nEND _ _\n");
    p = compile_name("init-budget.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(p->genes[p->main_gene].memory_cells == 65536);
    arg = 16; expect_run_error(p, &arg, 1, "init-budget.dna:10: execution budget exhausted");
    arg = 15; REQUIRE(bio_program_run(p, &arg, 1, &result, &e) && result == 65536);
    REQUIRE(bio_program_interpret(p, &arg, 1, &result, &e) && result == 65536);
    bio_program_release(p);
    write_source("exact-memory-budget.dna", "GENE main i32/1\nARRAY a 4\nLOAD r1 1\nLABEL loop _\nSUB r0 r1\nJNZ r0 loop\nRET r0 _\nEND _ _\n");
    p = compile_name("exact-memory-budget.dna", 0, &e); REQUIRE(p != NULL);
    arg = 499997;
    REQUIRE(bio_program_run(p, &arg, 1, &result, &e) && result == 0);
    REQUIRE(bio_program_interpret(p, &arg, 1, &result, &e) && result == 0);
    arg = 499998; expect_run_error(p, &arg, 1, "execution budget exhausted");
    bio_program_release(p);
}

typedef struct { BioProgram *program; unsigned id; volatile LONG failures; } MemoryWorker;

static DWORD WINAPI memory_reader(LPVOID arg) {
    MemoryWorker *worker = arg;
    unsigned i, mode;
    for (i = 0; i < 100; ++i) for (mode = 0; mode < 2; ++mode) {
        int32_t args[2] = {i % 3 == 0 ? -1 : 0, (int32_t)(worker->id * 1000u + i)}, result = 99;
        BioError e = {{0}};
        int ok = mode ? bio_program_interpret(worker->program, args, 2, &result, &e) :
                        bio_program_run(worker->program, args, 2, &result, &e);
        if (args[0] < 0 ? (ok || result != 99 || !strstr(e.message, "out of bounds")) :
            (!ok || result != args[1] + 100 || *e.message)) InterlockedIncrement(&worker->failures);
    }
    return 0;
}

static void memory_concurrent_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    MemoryWorker workers[4] = {0};
    HANDLE threads[4];
    unsigned i;
    write_source("memory-threads.dna", "GENE main i32/2\nVAR saved 0\nARRAY a 1\nSET saved r1\n"
                 "AGET r2 a,r0\nADD r2 r1\nASET a r0,r2\nLOAD r3 100\nLOAD r4 1\nLABEL loop _\n"
                 "AGET r2 a,r0\nADD r2 r4\nASET a r0,r2\nSUB r3 r4\nJNZ r3 loop\n"
                 "AGET r2 a,r0\nGET r5 saved\nSUB r2 r5\nADD r2 r1\nRET r2 _\nEND _ _\n");
    p = compile_name("memory-threads.dna", 0, &e); REQUIRE(p != NULL);
    for (i = 0; i < 4; ++i) {
        workers[i].program = p; workers[i].id = i;
        threads[i] = CreateThread(NULL, 0, memory_reader, &workers[i], 0, NULL); REQUIRE(threads[i] != NULL);
    }
    REQUIRE(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 4; ++i) { REQUIRE(workers[i].failures == 0); CloseHandle(threads[i]); }
    bio_program_release(p);
}

typedef int (*RunIO)(const BioProgram *, const int32_t *, size_t, BioIO *, int32_t *, BioError *);

static DWORD WINAPI io_reader(LPVOID arg) {
    MemoryWorker *worker = arg;
    unsigned i;
    for (i = 0; i < 120; ++i) {
        int32_t input[5] = {(int32_t)worker->id, (int32_t)i, INT32_MIN, 4, 5};
        int32_t output[3] = {99, 99, 99}, result = 99;
        BioIO io = {input, i % 3 ? 3 : 5, output, 3, 77, 77, NULL, NULL};
        BioError e = {{0}};
        RunIO run = i % 2 ? bio_program_run_io : bio_program_interpret_io;
        int ok = run(worker->program, NULL, 0, &io, &result, &e);
        if (i % 3 ? (!ok || result != 3 || io.output_count != 3 || io.output_written != 1 ||
                     memcmp(input, output, sizeof(output)) || *e.message) :
            (ok || result != 99 || io.output_count != 77 || io.output_written != 77 ||
             output[0] != 99 || !strstr(e.message, "READ input has 5 numbers")))
            InterlockedIncrement(&worker->failures);
    }
    return 0;
}

static void io_tests(void) {
    BioError e = {{0}};
    unsigned mode, slot;
    char source[1024];
    const int32_t input[] = {INT32_MIN, -7, INT32_MAX, 55, 99};
    int32_t output[6], result;
    BioProgram *p;
    for (slot = 0; slot < 8; ++slot) {
        snprintf(source, sizeof(source), "GENE main i32\nVAR before 99\nARRAY a 4\nVAR after 77\n"
                 "READ a r%u\nWRITE a r%u\nRET r%u _\nEND _ _\n", slot, slot, slot);
        write_source("io.dna", source); p = compile_name("io.dna", 0, &e); REQUIRE(p != NULL);
        REQUIRE(p->stats.dynamic_steps && p->stats.execution_steps == 0);
        for (mode = 0; mode < 2; ++mode) {
            RunIO run = mode ? bio_program_interpret_io : bio_program_run_io;
            BioIO io = {input, 3, output + 1, 4, 99, 99, NULL, NULL};
            unsigned i;
            for (i = 0; i < 6; ++i) output[i] = 123;
            REQUIRE(run(p, NULL, 0, &io, &result, &e) && result == 3);
            REQUIRE(io.output_written == 1 && io.output_count == 3);
            REQUIRE(!memcmp(output + 1, input, 3 * sizeof(*input)));
            REQUIRE(output[0] == 123 && output[4] == 123 && output[5] == 123);
            io.input_count = 0;
            REQUIRE(run(p, NULL, 0, &io, &result, &e) && result == 0);
            REQUIRE(io.output_written == 1 && io.output_count == 0 && output[1] == INT32_MIN);
            io.input_count = 4;
            REQUIRE(run(p, NULL, 0, &io, &result, &e) && result == 4);
            REQUIRE(io.output_count == 4 && output[4] == 55);
            io.input_count = 5; result = 999;
            REQUIRE(!run(p, NULL, 0, &io, &result, &e));
            REQUIRE(strstr(e.message, "READ input has 5 numbers; array capacity is 4") != NULL);
            REQUIRE(result == 999 && io.output_count == 4 && io.output_written == 1 && output[4] == 55);
            io.input_count = 4; io.output_capacity = 3;
            REQUIRE(!run(p, NULL, 0, &io, &result, &e));
            REQUIRE(strstr(e.message, "WRITE count 4 exceeds output capacity 3") != NULL);
            io.input = NULL; io.input_count = 0;
            REQUIRE(!run(p, NULL, 0, &io, &result, &e) && strstr(e.message, "READ requires an input list"));
            io.input = input; io.input_count = 3; io.output = NULL; io.output_capacity = 0;
            REQUIRE(!run(p, NULL, 0, &io, &result, &e) && strstr(e.message, "WRITE requires an output list"));
            io.input_count = BIO_LIST_LIMIT + 1u;
            REQUIRE(!run(p, NULL, 0, &io, &result, &e) && strstr(e.message, "invalid BioIO"));
        }
        bio_program_release(p);
    }
    /* Last WRITE is a snapshot, remains valid after the callee returns, and is transactional. */
    write_source("io.dna", "GENE main i32/1\nARRAY parent 4\nREAD parent r2\nWRITE parent r2\n"
                 "JNZ r0 fail\nCALL child _\nRET r0 _\nLABEL fail _\nLOAD r1 0\nDIV r0 r1\nRET r0 _\nEND _ _\n"
                 "GENE child i32\nVAR pad 42\nARRAY a 4\nREAD a r0\nLOAD r1 0\nLOAD r2 88\nASET a r1,r2\n"
                 "READ a r0\nLOAD r1 1\nWRITE a r1\nASET a r1,r2\nRET r0 _\nEND _ _\n");
    p = compile_name("io.dna", 0, &e); REQUIRE(p != NULL);
    for (mode = 0; mode < 2; ++mode) {
        RunIO run = mode ? bio_program_interpret_io : bio_program_run_io;
        int32_t arg = 0;
        BioIO io = {input, 3, output, 6, 99, 99, NULL, NULL};
        output[0] = 123;
        REQUIRE(run(p, &arg, 1, &io, &result, &e));
        REQUIRE(result == 3 && io.output_count == 1 && output[0] == INT32_MIN);
        arg = 1; output[0] = 123; result = 456;
        REQUIRE(!run(p, &arg, 1, &io, &result, &e) && strstr(e.message, "division by zero"));
        REQUIRE(output[0] == 123 && io.output_count == 1 && io.output_written == 1 && result == 456);
    }
    bio_program_release(p);
    expect_error("GENE main i32\nVAR x 0\nREAD x r0\nRET r0 _\nEND _ _\n", "requires an array");
    expect_error("GENE main i32\nARRAY a 1\nWRITE a r7\nLOAD r0 0\nRET r0 _\nEND _ _\n", "uninitialized register r7");
    expect_error("GENE main i32\nREAD missing r0\nRET r0 _\nEND _ _\n", "undefined local");
    expect_error("GENE main i32\nARRAY a 1\nREAD a _\n", "requires '<array> rN'");
    expect_error("GENE main i32\nARRAY a 1\nWRITE a 1\n", "requires '<array> rN'");
    /* Exactly one million steps: 1 init + 2 setup + 249999*(READ+cell+SUB+JNZ) + RET. */
    write_source("io-budget.dna", "GENE main i32/1\nARRAY a 1\nMOV r3 r0\nLOAD r4 1\nLABEL loop _\n"
                 "READ a r2\nSUB r3 r4\nJNZ r3 loop\nRET r2 _\nEND _ _\n");
    p = compile_name("io-budget.dna", 0, &e); REQUIRE(p != NULL);
    for (mode = 0; mode < 2; ++mode) {
        RunIO run = mode ? bio_program_interpret_io : bio_program_run_io;
        BioIO io = {input, 1, NULL, 0, 9, 9, NULL, NULL};
        int32_t arg = 249999;
        REQUIRE(run(p, &arg, 1, &io, &result, &e) && result == 1);
        REQUIRE(io.output_count == 0 && io.output_written == 0);
        arg = 250000;
        REQUIRE(!run(p, &arg, 1, &io, &result, &e) && strstr(e.message, "execution budget exhausted"));
        REQUIRE(result == 1 && io.output_count == 0 && io.output_written == 0);
    }
    bio_program_release(p);
    write_source("io-runtime.dna", "GENE main i32\nARRAY a 3\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n");
    p = compile_name("io-runtime.dna", 0, &e); REQUIRE(p != NULL);
    {
        BioRuntime *runtime = bio_runtime_create(p, &e);
        MemoryWorker workers[4] = {0};
        HANDLE threads[4];
        unsigned i;
        BioIO io = {input, 3, output, 6, 0, 0, NULL, NULL};
        REQUIRE(runtime != NULL);
        REQUIRE(bio_runtime_run_io(runtime, NULL, 0, &io, &result, &e) && result == 3);
        REQUIRE(io.output_count == 3 && !memcmp(output, input, 3 * sizeof(*input)));
        bio_runtime_destroy(runtime);
        for (i = 0; i < 4; ++i) {
            workers[i].program = p; workers[i].id = i;
            threads[i] = CreateThread(NULL, 0, io_reader, &workers[i], 0, NULL); REQUIRE(threads[i] != NULL);
        }
        REQUIRE(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
        for (i = 0; i < 4; ++i) { REQUIRE(workers[i].failures == 0); CloseHandle(threads[i]); }
    }
    for (mode = 0; mode < 2; ++mode) {
        RunIO run = mode ? bio_program_interpret_io : bio_program_run_io;
        BioIO io = {input, 0, output, 0, 77, 77, NULL, NULL};
        output[0] = 99;
        REQUIRE(run(p, NULL, 0, &io, &result, &e) && result == 0);
        REQUIRE(io.output_count == 0 && io.output_written == 1 && output[0] == 99);
        /* In-place API input/output is safe: publication happens after the computation. */
        output[0] = -17; output[1] = 42;
        io.input = output; io.input_count = 2; io.output_capacity = 3;
        REQUIRE(run(p, NULL, 0, &io, &result, &e) && result == 2);
        REQUIRE(output[0] == -17 && output[1] == 42 && io.output_count == 2);
    }
    bio_program_release(p);
}

static void array_parameter_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    char source[8192], path[256];
    unsigned length_slot, value_slot, i, mode;
    int32_t arg, result;
    /* The length and optional scalar may come from any slots, including aliases. */
    for (length_slot = 0; length_slot < 8; ++length_slot) for (value_slot = 0; value_slot < 8; ++value_slot) {
        snprintf(source, sizeof(source), "GENE main i32\nVAR before 11\nARRAY a 8\nVAR after 13\n"
                 "LOAD r%u 6\nLOAD r%u 4\nCALL edit a,r%u,r%u\n"
                 "GET r1 before\nADD r0 r1\nGET r1 after\nADD r0 r1\nRET r0 _\nEND _ _\n"
                 "GENE edit i32/2\nVAR pad 99\nPARAM items r0\nARRAY scratch 4\n"
                 "MOV r2 r0\nLOAD r3 1\nSUB r2 r3\nASET items r2,r1\n"
                 "LOAD r0 0\nLEN r3 items\nAGET r0 items,r2\nADD r0 r3\nRET r0 _\nEND _ _\n",
                 length_slot, value_slot, length_slot, value_slot);
        expect_value(source, 28 + (length_slot == value_slot ? 4 : 6));
    }
    /* Calls use the caller's local/borrowed array, not the callee's storage slab.
       A nested narrower view must not replace the caller's view on return. */
    expect_value("GENE main i32\nARRAY root 8\nLOAD r1 7\nLOAD r2 777\nASET root r1,r2\n"
                 "LOAD r0 2\nCALL middle root,r0\nLOAD r1 7\nAGET r2 root,r1\nADD r0 r2\nRET r0 _\nEND _ _\n"
                 "GENE middle i32/1\nPARAM p r0\nVAR saved 11\nARRAY temp 3\n"
                 "LOAD r0 3\nLOAD r1 31\nCALL edit temp,r0,r1\nLOAD r0 1\nLOAD r1 41\nCALL edit p,r0,r1\n"
                 "LEN r0 p\nLOAD r1 43\nCALL edit p,r0,r1\n"
                 "LOAD r2 0\nAGET r3 p,r2\nLOAD r2 1\nAGET r4 p,r2\nADD r3 r4\n"
                 "LOAD r2 2\nAGET r4 temp,r2\nADD r3 r4\nGET r4 saved\nADD r3 r4\nRET r3 _\nEND _ _\n"
                 "GENE edit i32/2\nPARAM p r0\nLOAD r2 1\nSUB r0 r2\nASET p r0,r1\nRET r0 _\nEND _ _\n", 903);
    expect_value("GENE main i32\nARRAY a 65536\nLOAD r0 65536\nCALL set_last a,r0\n"
                 "LOAD r1 65535\nAGET r0 a,r1\nRET r0 _\nEND _ _\n"
                 "GENE set_last i32/1\nPARAM a r0\nLOAD r1 65535\nLOAD r2 -2147483648\n"
                 "ASET a r1,r2\nRET r0 _\nEND _ _\n", INT32_MIN);
    expect_value("GENE main i32\nARRAY a 1\nLOAD r0 0\nCALL empty a,r0\nRET r0 _\nEND _ _\n"
                 "GENE empty i32/1\nPARAM a r0\nCALL plain _\nLEN r0 a\nRET r0 _\nEND _ _\n"
                 "GENE plain i32\nARRAY scratch 64\nLOAD r0 999\nRET r0 _\nEND _ _\n", 0);
    /* Bound checks are identical in both engines and report the responsible node. */
    write_source("parameter.dna", "GENE main i32/1\nARRAY a 3\nCALL size a,r0\nRET r0 _\nEND _ _\n"
                 "GENE size i32/1\nPARAM p r0\nLEN r0 p\nRET r0 _\nEND _ _\n");
    p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(p->stats.execution_steps == 7 && p->genes[p->main_gene].memory_cells == 3);
    for (arg = 0; arg <= 3; ++arg) {
        REQUIRE(bio_program_run(p, &arg, 1, &result, &e) && result == arg);
        REQUIRE(bio_program_interpret(p, &arg, 1, &result, &e) && result == arg);
    }
    arg = -1; expect_run_error(p, &arg, 1, "parameter.dna:3: CALL array length -1 out of bounds: expected 0..3");
    arg = INT32_MIN; expect_run_error(p, &arg, 1, "CALL array length -2147483648");
    arg = 4; expect_run_error(p, &arg, 1, "CALL array length 4");
    arg = INT32_MAX; expect_run_error(p, &arg, 1, "CALL array length 2147483647");
    bio_program_release(p);
    for (i = 0; i < 3; ++i) {
        snprintf(source, sizeof(source), "GENE main i32\nARRAY a 8\nLOAD r0 2\nCALL middle a,r0\nRET r0 _\nEND _ _\n"
                 "GENE middle i32/1\nPARAM a r0\nLOAD r0 3\n%s\nRET r0 _\nEND _ _\n"
                 "GENE leaf i32/1\nPARAM a r0\nRET r0 _\nEND _ _\n",
                 i == 0 ? "CALL leaf a,r0" : i == 1 ? "AGET r0 a,r0" : "ASET a r0,r0");
        write_source("parameter.dna", source); p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
        expect_run_error(p, NULL, 0, i == 0 ? "CALL array length 3 out of bounds: expected 0..2" : "array index 3 out of bounds");
        bio_program_release(p);
    }
    write_source("parameter.dna", "GENE main i32\nARRAY a 8\nLOAD r0 0\nCALL empty a,r0\nRET r0 _\nEND _ _\n"
                 "GENE empty i32/1\nPARAM a r0\nAGET r0 a,r0\nRET r0 _\nEND _ _\n");
    p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
    expect_run_error(p, NULL, 0, "array is empty (length 0)"); bio_program_release(p);
    /* A PARAM owns no cells and consumes no initialization/copy steps, even when forwarded. */
    {
        size_t used = (size_t)snprintf(source, sizeof(source), "GENE main i32\nARRAY a 65536\nLOAD r0 65536\nCALL f0 a,r0\nRET r0 _\nEND _ _\n");
        for (i = 0; i < 8; ++i) {
            used += (size_t)snprintf(source + used, sizeof(source) - used, "GENE f%u i32/1\nPARAM p r0\n", i);
            if (i < 7) used += (size_t)snprintf(source + used, sizeof(source) - used, "CALL f%u p,r0\n", i + 1);
            else used += (size_t)snprintf(source + used, sizeof(source) - used, "LEN r0 p\n");
            used += (size_t)snprintf(source + used, sizeof(source) - used, "RET r0 _\nEND _ _\n");
        }
        write_source("parameter.dna", source); p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
        REQUIRE(p->genes[p->main_gene].memory_cells == 65536 && p->stats.execution_steps == 65555);
        REQUIRE(run0(p) == 65536 && interpret0(p) == 65536);
        bio_program_release(p);
    }
    /* 1 cell + 3 setup + 199999*(CALL+LEN+RET+SUB+JNZ) + RET = 1000000. */
    write_source("parameter.dna", "GENE main i32/1\nARRAY a 1\nMOV r3 r0\nLOAD r4 1\nLOAD r2 1\n"
                 "LABEL loop _\nCALL size a,r2\nSUB r3 r4\nJNZ r3 loop\nRET r0 _\nEND _ _\n"
                 "GENE size i32/1\nPARAM p r0\nLEN r0 p\nRET r0 _\nEND _ _\n");
    p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
    arg = 199999;
    REQUIRE(bio_program_run(p, &arg, 1, &result, &e) && result == 1);
    REQUIRE(bio_program_interpret(p, &arg, 1, &result, &e) && result == 1);
    arg = 200000; expect_run_error(p, &arg, 1, "execution budget exhausted"); bio_program_release(p);
    /* Channel operations respect the borrowed bound, and errors do not publish output. */
    write_source("parameter.dna", "GENE main i32/1\nARRAY a 8\nCALL copy a,r0\nRET r0 _\nEND _ _\n"
                 "GENE copy i32/1\nPARAM p r0\nREAD p r1\nWRITE p r0\nRET r1 _\nEND _ _\n");
    p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
    for (mode = 0; mode < 2; ++mode) {
        RunIO run = mode ? bio_program_interpret_io : bio_program_run_io;
        int32_t input[3] = {-5, INT32_MIN, INT32_MAX}, output[3] = {11, 12, 13};
        BioIO io = {input, 3, output, 3, 9, 9, NULL, NULL};
        arg = 2; result = 99;
        REQUIRE(!run(p, &arg, 1, &io, &result, &e) && strstr(e.message, "array capacity is 2"));
        REQUIRE(output[0] == 11 && io.output_count == 9 && io.output_written == 9 && result == 99);
        arg = 3;
        REQUIRE(run(p, &arg, 1, &io, &result, &e) && result == 3);
        REQUIRE(io.output_count == 3 && !memcmp(input, output, sizeof(input)));
        arg = 0; io.input_count = 0;
        REQUIRE(run(p, &arg, 1, &io, &result, &e) && result == 0 && io.output_count == 0 && io.output_written == 1);
    }
    bio_program_release(p);
    /* Independent invocations borrow only their own storage, through two nested calls. */
    write_source("parameter.dna", "GENE main i32\nARRAY a 3\nLOAD r0 3\nCALL bridge a,r0\nRET r0 _\nEND _ _\n"
                 "GENE bridge i32/1\nPARAM p r0\nVAR pad 7\nCALL copy p,r0\nRET r0 _\nEND _ _\n"
                 "GENE copy i32/1\nPARAM p r0\nREAD p r0\nWRITE p r0\nRET r0 _\nEND _ _\n");
    p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
    {
        MemoryWorker workers[4] = {0};
        HANDLE threads[4];
        for (i = 0; i < 4; ++i) {
            workers[i].program = p; workers[i].id = i;
            threads[i] = CreateThread(NULL, 0, io_reader, &workers[i], 0, NULL); REQUIRE(threads[i] != NULL);
        }
        REQUIRE(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
        for (i = 0; i < 4; ++i) { REQUIRE(workers[i].failures == 0); CloseHandle(threads[i]); }
    }
    bio_program_release(p);
    /* PARAM presence is part of the reload signature; the local name is not. */
    write_source("parameter.dna", "GENE main i32\nARRAY a 1\nLOAD r0 1\nCALL value a,r0\nRET r0 _\nEND _ _\n"
                 "GENE value i32/1\nPARAM p r0\nRET r0 _\nEND _ _\n");
    p = compile_name("parameter.dna", 0, &e); REQUIRE(p != NULL);
    {
        BioRuntime *runtime = bio_runtime_create(p, &e); REQUIRE(runtime != NULL);
        path_for(path, sizeof(path), "reload-array.dna");
        write_source("reload-array.dna", "GENE main i32\nLOAD r0 1\nCALL value r0\nRET r0 _\nEND _ _\n"
                     "GENE value i32/1\nRET r0 _\nEND _ _\n");
        REQUIRE(!bio_runtime_reload(runtime, path, 0, &e) && strstr(e.message, "incompatible signature"));
        REQUIRE(runtime0(runtime) == 1);
        write_source("reload-array.dna", "GENE main i32\nARRAY a 1\nLOAD r0 1\nCALL value a,r0\nRET r0 _\nEND _ _\n"
                     "GENE value i32/1\nPARAM renamed r0\nLOAD r0 42\nRET r0 _\nEND _ _\n");
        REQUIRE(bio_runtime_reload(runtime, path, 0, &e) && runtime0(runtime) == 42);
        REQUIRE(run0(p) == 1);
        bio_runtime_destroy(runtime);
    }
    bio_program_release(p);
}

static void module_and_repair_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    BioStats s;
    write_source("math.dna", "GENE add i32/2\nADD r0 r1\nRET r0 _\nEND _ _\nGENE unused i32\nLOAD r0 99\nRET r0 _\nEND _ _\n");
    write_source("left.dna", "IMPORT math.dna _\n");
    write_source("right.dna", "IMPORT ./math.dna _\n");
    write_source("module.dna", "IMPORT left.dna _\nIMPORT right.dna _\nGENE main i32\nLOAD r1 10\nLOAD r2 20\nCALL add r1,r2\nRET r0 _\nEND _ _\n");
    p = compile_name("module.dna", 0, &e);
    REQUIRE(p != NULL);
    REQUIRE(run0(p) == 30);
    REQUIRE(interpret0(p) == 30);
    s = bio_program_stats(p);
    REQUIRE(s.files == 4 && s.genes == 3 && s.live_genes == 2);
    REQUIRE(p->genes[bio_find_gene(p, "unused")].code_end == 0);
    REQUIRE(p->genes[p->main_gene].head->next->next->next->bond == p->genes[0].head);
    bio_program_release(p);
    write_source("cycle_a.dna", "IMPORT cycle_b.dna _\n");
    write_source("cycle_b.dna", "IMPORT cycle_a.dna _\n");
    p = compile_name("cycle_a.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "cyclic import"));
    write_source("repair.dna", "GENE main i32\nLOAD r0 42\nRET r0\nEND _\n");
    p = compile_name("repair.dna", 1, &e);
    REQUIRE(p != NULL);
    REQUIRE(bio_program_stats(p).repairs == 2);
    REQUIRE(run0(p) == 42);
    bio_program_release(p);
    write_source("repair_bad.dna", "GENE main i32\nLOAD r0\nRET r0\nEND _\n");
    p = compile_name("repair_bad.dna", 1, &e);
    REQUIRE(p != NULL);
    REQUIRE(bio_program_stats(p).repairs == 3 && bio_program_stats(p).quarantines == 1);
    REQUIRE(bio_program_repair_count(p) == 3);
    {
        BioRepair repair = {0};
        int32_t result = 123;
        REQUIRE(bio_program_repair_at(p, 0, &repair));
        REQUIRE(!strcmp(repair.kind, "quarantine") && strstr(repair.detail, "three fields"));
        REQUIRE(!bio_program_run(p, NULL, 0, &result, &e));
        REQUIRE(result == 123 && strstr(e.message, "quarantined repaired codon"));
    }
    bio_program_release(p);
    write_source("repair_link.dna", "GENE main i32\nCALL absent _\nRET r0 _\nEND _ _\n");
    p = compile_name("repair_link.dna", 1, &e);
    REQUIRE(p != NULL && bio_program_stats(p).quarantines == 1);
    {
        int32_t result = 77;
        REQUIRE(!bio_program_run(p, NULL, 0, &result, &e));
        REQUIRE(result == 77 && strstr(e.message, "quarantined repaired codon"));
        REQUIRE(!bio_program_interpret(p, NULL, 0, &result, &e));
        REQUIRE(result == 77 && strstr(e.message, "quarantined repaired codon"));
    }
    bio_program_release(p);
    write_source("repair_flow.dna", "GENE main i32\nMOV r0 r1\nRET r0 _\nEND _ _\n");
    p = compile_name("repair_flow.dna", 1, &e);
    REQUIRE(p != NULL && bio_program_stats(p).quarantines == 1);
    {
        int32_t result = 77;
        REQUIRE(!bio_program_run(p, NULL, 0, &result, &e));
        REQUIRE(result == 77 && strstr(e.message, "quarantined repaired codon"));
    }
    bio_program_release(p);
    write_source("repair_end.dna", "GENE main i32\nLOAD r0 9\nRET r0 _\n");
    p = compile_name("repair_end.dna", 1, &e);
    REQUIRE(p != NULL && bio_program_stats(p).repairs == 1 && run0(p) == 9);
    bio_program_release(p);
}

static uint32_t random_state = 0x51921u;
static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void differential_tests(void) {
    unsigned iteration, i;
    for (iteration = 0; iteration < 250; ++iteration) {
        char source[4096];
        size_t length = 0;
        uint32_t r[8];
        int32_t expected;
        length += (size_t)snprintf(source + length, sizeof(source) - length, "GENE main i32\n");
        for (i = 0; i < 8; ++i) {
            int32_t value;
            r[i] = next_random();
            memcpy(&value, &r[i], sizeof(value));
            length += (size_t)snprintf(source + length, sizeof(source) - length, "LOAD r%u %" PRId32 "\n", i, value);
        }
        for (i = 0; i < 100; ++i) {
            static const char *ops[] = {"ADD", "SUB", "MUL", "MOV", "EQ", "LT", "MIN", "MAX"};
            unsigned op = next_random() % 8, a = next_random() % 8, b = next_random() % 8;
            int32_t left, right;
            memcpy(&left, &r[a], sizeof(left));
            memcpy(&right, &r[b], sizeof(right));
            if (op == 0) r[a] += r[b];
            else if (op == 1) r[a] -= r[b];
            else if (op == 2) r[a] *= r[b];
            else if (op == 3) r[a] = r[b];
            else if (op == 4) r[a] = left == right;
            else if (op == 5) r[a] = left < right;
            else if (op == 6 && left > right) r[a] = r[b];
            else if (op == 7 && left < right) r[a] = r[b];
            length += (size_t)snprintf(source + length, sizeof(source) - length, "%s r%u r%u\n", ops[op], a, b);
        }
        i = next_random() % 8;
        snprintf(source + length, sizeof(source) - length, "RET r%u _\nEND _ _\n", i);
        memcpy(&expected, &r[i], sizeof(expected));
        expect_value(source, expected);
    }
}

static void argument_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    BioRuntime *runtime;
    int32_t args[] = {-7, 6, 9}, result = 123;
    char path[256];
    FILE *dump;
    write_source("args.dna", "GENE main i32/2\nCALL product r0,r1\nRET r0 _\nEND _ _\nGENE product i32/2\nMUL r0 r1\nRET r0 _\nEND _ _\n");
    p = compile_name("args.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_stats(p).arity == 2 && bio_program_stats(p).execution_steps == 4);
    REQUIRE(bio_program_run(p, args, 2, &result, &e) && result == -42);
    REQUIRE(bio_program_interpret(p, args, 2, &result, &e) && result == -42);
    result = 123;
    REQUIRE(!bio_program_run(p, args, 1, &result, &e) && result == 123);
    REQUIRE(strstr(e.message, "expects 2 arguments") != NULL);
    REQUIRE(!bio_program_run(p, args, 3, &result, &e) && result == 123);
    REQUIRE(!bio_program_run(p, NULL, 2, &result, &e) && result == 123);
    REQUIRE(!bio_program_interpret(p, args, 0, &result, &e) && result == 123);
    REQUIRE(!bio_program_run(p, args, 2, NULL, &e));
    runtime = bio_runtime_create(p, &e); REQUIRE(runtime != NULL);
    REQUIRE(bio_runtime_run(runtime, args, 2, &result, &e) && result == -42);
    REQUIRE(!bio_runtime_run(runtime, args, 0, &result, &e) && result == -42);
    bio_runtime_destroy(runtime);
    path_for(path, sizeof(path), "normalized.dna");
    dump = fopen(path, "wb"); REQUIRE(dump != NULL);
    bio_program_dump(p, dump); REQUIRE(fclose(dump) == 0);
    bio_program_release(p);
    p = bio_load(path, 0, &e); REQUIRE(p != NULL);
    REQUIRE(p->code == NULL && p->unwind_table == NULL && p->stats.code_bytes == 0);
    REQUIRE(bio_program_interpret(p, args, 2, &result, &e) && result == -42);
    REQUIRE(!bio_program_run(p, args, 2, &result, &e) && result == -42);
    REQUIRE(bio_runtime_create(p, &e) == NULL);
    bio_program_release(p);
    write_source("one-arg.dna", "GENE main i32/1\nLOAD r1 -2\nMUL r0 r1\nRET r0 _\nEND _ _\n");
    p = compile_name("one-arg.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_run(p, args, 1, &result, &e) && result == 14);
    bio_program_release(p);
}

static void wide_call_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    int32_t args[] = {3, -5, 7, 100}, result;
    char path[256];
    FILE *dump;
    /* Four arguments through a two-level chain; the middle gene forwards all. */
    write_source("four.dna",
        "GENE main i32/4\nCALL mix r0,r1,r2,r3\nRET r0 _\nEND _ _\n"
        "GENE mix i32/4\nCALL add4 r0,r1,r2,r3\nMUL r0 r1\nRET r0 _\nEND _ _\n"
        "GENE add4 i32/4\nADD r0 r1\nADD r0 r2\nADD r0 r3\nRET r0 _\nEND _ _\n");
    p = compile_name("four.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_run(p, args, 4, &result, &e) && result == (3 - 5 + 7 + 100) * -5);
    REQUIRE(bio_program_interpret(p, args, 4, &result, &e) && result == (3 - 5 + 7 + 100) * -5);
    result = 0;
    REQUIRE(!bio_program_run(p, args, 3, &result, &e));
    REQUIRE(strstr(e.message, "expects 4 arguments") != NULL);
    /* The dump keeps all four argument registers and reparses identically. */
    path_for(path, sizeof(path), "four-normalized.dna");
    dump = fopen(path, "wb"); REQUIRE(dump != NULL);
    bio_program_dump(p, dump); REQUIRE(fclose(dump) == 0);
    bio_program_release(p);
    p = bio_load(path, 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_interpret(p, args, 4, &result, &e) && result == (3 - 5 + 7 + 100) * -5);
    bio_program_release(p);
    /* Array plus length and three scalars: i32/4 with PARAM. data[3] = -5, then
       pick receives length 4, index 3 and the two scalars: xs[3] + r2 + r3. */
    write_source("wide-array.dna",
        "GENE main i32/2\nARRAY data 4\nASET data r0,r1\nLOAD r2 4\nCALL pick data,r2,r0,r1,r0\nRET r0 _\nEND _ _\n"
        "GENE pick i32/4\nPARAM xs r0\nLOAD r4 0\nAGET r4 xs,r1\nADD r4 r2\nADD r4 r3\nRET r4 _\nEND _ _\n");
    p = compile_name("wide-array.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_run(p, args, 2, &result, &e) && result == -7);
    REQUIRE(bio_program_interpret(p, args, 2, &result, &e) && result == -7);
    bio_program_release(p);
}

/* Both backends must fail with the identical message. */
static void expect_run_failure(const char *source, const char *message) {
    BioError e = {{0}}, ie = {{0}};
    BioProgram *p;
    int32_t result = 77;
    write_source("sfail.dna", source);
    p = compile_name("sfail.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(!bio_program_run(p, NULL, 0, &result, &e));
    REQUIRE(strstr(e.message, message) != NULL);
    REQUIRE(!bio_program_interpret(p, NULL, 0, &result, &ie));
    REQUIRE(strstr(ie.message, message) != NULL);
    REQUIRE(!strcmp(e.message, ie.message));
    bio_program_release(p);
}

/* Captures PRINT bytes; fail=1 makes the callback report an I/O failure. */
typedef struct {
    unsigned char data[1024];
    size_t length;
    int fail;
} PrintCapture;

static int capture_print(void *user, const unsigned char *bytes, size_t length) {
    PrintCapture *capture = user;
    if (capture->fail) return 0;
    if (length > sizeof(capture->data) - capture->length) return 0;
    memcpy(capture->data + capture->length, bytes, length);
    capture->length += length;
    return 1;
}

/* Counts bytes without storing them: the budget parity probe prints a lot. */
static int count_print(void *user, const unsigned char *bytes, size_t length) {
    (void)bytes;
    *(size_t *)user += length;
    return 1;
}

/* Both backends must deliver identical bytes and the same result. */
static void expect_print(const char *source, int32_t expected, const char *bytes) {
    BioError e = {{0}}, ie = {{0}};
    BioProgram *p;
    int32_t result = 0;
    PrintCapture a = {{0}, 0, 0}, b = {{0}, 0, 0};
    BioIO io_a = {NULL, 0, NULL, 0, 0, 0, capture_print, &a};
    BioIO io_b = {NULL, 0, NULL, 0, 0, 0, capture_print, &b};
    write_source("print.dna", source);
    p = compile_name("print.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_run_io(p, NULL, 0, &io_a, &result, &e)); REQUIRE(result == expected);
    REQUIRE(bio_program_interpret_io(p, NULL, 0, &io_b, &result, &ie)); REQUIRE(result == expected);
    REQUIRE(a.length == strlen(bytes) && !memcmp(a.data, bytes, a.length));
    REQUIRE(b.length == a.length && !memcmp(a.data, b.data, a.length));
    bio_program_release(p);
}

static void expect_print_failure(const char *source, const char *message, int grant) {
    BioError e = {{0}}, ie = {{0}};
    BioProgram *p;
    int32_t result = 0;
    PrintCapture a = {{0}, 0, 0}, b = {{0}, 0, 0};
    BioIO io_a = {NULL, 0, NULL, 0, 0, 0, grant ? capture_print : NULL, &a};
    BioIO io_b = {NULL, 0, NULL, 0, 0, 0, grant ? capture_print : NULL, &b};
    write_source("printfail.dna", source);
    p = compile_name("printfail.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(!bio_program_run_io(p, NULL, 0, &io_a, &result, &e));
    REQUIRE(strstr(e.message, message) != NULL);
    REQUIRE(!bio_program_interpret_io(p, NULL, 0, &io_b, &result, &ie));
    REQUIRE(!strcmp(e.message, ie.message));
    bio_program_release(p);
}

static void print_tests(void) {
    BioError e = {{0}}, ie = {{0}};
    BioProgram *p;
    int32_t result = 0;
    char path[256];
    FILE *dump;
    size_t total_a = 0, total_b = 0;
    PrintCapture a = {{0}, 0, 0}, b = {{0}, 0, 0};
    BioIO io_a = {NULL, 0, NULL, 0, 0, 0, capture_print, &a};
    BioIO io_b = {NULL, 0, NULL, 0, 0, 0, capture_print, &b};
    /* Basics: concatenated prints, empty string, repetition, printing in a call. */
    expect_print("GENE main i32\nSLOAD r1 \"hello\"\nPRINT r1 _\nSLOAD r2 \" world\"\nPRINT r2 _\n"
                 "SLEN r3 r1\nRET r3 _\nEND _ _\n", 5, "hello world");
    expect_print("GENE main i32\nSLOAD r1 \"\"\nPRINT r1 _\nLOAD r0 7\nRET r0 _\nEND _ _\n", 7, "");
    expect_print("GENE main i32\nSLOAD r1 \"ab\"\nLOAD r2 3\nLOAD r3 1\nLABEL loop _\n"
                 "PRINT r1 _\nSUB r2 r3\nJNZ r2 loop\nLOAD r0 0\nRET r0 _\nEND _ _\n", 0, "ababab");
    expect_print("GENE main i32\nCALL say _\nRET r0 _\nEND _ _\n"
                 "GENE say i32\nSLOAD r1 \"hi\"\nPRINT r1 _\nLOAD r0 7\nRET r0 _\nEND _ _\n", 7, "hi");
    /* A printed result of SCAT, with escapes and non-ASCII bytes. */
    expect_print("GENE main i32\nSLOAD r1 \"a\\n\"\nSLOAD r2 \"\\x80\\xff\"\nSCAT r3 r1,r2\n"
                 "PRINT r3 _\nSLEN r4 r3\nRET r4 _\nEND _ _\n", 4, "a\n\x80\xff");
    /* Faults: identical messages in both backends. */
    expect_print_failure("GENE main i32\nLOAD r1 7\nPRINT r1 _\nRET r1 _\nEND _ _\n",
                         "invalid string handle 7", 1);
    expect_print_failure("GENE main i32\nSLOAD r1 \"x\"\nPRINT r1 _\nRET r1 _\nEND _ _\n",
                         "PRINT requires a host print capability", 0);
    /* A failing callback stops the run; nothing is retried. */
    a.fail = 1; b.fail = 1;
    write_source("printfail.dna", "GENE main i32\nSLOAD r1 \"x\"\nPRINT r1 _\nRET r1 _\nEND _ _\n");
    p = compile_name("printfail.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(!bio_program_run_io(p, NULL, 0, &io_a, &result, &e));
    REQUIRE(strstr(e.message, "the host print callback failed") != NULL);
    REQUIRE(!bio_program_interpret_io(p, NULL, 0, &io_b, &result, &ie));
    REQUIRE(!strcmp(e.message, ie.message));
    REQUIRE(a.length == 0 && b.length == 0);
    bio_program_release(p);
    /* Byte charging: an unbounded print loop dies on the step budget, after
       delivering exactly the same byte count through both backends. */
    write_source("ploop.dna", "GENE main i32\nSLOAD r1 \"ab\"\nLABEL loop _\nPRINT r1 _\nJMP loop _\nEND _ _\n");
    p = compile_name("ploop.dna", 0, &e); REQUIRE(p != NULL);
    io_a.print = count_print; io_a.print_user = &total_a;
    io_b.print = count_print; io_b.print_user = &total_b;
    REQUIRE(!bio_program_run_io(p, NULL, 0, &io_a, &result, &e));
    REQUIRE(strstr(e.message, "execution budget exhausted") != NULL);
    REQUIRE(!bio_program_interpret_io(p, NULL, 0, &io_b, &result, &ie));
    REQUIRE(!strcmp(e.message, ie.message));
    REQUIRE(total_a > 0 && total_a == total_b);
    bio_program_release(p);
    /* Parse rules and repair of the unused third field. */
    expect_error("GENE main i32\nPRINT r1 x\nRET r1 _\nEND _ _\n", "PRINT requires an unused '_' third field");
    expect_error("GENE main i32\nPRINT r8 _\nRET r1 _\nEND _ _\n", "first operand must be r0..r7");
    write_source("prepair.dna", "GENE main i32\nSLOAD r1 \"ok\"\nPRINT r1\nLOAD r2 1\nRET r2 _\nEND _ _\n");
    p = compile_name("prepair.dna", 1, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_repair_count(p) == 1);
    a.fail = 0; a.length = 0;
    io_a.print = capture_print; io_a.print_user = &a;
    REQUIRE(bio_program_run_io(p, NULL, 0, &io_a, &result, &e) && result == 1);
    REQUIRE(a.length == 2 && !memcmp(a.data, "ok", 2));
    bio_program_release(p);
    /* Dump prints PRINT rN _ and the round trip keeps printing. */
    write_source("pdump.dna", "GENE main i32\nSLOAD r1 \"z\"\nPRINT r1 _\nSLEN r2 r1\nRET r2 _\nEND _ _\n");
    p = compile_name("pdump.dna", 0, &e); REQUIRE(p != NULL);
    path_for(path, sizeof(path), "pdump-normalized.dna");
    dump = fopen(path, "wb"); REQUIRE(dump != NULL);
    bio_program_dump(p, dump); REQUIRE(fclose(dump) == 0);
    bio_program_release(p);
    p = compile_name("pdump-normalized.dna", 0, &e); REQUIRE(p != NULL);
    a.length = 0;
    REQUIRE(bio_program_run_io(p, NULL, 0, &io_a, &result, &e) && result == 1);
    REQUIRE(a.length == 1 && a.data[0] == 'z');
    bio_program_release(p);
}

static void string_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    int32_t result;
    char path[256];
    FILE *dump;
    /* Literals, escapes, length, equality. */
    expect_value("GENE main i32\nSLOAD r1 \"hello\"\nSLEN r2 r1\nRET r2 _\nEND _ _\n", 5);
    expect_value("GENE main i32\nSLOAD r1 \"a\\n\\t\\\\\\\"\\x41b\"\nSLEN r2 r1\nRET r2 _\nEND _ _\n", 7);
    expect_value("GENE main i32\nSLOAD r1 \"\"\nSLEN r2 r1\nRET r2 _\nEND _ _\n", 0);
    expect_value("GENE main i32\nSLOAD r1 \"x\"\nSLOAD r2 \"x\"\nSEQ r3 r1,r2\nRET r3 _\nEND _ _\n", 1);
    expect_value("GENE main i32\nSLOAD r1 \"ab\"\nSLOAD r2 \"abc\"\nSEQ r3 r1,r2\nRET r3 _\nEND _ _\n", 0);
    expect_value("GENE main i32\nSLOAD r1 \"abc\"\nLOAD r2 1\nSGET r3 r1,r2\nRET r3 _\nEND _ _\n", 'b');
    /* Quotes protect spaces and '#'; comment stripping respects them. */
    expect_value("GENE main i32\nSLOAD r1 \"a b#c\" # \"quoted comment\"\nSLEN r2 r1\nRET r2 _\nEND _ _\n", 5);
    /* Concatenation, including a string concatenated with itself. */
    expect_value("GENE main i32\nSLOAD r1 \"hello\"\nSLOAD r2 \" world\"\nSCAT r3 r1,r2\n"
                 "SLOAD r4 \"hello world\"\nSEQ r5 r3,r4\nSLEN r6 r3\nADD r6 r5\nRET r6 _\nEND _ _\n", 12);
    expect_value("GENE main i32\nSLOAD r1 \"ab\"\nSCAT r2 r1,r1\nSLOAD r3 \"abab\"\nSEQ r4 r2,r3\nRET r4 _\nEND _ _\n", 1);
    expect_value("GENE main i32\nSLOAD r1 \"x\"\nSLOAD r2 \"\"\nSCAT r3 r1,r2\nSLEN r4 r3\nRET r4 _\nEND _ _\n", 1);
    /* Doubling loop: 10 SCATs of "a" give 1024 bytes; both backends agree. */
    expect_value("GENE main i32\nSLOAD r1 \"a\"\nLOAD r3 1\nLOAD r4 10\nLABEL grow _\n"
                 "SCAT r1 r1,r1\nSUB r4 r3\nJNZ r4 grow\nSLEN r2 r1\nRET r2 _\nEND _ _\n", 1024);
    /* Faults: identical in both backends. */
    expect_run_failure("GENE main i32\nLOAD r1 99\nSLEN r2 r1\nRET r2 _\nEND _ _\n", "invalid string handle 99");
    expect_run_failure("GENE main i32\nSLOAD r1 \"ab\"\nLOAD r2 -1\nSLEN r3 r2\nRET r3 _\nEND _ _\n",
                       "invalid string handle -1");
    expect_run_failure("GENE main i32\nSLOAD r1 \"ab\"\nLOAD r2 2\nSGET r3 r1,r2\nRET r3 _\nEND _ _\n",
                       "string index 2 out of bounds: valid indices are 0..1 (length 2)");
    expect_run_failure("GENE main i32\nSLOAD r1 \"\"\nLOAD r2 0\nSGET r3 r1,r2\nRET r3 _\nEND _ _\n",
                       "the string is empty");
    /* The handle table fills after 4096 entries: empty concatenations cost no
       bytes and no extra steps, so the loop reaches the table limit. */
    expect_run_failure("GENE main i32\nSLOAD r1 \"\"\nLOAD r3 1\nLOAD r4 5000\nLABEL loop _\n"
                       "SCAT r1 r1,r1\nSUB r4 r3\nJNZ r4 loop\nRET r1 _\nEND _ _\n",
                       "string handle table full (4096 entries");
    /* A 128 KiB string built by doubling, then appended: the 512 KiB pool runs
       out before the step budget does. */
    expect_run_failure("GENE main i32\nSLOAD r1 \"a\"\nLOAD r3 1\nLOAD r4 17\nLABEL grow _\n"
                       "SCAT r1 r1,r1\nSUB r4 r3\nJNZ r4 grow\nMOV r2 r1\nLOAD r4 8\nLABEL fill _\n"
                       "SCAT r2 r2,r1\nSUB r4 r3\nJNZ r4 fill\nSLEN r5 r2\nRET r5 _\nEND _ _\n",
                       "string memory exhausted");
    /* Parse and analysis errors. */
    expect_error("GENE main i32\nSLOAD r1 hello\nRET r1 _\nEND _ _\n", "requires a quoted literal");
    expect_error("GENE main i32\nSLOAD r1 \"a\\qb\"\nRET r1 _\nEND _ _\n", "unknown escape");
    expect_error("GENE main i32\nSLOAD r1 \"unterminated\nRET r1 _\nEND _ _\n", "requires a quoted literal");
    expect_error("GENE main i32\nSLEN r1 r2,r3\nRET r1 _\nEND _ _\n", "SLEN requires");
    expect_error("GENE main i32\nSCAT r1 r2\nRET r1 _\nEND _ _\n", "requires 'rN rM,rK'");
    expect_error("GENE main i32\nSLOAD r1 \"x\"\nSLEN r2 r7\nRET r2 _\nEND _ _\n", "uninitialized register r7");
    /* The dump escapes literals; a reparse reproduces them byte for byte. */
    write_source("str.dna",
        "GENE main i32\nSLOAD r1 \"a\\n\\t\\\\\\\"\\x01# b\"\nSLOAD r2 \"plain\"\n"
        "SCAT r3 r1,r2\nSLEN r4 r3\nLOAD r5 13\nSGET r6 r3,r5\nADD r4 r6\nRET r4 _\nEND _ _\n");
    p = compile_name("str.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_run(p, NULL, 0, &result, &e) && result == 14 + 'n');
    path_for(path, sizeof(path), "str-normalized.dna");
    dump = fopen(path, "wb"); REQUIRE(dump != NULL);
    bio_program_dump(p, dump); REQUIRE(fclose(dump) == 0);
    bio_program_release(p);
    p = bio_load(path, 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_interpret(p, NULL, 0, &result, &e) && result == 14 + 'n');
    bio_program_release(p);
    /* The dumped file also compiles to native code with identical behavior. */
    p = compile_name("str-normalized.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(bio_program_run(p, NULL, 0, &result, &e) && result == 14 + 'n');
    bio_program_release(p);
}

static void flow_tests(void) {
    expect_value("GENE main i32\nLOAD r0 0\nJZ r0 yes\nLOAD r1 8\nJMP done _\nLABEL yes _\nLOAD r1 42\nLABEL done _\nRET r1 _\nEND _ _\n", 42);
    expect_value("GENE main i32\nLOAD r0 -1\nJNZ r0 yes\nLOAD r1 8\nRET r1 _\nLABEL yes _\nLOAD r1 42\nRET r1 _\nEND _ _\n", 42);
    expect_value("GENE main i32\nLOAD r0 3\nLOAD r1 1\nLABEL loop _\nSUB r0 r1\nJNZ r0 loop\nRET r0 _\nEND _ _\n", 0);
    expect_value("GENE main i32\nJMP done _\nADD r5 r6\nLABEL done _\nLOAD r0 7\nRET r0 _\nEND _ _\n", 7);
    expect_value("GENE main i32\nLABEL same _\nCALL f _\nRET r0 _\nEND _ _\nGENE f i32\nJMP same _\nLABEL same _\nLOAD r0 9\nRET r0 _\nEND _ _\n", 9);
    expect_error("GENE main i32\nJMP missing _\nEND _ _\n", "undefined label");
    expect_error("GENE main i32\nLABEL x _\nLABEL x _\nJMP x _\nEND _ _\n", "duplicate label");
    expect_error("GENE main i32\nJMP other _\nEND _ _\nGENE f i32\nLABEL other _\nLOAD r0 1\nRET r0 _\nEND _ _\n", "undefined label");
    expect_error("GENE main i32\nJZ r0 x\nLABEL x _\nJMP x _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32/1\nJZ r0 skip\nLOAD r1 1\nLABEL skip _\nRET r1 _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32/1\nJZ r0 late\nLOAD r1 7\nJMP join _\nLABEL late _\nJMP join _\nLABEL join _\nRET r1 _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32/1\nLOAD r2 1\nLABEL loop _\nJZ r0 done\nLOAD r1 7\nSUB r0 r2\nJMP loop _\nLABEL done _\nRET r1 _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32/1\nJNZ r0 done\nRET r0 _\nLABEL done _\nEND _ _\n", "falls through END");
    expect_error("GENE main i32\nLABEL loop _\nEND _ _\n", "falls through END");
    expect_error("GENE main i32\nEND _ _\n", "empty gene");
    expect_error("GENE main i32\nLABEL x 1\n", "'<name> _'");
    expect_error("GENE main i32\nJMP x r0\n", "'<name> _'");
    expect_error("GENE main i32\nJZ r8 x\n", "'rN <label>'");
}

static void expect_budget_failure(BioProgram *p, const int32_t *args, size_t argc) {
    BioError native_error = {{0}}, interpreted_error = {{0}};
    int32_t result = 2468;
    REQUIRE(!bio_program_run(p, args, argc, &result, &native_error));
    REQUIRE(result == 2468);
    REQUIRE(!bio_program_interpret(p, args, argc, &result, &interpreted_error));
    REQUIRE(result == 2468);
    REQUIRE(strstr(native_error.message, "execution budget exhausted") != NULL);
    REQUIRE(!strcmp(native_error.message, interpreted_error.message));
}

static void loop_budget_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    int32_t input, result;
    write_source("loop.dna", "GENE main i32\nLABEL spin _\nLABEL alias _\nJMP spin _\nEND _ _\n");
    p = compile_name("loop.dna", 0, &e); REQUIRE(p != NULL);
    REQUIRE(p->stats.dynamic_steps && p->stats.execution_steps == 0);
    expect_budget_failure(p, NULL, 0);
    bio_program_release(p);
    /* 1 initial LOAD + 2*n loop steps + RET: exactly one million is permitted. */
    write_source("loop.dna", "GENE main i32/1\nLOAD r1 1\nLABEL loop _\nSUB r0 r1\nJNZ r0 loop\nRET r0 _\nEND _ _\n");
    p = compile_name("loop.dna", 0, &e); REQUIRE(p != NULL);
    input = 499999;
    REQUIRE(bio_program_run(p, &input, 1, &result, &e) && result == 0);
    REQUIRE(bio_program_interpret(p, &input, 1, &result, &e) && result == 0);
    input = 500000; expect_budget_failure(p, &input, 1);
    input = 3;
    REQUIRE(bio_program_run(p, &input, 1, &result, &e) && result == 0);
    REQUIRE(bio_program_interpret(p, &input, 1, &result, &e) && result == 0);
    bio_program_release(p);
    /* One extra LOAD pushes the otherwise identical run to 1,000,001 steps. */
    write_source("loop.dna", "GENE main i32/1\nLOAD r1 1\nLOAD r2 0\nLABEL loop _\nSUB r0 r1\nJNZ r0 loop\nRET r0 _\nEND _ _\n");
    p = compile_name("loop.dna", 0, &e); REQUIRE(p != NULL);
    input = 499999; expect_budget_failure(p, &input, 1);
    bio_program_release(p);
    /* Each callee fits individually, but the whole run exceeds the shared budget. */
    write_source("loop.dna", "GENE main i32/1\nMOV r4 r0\nCALL work r4\nCALL work r4\nRET r0 _\nEND _ _\nGENE work i32/1\nLOAD r1 1\nLABEL again _\nSUB r0 r1\nJNZ r0 again\nRET r0 _\nEND _ _\n");
    p = compile_name("loop.dna", 0, &e); REQUIRE(p != NULL);
    input = 300000; expect_budget_failure(p, &input, 1);
    input = 4; REQUIRE(bio_program_run(p, &input, 1, &result, &e) && result == 0);
    bio_program_release(p);
    /* Fault propagation must pass through multiple callers without overwriting output. */
    write_source("loop.dna", "GENE main i32\nCALL middle _\nRET r0 _\nEND _ _\nGENE middle i32\nCALL spin _\nRET r0 _\nEND _ _\nGENE spin i32\nLABEL spin _\nJMP spin _\nEND _ _\n");
    p = compile_name("loop.dna", 0, &e); REQUIRE(p != NULL);
    expect_budget_failure(p, NULL, 0);
    bio_program_release(p);
}

typedef struct { BioRuntime *runtime; volatile LONG failures; } LoopWorkers;

static DWORD WINAPI loop_reader(LPVOID arg) {
    LoopWorkers *workers = arg;
    unsigned i;
    for (i = 0; i < 10; ++i) {
        int32_t input = (int32_t)(i % 2), result = 123;
        BioError e = {{0}};
        int ok = bio_runtime_run(workers->runtime, &input, 1, &result, &e);
        if ((!input && (!ok || result != 7)) ||
            (input && (ok || result != 123 || !strstr(e.message, "execution budget exhausted"))))
            InterlockedIncrement(&workers->failures);
    }
    return 0;
}

static void loop_runtime_tests(void) {
    BioError e = {{0}};
    BioProgram *p;
    LoopWorkers workers = {0};
    HANDLE threads[4];
    unsigned i;
    char path[256];
    int32_t input = 1, result;
    write_source("runtime-loop.dna", "GENE main i32/1\nJZ r0 done\nLABEL spin _\nJMP spin _\nLABEL done _\nLOAD r0 7\nRET r0 _\nEND _ _\n");
    p = compile_name("runtime-loop.dna", 0, &e); REQUIRE(p != NULL);
    workers.runtime = bio_runtime_create(p, &e); REQUIRE(workers.runtime != NULL);
    bio_program_release(p);
    for (i = 0; i < 4; ++i) {
        threads[i] = CreateThread(NULL, 0, loop_reader, &workers, 0, NULL);
        REQUIRE(threads[i] != NULL);
    }
    REQUIRE(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    REQUIRE(workers.failures == 0);
    for (i = 0; i < 4; ++i) CloseHandle(threads[i]);
    write_source("fixed-loop.dna", "GENE main i32/1\nLOAD r0 42\nRET r0 _\nEND _ _\n");
    path_for(path, sizeof(path), "fixed-loop.dna");
    REQUIRE(bio_runtime_reload(workers.runtime, path, 0, &e));
    REQUIRE(bio_runtime_run(workers.runtime, &input, 1, &result, &e) && result == 42);
    bio_runtime_destroy(workers.runtime);
}

static void execution_budget_tests(void) {
    unsigned excess, i;
    char path[256];
    BioError e = {{0}};
    BioProgram *p;
    FILE *f;
    path_for(path, sizeof(path), "steps.dna");
    for (excess = 0; excess < 2; ++excess) {
        f = fopen(path, "wb"); REQUIRE(f != NULL);
        fputs("GENE main i32\n", f);
        for (i = 0; i < 999; ++i) fputs("CALL unit _\n", f);
        if (excess) fputs("LOAD r0 1\n", f);
        fputs("RET r0 _\nEND _ _\nGENE unit i32\n", f);
        for (i = 0; i < 333; ++i) fputs("CALL one _\n", f);
        fputs("RET r0 _\nEND _ _\nGENE one i32\nLOAD r0 1\nRET r0 _\nEND _ _\n", f);
        REQUIRE(fclose(f) == 0);
        p = bio_compile(path, 0, &e);
        if (!excess) {
            REQUIRE(p != NULL && p->stats.execution_steps == BIO_EXECUTION_LIMIT);
            REQUIRE(run0(p) == 1 && interpret0(p) == 1);
            bio_program_release(p);
        } else REQUIRE(p == NULL && strstr(e.message, "execution budget exceeded"));
    }
    /* A tiny DAG can expand exponentially; never execute it to test the guard. */
    for (excess = 0; excess < 2; ++excess) {
        f = fopen(path, "wb"); REQUIRE(f != NULL);
        fputs("GENE main i32\n", f);
        fputs(excess ? "LOAD r0 1\n" : "CALL g0 _\n", f);
        fputs("RET r0 _\nEND _ _\n", f);
        for (i = 0; i < 60; ++i) {
            fprintf(f, "GENE g%u i32\n", i);
            if (i < 59) fprintf(f, "CALL g%u _\nCALL g%u _\n", i + 1, i + 1);
            else fputs("LOAD r0 1\n", f);
            fputs("RET r0 _\nEND _ _\n", f);
        }
        REQUIRE(fclose(f) == 0);
        p = bio_compile(path, 0, &e);
        if (!excess) REQUIRE(p == NULL && strstr(e.message, "execution budget exceeded"));
        else {
            REQUIRE(p != NULL && p->stats.execution_steps == 2 && p->stats.live_genes == 1);
            bio_program_release(p);
        }
    }
}

static void budget_tests(void) {
    char path[256];
    FILE *f;
    BioProgram *p;
    BioError e = {{0}};
    unsigned i;
    path_for(path, sizeof(path), "budget.dna");
    f = fopen(path, "wb"); REQUIRE(f != NULL);
    fputs("GENE main i32\n", f);
    for (i = 0; i < BIO_MAX_NODES; ++i) fputs("LOAD r0 1\n", f);
    fputs("RET r0 _\nEND _ _\n", f); REQUIRE(fclose(f) == 0);
    p = compile_name("budget.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "node budget"));
    f = fopen(path, "wb"); REQUIRE(f != NULL);
    fputs("GENE main i32\nCALL g0 _\nRET r0 _\nEND _ _\n", f);
    for (i = 0; i < BIO_MAX_DEPTH; ++i) {
        fprintf(f, "GENE g%u i32\n", i);
        if (i + 1 < BIO_MAX_DEPTH) fprintf(f, "CALL g%u _\n", i + 1);
        else fputs("LOAD r0 1\n", f);
        fputs("RET r0 _\nEND _ _\n", f);
    }
    REQUIRE(fclose(f) == 0);
    p = compile_name("budget.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "call depth"));
    f = fopen(path, "wb"); REQUIRE(f != NULL);
    for (i = 0; i < BIO_LINE_SIZE; ++i) fputc(' ', f);
    REQUIRE(fclose(f) == 0);
    p = compile_name("budget.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "line too long"));
    f = fopen(path, "wb"); REQUIRE(f != NULL);
    fputs("GENE main i32\n", f); fputc(0, f); REQUIRE(fclose(f) == 0);
    p = compile_name("budget.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "NUL byte"));
    for (i = 0; i < BIO_IMPORT_DEPTH + 2; ++i) {
        char name[64], source[128];
        snprintf(name, sizeof(name), "depth%u.dna", i);
        snprintf(source, sizeof(source), "IMPORT depth%u.dna _\n", i + 1);
        write_source(name, source);
    }
    p = compile_name("depth0.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "import depth"));
    f = fopen(path, "wb"); REQUIRE(f != NULL);
    for (i = 0; i <= BIO_MAX_GENES; ++i)
        fprintf(f, "GENE f%u i32\nLOAD r0 0\nRET r0 _\nEND _ _\n", i);
    REQUIRE(fclose(f) == 0);
    p = compile_name("budget.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "gene budget"));
    f = fopen(path, "wb"); REQUIRE(f != NULL);
    for (i = 0; i <= BIO_MAX_SOURCE_BYTES / 2; ++i) fputs("#\n", f);
    REQUIRE(fclose(f) == 0);
    p = compile_name("budget.dna", 0, &e);
    REQUIRE(p == NULL && strstr(e.message, "source byte budget"));
}

typedef struct {
    BioRuntime *runtime;
    volatile LONG stop;
    volatile LONG failures;
    volatile LONG calls;
    volatile LONG ready;
    HANDLE start;
} Workers;

static DWORD WINAPI reader(LPVOID arg) {
    Workers *workers = arg;
    InterlockedIncrement(&workers->ready);
    WaitForSingleObject(workers->start, INFINITE);
    do {
        int32_t value = 0;
        BioError e = {{0}};
        if (!bio_runtime_run(workers->runtime, NULL, 0, &value, &e) || (value != 30 && value != 42))
            InterlockedIncrement(&workers->failures);
        InterlockedIncrement(&workers->calls);
    } while (!InterlockedCompareExchange(&workers->stop, 0, 0));
    return 0;
}

static void publish_tests(void) {
    BioError e = {{0}};
    BioProgram *initial, *candidate;
    BioRuntime *runtime;
    char path[256];
    write_source("pub-old.dna", "GENE main i32\nLOAD r0 30\nRET r0 _\nEND _ _\n");
    write_source("pub-new.dna", "GENE main i32\nLOAD r0 42\nRET r0 _\nEND _ _\n");
    write_source("pub-bad.dna", "GENE main i32/1\nRET r0 _\nEND _ _\n");
    initial = compile_name("pub-old.dna", 0, &e); REQUIRE(initial != NULL);
    runtime = bio_runtime_create(initial, &e); REQUIRE(runtime != NULL);
    /* The caller verifies the candidate itself, then publishes it. */
    candidate = compile_name("pub-new.dna", 0, &e); REQUIRE(candidate != NULL);
    REQUIRE(bio_runtime_publish(runtime, candidate, &e));
    REQUIRE(runtime0(runtime) == 42);
    /* Publication retained its own reference; the caller's release is safe. */
    bio_program_release(candidate);
    REQUIRE(runtime0(runtime) == 42);
    /* Incompatible and load-only (no native code) candidates are rejected. */
    candidate = compile_name("pub-bad.dna", 0, &e); REQUIRE(candidate != NULL);
    REQUIRE(!bio_runtime_publish(runtime, candidate, &e));
    REQUIRE(strstr(e.message, "incompatible signature") != NULL);
    bio_program_release(candidate);
    path_for(path, sizeof(path), "pub-old.dna");
    candidate = bio_load(path, 0, &e); REQUIRE(candidate != NULL);
    REQUIRE(!bio_runtime_publish(runtime, candidate, &e));
    bio_program_release(candidate);
    REQUIRE(runtime0(runtime) == 42);
    bio_runtime_destroy(runtime);
    bio_program_release(initial);
}

static void runtime_tests(void) {
    BioError e = {{0}};
    BioProgram *initial;
    BioRuntime *runtime;
    char new_path[256], old_path[256], bad_path[256], changed_path[256];
    HANDLE threads[4];
    Workers workers = {0};
    unsigned i;
    write_source("old.dna", "GENE main i32\nARRAY a 1\nCALL val _\nLOAD r1 0\nASET a r1,r0\nAGET r0 a,r1\nRET r0 _\nEND _ _\n"
                 "GENE val i32\nVAR x 30\nGET r0 x\nRET r0 _\nEND _ _\n");
    write_source("new.dna", "GENE main i32\nVAR saved 0\nARRAY a 32\nCALL val _\nSET saved r0\nLOAD r1 31\n"
                 "ASET a r1,r0\nGET r2 saved\nAGET r0 a,r1\nADD r0 r2\nRET r0 _\nEND _ _\n"
                 "GENE val i32\nARRAY data 7\nVAR x 21\nGET r0 x\nRET r0 _\nEND _ _\n");
    write_source("invalid.dna", "GENE main i32\nRET r0 _\nEND _ _\n");
    write_source("changed.dna", "GENE main i32\nLOAD r0 8\nCALL val r0\nRET r0 _\nEND _ _\nGENE val i32/1\nRET r0 _\nEND _ _\n");
    path_for(new_path, sizeof(new_path), "new.dna");
    path_for(old_path, sizeof(old_path), "old.dna");
    path_for(bad_path, sizeof(bad_path), "invalid.dna");
    path_for(changed_path, sizeof(changed_path), "changed.dna");
    initial = compile_name("old.dna", 0, &e); REQUIRE(initial != NULL);
    runtime = bio_runtime_create(initial, &e); REQUIRE(runtime != NULL);
    REQUIRE(runtime0(runtime) == 30);
    REQUIRE(bio_runtime_reload(runtime, new_path, 0, &e));
    REQUIRE(runtime0(runtime) == 42);
    /* An independently retained old snapshot remains executable after publication. */
    REQUIRE(run0(initial) == 30);
    bio_program_release(initial);
    REQUIRE(!bio_runtime_reload(runtime, bad_path, 0, &e));
    REQUIRE(runtime0(runtime) == 42);
    REQUIRE(!bio_runtime_reload(runtime, changed_path, 0, &e));
    REQUIRE(strstr(e.message, "incompatible signature") != NULL);
    REQUIRE(runtime0(runtime) == 42);
    workers.runtime = runtime;
    workers.start = CreateEventA(NULL, TRUE, FALSE, NULL); REQUIRE(workers.start != NULL);
    for (i = 0; i < 4; ++i) { threads[i] = CreateThread(NULL, 0, reader, &workers, 0, NULL); REQUIRE(threads[i] != NULL); }
    while (InterlockedCompareExchange(&workers.ready, 0, 0) != 4) Sleep(1);
    REQUIRE(SetEvent(workers.start));
    for (i = 0; i < 100; ++i) {
        REQUIRE(bio_runtime_reload(runtime, i % 2 ? new_path : old_path, 0, &e));
        if (i % 10 == 0) REQUIRE(!bio_runtime_reload(runtime, bad_path, 0, &e));
    }
    InterlockedExchange(&workers.stop, 1);
    REQUIRE(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    REQUIRE(workers.calls >= 4);
    REQUIRE(workers.failures == 0);
    for (i = 0; i < 4; ++i) CloseHandle(threads[i]);
    CloseHandle(workers.start);
    printf("Concurrent calls during 100 reloads: %ld\n", workers.calls);
    bio_runtime_destroy(runtime);
}

int main(void) {
    REQUIRE(CreateDirectoryA("work", NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    snprintf(directory, sizeof(directory), "work\\test-%lu", GetCurrentProcessId());
    REQUIRE(CreateDirectoryA(directory, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    language_tests();
    arithmetic_slot_tests();
    arithmetic_error_tests();
    memory_slot_tests();
    memory_error_tests();
    memory_call_and_budget_tests();
    memory_concurrent_tests();
    io_tests();
    array_parameter_tests();
    module_and_repair_tests();
    differential_tests();
    argument_tests();
    wide_call_tests();
    string_tests();
    print_tests();
    flow_tests();
    loop_budget_tests();
    loop_runtime_tests();
    execution_budget_tests();
    budget_tests();
    publish_tests();
    runtime_tests();
    printf("PASS: %u checks; 250 generated programs matched JIT, interpreter and reference.\n", checks);
    return 0;
}
