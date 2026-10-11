#include "internal.h"
#include "cli.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *command;
    const char *path;
    const char *output;
    const char *input_list;
    const char *output_list;
    int32_t arguments[2];
    size_t argument_count;
    int repair;
    int json;
} Options;

static void usage(void) {
    puts("Codons " BIO_VERSION " - a small native language for Windows x64\n"
         "\nGet started:\n"
         "  codons new hello.dna\n"
         "  codons run hello.dna 10 20\n"
         "\nCommands:\n"
         "  new <file.dna>                 Create an example; never overwrite a file\n"
         "  run <file.dna> [number ...]     Compile and run with 0, 1 or 2 i32 inputs\n"
         "  test <file.dna> <cases.csv>     Compare CSV examples with native results\n"
         "  check <file.dna>               Validate without running or preparing JIT\n"
         "  interpret <file.dna> [number ...]  Run the reference interpreter\n"
         "  dump <file.dna>                Show the normalized program\n"
         "  live <file.dna>                Run and reload in one process\n"
         "  emit <file.dna> <code.bin>     Export raw machine code (not an .exe)\n"
         "\nOptions after the file: --repair, --json\n"
         "  --repair restores only an unused '_' in RET/END\n"
         "  --json is available for new, run, test, check, interpret and emit\n"
         "  run/interpret: --input <numbers.txt> --output <result.txt>\n"
         "  Lists: one i32 per nonblank UTF-8 line; output must be a new file\n"
         "CSV header: expected | arg1,expected | arg1,arg2,expected\n"
         "Live commands: run [number ...] | reload <file.dna> | quit\n"
         "Every run shares a 1000000-step budget across loops, calls, initialization and list copies.\n"
         "Exit codes: 0 success, 1 failed tests or program/file error, 2 invalid command or arguments");
}

void cli_json_string(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    putchar('"');
    for (; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", (unsigned)*p);
        else putchar(*p);
    }
    putchar('"');
}

static int failure(int json, const char *stage, const char *message, int code) {
    if (json) {
        fputs("{\"ok\":false,\"error\":{\"stage\":", stdout); cli_json_string(stage);
        fputs(",\"message\":", stdout); cli_json_string(message); puts("}}");
    } else fprintf(stderr, "%s\n", message);
    return code;
}

int cli_parse_i32(const char *s, int32_t *out) {
    const char *digit = s;
    char *end;
    long long value;
    if (*digit == '+' || *digit == '-') ++digit;
    if (!*digit) return 0;
    for (; *digit; ++digit) if (*digit < '0' || *digit > '9') return 0;
    errno = 0;
    value = strtoll(s, &end, 10);
    if (errno || *end || value < INT32_MIN || value > INT32_MAX) return 0;
    *out = (int32_t)value;
    return 1;
}

static int options(int argc, char **argv, Options *o, BioError *e) {
    int i, is_emit, is_test;
    for (i = 1; i < argc; ++i) if (!strcmp(argv[i], "--json")) o->json = 1;
    if (argc < 3) { bio_error(e, "expected a command and file; use --help"); return 0; }
    o->command = argv[1]; o->path = argv[2];
    is_emit = !strcmp(o->command, "emit");
    is_test = !strcmp(o->command, "test");
    if (strcmp(o->command, "new") && strcmp(o->command, "run") && strcmp(o->command, "check") &&
        strcmp(o->command, "interpret") && strcmp(o->command, "dump") &&
        strcmp(o->command, "live") && !is_emit && !is_test) {
        bio_error(e, "unknown command '%s'; use --help", o->command); return 0;
    }
    if (is_emit || is_test) {
        if (argc < 4 || !strncmp(argv[3], "--", 2)) {
            bio_error(e, "%s requires %s", o->command, is_test ? "a CSV examples file" : "an output file"); return 0;
        }
        o->output = argv[3];
    }
    for (i = (is_emit || is_test) ? 4 : 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--repair")) o->repair = 1;
        else if (!strcmp(argv[i], "--json")) o->json = 1;
        else if (!strcmp(argv[i], "--input") || !strcmp(argv[i], "--output")) {
            const char **target = !strcmp(argv[i], "--input") ? &o->input_list : &o->output_list;
            if (strcmp(o->command, "run") && strcmp(o->command, "interpret")) {
                bio_error(e, "%s is supported only for run and interpret", argv[i]); return 0;
            }
            if (*target || i + 1 == argc || !argv[i + 1][0] || !strncmp(argv[i + 1], "--", 2)) {
                bio_error(e, "%s requires exactly one file path and cannot be repeated", argv[i]); return 0;
            }
            *target = argv[++i];
        } else if ((!strcmp(o->command, "run") || !strcmp(o->command, "interpret")) &&
                 o->argument_count < 2 && cli_parse_i32(argv[i], &o->arguments[o->argument_count]))
            ++o->argument_count;
        else {
            if (!strcmp(o->command, "run") || !strcmp(o->command, "interpret"))
                bio_error(e, "unexpected argument '%s'; inputs must be at most two decimal i32 values", argv[i]);
            else bio_error(e, "unexpected argument '%s' for %s; use --help", argv[i], o->command);
            return 0;
        }
    }
    if (o->json && (!strcmp(o->command, "live") || !strcmp(o->command, "dump"))) {
        bio_error(e, "--json is not supported for live or dump"); return 0;
    }
    if (o->repair && !strcmp(o->command, "new")) {
        bio_error(e, "--repair is not supported for new"); return 0;
    }
    return 1;
}

static void stats(BioStats s, int json) {
    if (json) {
        printf("\"stats\":{\"nodes\":%zu,\"genes\":%zu,\"live_genes\":%zu,\"files\":%zu,"
               "\"code_bytes\":%zu,\"repairs\":%zu,\"arity\":%u,\"execution_steps\":",
               s.nodes, s.genes, s.live_genes, s.files, s.code_bytes, s.repairs, s.arity);
        if (s.dynamic_steps) fputs("null", stdout);
        else printf("%" PRIu64, s.execution_steps);
        printf(",\"dynamic_steps\":%s,\"execution_limit\":%u,\"entry_offset\":%u}",
               s.dynamic_steps ? "true" : "false", BIO_EXECUTION_LIMIT, s.entry_offset);
    } else {
        printf("Valid: inputs=%u steps=", s.arity);
        if (s.dynamic_steps) fputs("runtime", stdout);
        else printf("%" PRIu64, s.execution_steps);
        printf(" limit=%u genes=%zu live=%zu files=%zu repairs=%zu\n",
               BIO_EXECUTION_LIMIT, s.genes, s.live_genes, s.files, s.repairs);
    }
}

static int create_source(const char *path, BioError *e) {
    static const char source[] =
        "# Run with: codons run <this-file.dna> 10 20\n"
        "# Inputs are r0 and r1. The result is 30.\n"
        "GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n";
    wchar_t wide[BIO_PATH_SIZE];
    HANDLE file;
    DWORD written = 0;
    int ok;
    if (!bio_wide_path(path, wide, BIO_PATH_SIZE)) {
        bio_error(e, "invalid UTF-8 path or path too long"); return 0;
    }
    file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            bio_error(e, "file already exists; choose a new path: %s", path);
        else bio_error(e, "cannot create %s (Windows error %lu)", path, error);
        return 0;
    }
    ok = WriteFile(file, source, (DWORD)(sizeof(source) - 1), &written, NULL) && written == sizeof(source) - 1;
    if (!CloseHandle(file)) ok = 0;
    if (!ok) bio_error(e, "could not finish writing %s", path);
    return ok;
}

static int live(BioProgram *p, int repair, BioError *e) {
    BioRuntime *runtime = bio_runtime_create(p, e);
    char line[1200];
    bio_program_release(p); /* Transfer CLI ownership to the runtime, including on failure. */
    if (!runtime) return failure(0, "runtime", e->message, 1);
    puts("Ready. Commands: run [number ...] | reload <file.dna> | quit");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            fputs("command too long\n", stderr);
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, "quit")) break;
        if (!strncmp(line, "run", 3) && (!line[3] || isspace((unsigned char)line[3]))) {
            int32_t args[2], result;
            size_t argc = 0;
            char *part = strtok(line + 3, " \t");
            int valid = 1;
            while (part) {
                if (argc == 2 || !cli_parse_i32(part, &args[argc])) { valid = 0; break; }
                ++argc; part = strtok(NULL, " \t");
            }
            if (!valid) fputs("run accepts at most two decimal i32 inputs\n", stderr);
            else if (bio_runtime_run(runtime, args, argc, &result, e)) printf("result=%" PRId32 "\n", result);
            else fprintf(stderr, "%s\n", e->message);
        } else if (!strncmp(line, "reload ", 7) && line[7]) {
            if (bio_runtime_reload(runtime, line + 7, repair, e)) puts("updated");
            else fprintf(stderr, "%s\nUpdate rejected; previous version is still active.\n", e->message);
        } else puts("expected: run [number ...] | reload <file.dna> | quit");
        fflush(stdout);
    }
    bio_runtime_destroy(runtime);
    return 0;
}

static int cli(int argc, char **argv) {
    Options o = {0};
    BioError e = {{0}};
    BioProgram *p;
    BioStats s;
    int code = 0, is_run, is_interpret;
    if (argc == 1 || (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))) {
        usage(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts("Codons " BIO_VERSION); return 0; }
    if (!options(argc, argv, &o, &e)) return failure(o.json, "arguments", e.message, 2);
    if (!strcmp(o.command, "new")) {
        if (!create_source(o.path, &e)) return failure(o.json, "file", e.message, 1);
        if (o.json) { fputs("{\"ok\":true,\"file\":", stdout); cli_json_string(o.path); puts("}"); }
        else printf("Created %s\nRun: codons run \"%s\" 10 20\n", o.path, o.path);
        return 0;
    }
    is_run = !strcmp(o.command, "run");
    is_interpret = !strcmp(o.command, "interpret");
    if (is_interpret || !strcmp(o.command, "check") || !strcmp(o.command, "dump"))
        p = bio_load(o.path, o.repair, &e);
    else p = bio_compile(o.path, o.repair, &e);
    if (!p) return failure(o.json, "compile", e.message, 1);
    s = bio_program_stats(p);
    if (s.repairs && !o.json) fprintf(stderr, "repair: restored %zu unused '_' fields\n", s.repairs);
    if (is_run || is_interpret) {
        int32_t result;
        if (o.argument_count != s.arity) {
            bio_error(&e, "main expects %u arguments, got %zu", s.arity, o.argument_count);
            code = failure(o.json, "arguments", e.message, 2);
        } else {
            BioIO io = {0};
            int32_t *input = NULL, *output = NULL;
            size_t input_count = 0;
            int ok = 1;
            const char *stage = "file";
            if (o.input_list) ok = cli_read_list(o.input_list, &input, &input_count, &e);
            if (ok && o.output_list) {
                output = malloc(BIO_LIST_LIMIT * sizeof(*output));
                if (!output) { bio_error(&e, "out of memory preparing output list"); ok = 0; }
            }
            io.input = input; io.input_count = input_count;
            io.output = output; io.output_capacity = output ? BIO_LIST_LIMIT : 0;
            if (ok) {
                stage = "run";
                ok = is_interpret ? bio_program_interpret_io(p, o.arguments, o.argument_count, &io, &result, &e) :
                                    bio_program_run_io(p, o.arguments, o.argument_count, &io, &result, &e);
            }
            if (ok && o.output_list) {
                stage = "file";
                if (!io.output_written) { bio_error(&e, "program did not execute WRITE; output file was not created"); ok = 0; }
                else ok = cli_write_list(o.output_list, output, io.output_count, &e);
            }
            if (!ok) code = failure(o.json, stage, e.message, 1);
            else if (o.json) {
                printf("{\"ok\":true,\"result\":%" PRId32 ",", result); stats(s, 1);
                if (o.output_list) {
                    fputs(",\"output\":", stdout); cli_json_string(o.output_list);
                    printf(",\"output_count\":%zu", io.output_count);
                }
                puts("}");
            } else printf("%" PRId32 "\n", result);
            free(input); free(output);
        }
    } else if (!strcmp(o.command, "check")) {
        if (o.json) { fputs("{\"ok\":true,", stdout); stats(s, 1); puts("}"); }
        else stats(s, 0);
    } else if (!strcmp(o.command, "test")) code = cli_test(p, o.output, o.json);
    else if (!strcmp(o.command, "dump")) bio_program_dump(p, stdout);
    else if (!strcmp(o.command, "live")) return live(p, o.repair, &e);
    else if (!strcmp(o.command, "emit")) {
        if (!bio_program_write_code(p, o.output, &e)) code = failure(o.json, "file", e.message, 1);
        else if (o.json) { fputs("{\"ok\":true,", stdout); stats(s, 1); puts("}"); }
        else printf("Wrote %zu bytes; main offset=%u. Raw x64 code, not a standalone .exe.\n", s.code_bytes, s.entry_offset);
    }
    bio_program_release(p);
    return code;
}

int wmain(int argc, wchar_t **wide_argv) {
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    int i, code = 1;
    UINT old_output = GetConsoleOutputCP(), old_input = GetConsoleCP();
    if (!argv) { fputs("out of memory\n", stderr); return 1; }
    if (old_output) SetConsoleOutputCP(CP_UTF8);
    if (old_input) SetConsoleCP(CP_UTF8);
    for (i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (!size || !(argv[i] = malloc((size_t)size))) {
            fputs("invalid command-line encoding or out of memory\n", stderr); goto cleanup;
        }
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, argv[i], size, NULL, NULL))
            goto cleanup;
    }
    code = cli(argc, argv);
cleanup:
    for (i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    if (old_output) SetConsoleOutputCP(old_output);
    if (old_input) SetConsoleCP(old_input);
    return code;
}
