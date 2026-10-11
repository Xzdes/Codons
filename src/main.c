#include "bio.h"
#include <inttypes.h>
#include <string.h>

static void usage(void) {
    puts("AI-Native Bio-Compiler 0.1 (Windows x64, C11)\n"
         "  codons run <file.dna> [--repair]\n"
         "  codons check <file.dna> [--repair]\n"
         "  codons interpret <file.dna> [--repair]\n"
         "  codons dump <file.dna>\n"
         "  codons emit <file.dna> <code.bin>\n"
         "  codons live <file.dna> [--repair]\n"
         "Live commands: run | reload <file.dna> | quit");
}

static void stats(BioProgram *p) {
    BioStats s = bio_program_stats(p);
    printf("nodes=%zu genes=%zu live=%zu pruned=%zu files=%zu code=%zu bytes repairs=%zu\n",
           s.nodes, s.genes, s.live_genes, s.genes - s.live_genes, s.files, s.code_bytes, s.repairs);
}

static int live(BioProgram *p, int repair, BioError *e) {
    BioRuntime *runtime = bio_runtime_create(p, e);
    char line[1200];
    if (!runtime) { fprintf(stderr, "%s\n", e->message); return 1; }
    /* Transfer CLI ownership so replaced generations can actually be reclaimed. */
    bio_program_release(p);
    puts("Live runtime ready. Commands: run | reload <file.dna> | quit");
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            fputs("command too long\n", stderr);
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "quit") == 0) break;
        if (strcmp(line, "run") == 0) printf("result=%" PRId32 "\n", bio_runtime_run(runtime));
        else if (strncmp(line, "reload ", 7) == 0 && line[7]) {
            if (bio_runtime_reload(runtime, line + 7, repair, e)) puts("updated");
            else fprintf(stderr, "%s\n", e->message);
        } else puts("expected: run | reload <file.dna> | quit");
        fflush(stdout);
    }
    bio_runtime_destroy(runtime);
    return 0;
}

int main(int argc, char **argv) {
    BioError e = {{0}};
    BioProgram *p;
    int repair = 0, result = 0;
    int emit, is_live;
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(); return 0;
    }
    if (argc < 3) { usage(); return 2; }
    emit = strcmp(argv[1], "emit") == 0;
    is_live = strcmp(argv[1], "live") == 0;
    if (strcmp(argv[1], "run") && strcmp(argv[1], "check") && strcmp(argv[1], "interpret") &&
        strcmp(argv[1], "dump") && !emit && !is_live) { usage(); return 2; }
    if (emit) {
        if (argc != 4) { usage(); return 2; }
    } else if (argc == 4 && strcmp(argv[3], "--repair") == 0) repair = 1;
    else if (argc != 3) { usage(); return 2; }
    p = bio_compile(argv[2], repair, &e);
    if (!p) { fprintf(stderr, "%s\n", e.message); return 1; }
    if (is_live) {
        result = live(p, repair, &e);
        if (result) bio_program_release(p);
        return result;
    }
    if (repair && bio_program_stats(p).repairs)
        fprintf(stderr, "repair: restored %zu unused '_' fields\n", bio_program_stats(p).repairs);
    if (strcmp(argv[1], "run") == 0) printf("%" PRId32 "\n", bio_program_run(p));
    else if (strcmp(argv[1], "interpret") == 0) printf("%" PRId32 "\n", bio_program_interpret(p));
    else if (strcmp(argv[1], "check") == 0) stats(p);
    else if (strcmp(argv[1], "dump") == 0) bio_program_dump(p, stdout);
    else if (emit) {
        if (!bio_program_write_code(p, argv[3], &e)) { fprintf(stderr, "%s\n", e.message); result = 1; }
        else { stats(p); puts("Raw x64 code written (not a standalone executable). Use dump for entry offset."); }
    }
    bio_program_release(p);
    return result;
}
