/* parser_fuzz.c — libFuzzer coverage-guided target for the Codons parser and
 * analyzer only (no JIT, no executable memory, no execution).
 *
 * This is deliberately separate from tests/fuzz_diff.py, which compares the JIT
 * and interpreter BACKENDS. This target fuzzes the front-end instead: the bytes
 * are run through bio_load(), which is "load and validate" = bio_parse() followed
 * by bio_analyze() (see include/bio.h). bio_compile() would additionally build
 * the JIT image; we intentionally do NOT call it here.
 *
 * bio_load() takes a UTF-8 PATH, not a buffer, so each input is first written to
 * a per-process temp file, then loaded in strict mode (repair=0) and again in
 * repair mode (repair=1) to exercise both the strict diagnostics and the
 * repair/quarantine code paths.
 *
 * Build (Linux / CI — libFuzzer is not supported by clang on Windows):
 *   clang -g -O1 -std=c11 -fsanitize=fuzzer,address,undefined \
 *       -fno-omit-frame-pointer -Iinclude \
 *       tests/fuzz/parser_fuzz.c src/parser.c src/analyze.c src/core.c src/runtime.c \
 *       -o build/parser_fuzz
 *
 *   # run with the seed corpus
 *   ./build/parser_fuzz tests/fuzz/corpus -max_total_time=60 -print_final_stats=1
 *
 * On Windows this file is only checked to compile warning-clean (no fuzzer
 * runtime):  clang -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -c tests/fuzz/parser_fuzz.c
 */

/* MSVC's UCRT marks fopen() "deprecated" (a Windows-only style warning that has
 * nothing to do with C correctness). Defining this keeps clang-cl / MSVC-clang
 * warning-clean under -Werror; on Linux/gcc it has no effect. */
#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "bio.h"

#if defined(_WIN32)
#include <process.h>
#define FUZZ_PROCESS_ID ((int)_getpid())
#else
#include <unistd.h>
#define FUZZ_PROCESS_ID ((int)getpid())
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char path[256];
    BioError err;
    BioProgram *prog;
    FILE *f;

    /* Empty input and inputs that would not fit the temp path are ignored. */
    if (size == 0) {
        return 0;
    }

    if (snprintf(path, sizeof(path), ".codons_fuzz_%d.dna", FUZZ_PROCESS_ID) < 0) {
        return 0;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        remove(path);
        return 0;
    }
    fclose(f);

    /* Strict parse + analyze. */
    prog = bio_load(path, 0, &err);
    if (prog != NULL) {
        bio_program_release(prog);
    }

    /* Repair-enabled parse + analyze. */
    prog = bio_load(path, 1, &err);
    if (prog != NULL) {
        bio_program_release(prog);
    }

    remove(path);
    return 0;
}
