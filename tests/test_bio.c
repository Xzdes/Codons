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
    DWORD64 fake_stack[10] = {0};
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
    /* Exercise Windows' actual unwinder after the JIT's 72-byte prolog. */
    fake_stack[9] = 0x12345678u;
    context.ContextFlags = CONTEXT_FULL;
    context.Rip = base + table->BeginAddress + 4;
    context.Rsp = (DWORD64)(uintptr_t)fake_stack;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, table, &context,
                     &handler_data, &frame, NULL);
    REQUIRE(context.Rip == fake_stack[9]);
    REQUIRE(context.Rsp == (DWORD64)(uintptr_t)fake_stack + 80);
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
    expect_error("GENE main i32\nCALL f r0,r1,r2\n", "CALL operands");
    expect_error("GENE main i32\nMOV r0 r1\nRET r0 _\nEND _ _\n", "uninitialized register");
    expect_error("GENE main i32\nIMPORT f.dna _\n", "outside a GENE");
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
    REQUIRE(p == NULL && strstr(e.message, "exactly 3 fields"));
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

static void runtime_tests(void) {
    BioError e = {{0}};
    BioProgram *initial;
    BioRuntime *runtime;
    char new_path[256], old_path[256], bad_path[256], changed_path[256];
    HANDLE threads[4];
    Workers workers = {0};
    unsigned i;
    write_source("old.dna", "GENE main i32\nCALL val _\nRET r0 _\nEND _ _\nGENE val i32\nLOAD r0 30\nRET r0 _\nEND _ _\n");
    write_source("new.dna", "GENE main i32\nCALL val _\nRET r0 _\nEND _ _\nGENE val i32\nLOAD r0 42\nRET r0 _\nEND _ _\n");
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
    module_and_repair_tests();
    differential_tests();
    argument_tests();
    execution_budget_tests();
    budget_tests();
    runtime_tests();
    printf("PASS: %u checks; 250 generated programs matched JIT, interpreter and reference.\n", checks);
    return 0;
}
