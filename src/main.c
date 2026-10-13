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
    int32_t arguments[4];
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
         "  run <file.dna> [number ...]     Compile and run with 0 to 4 i32 inputs\n"
         "  test <file.dna> <cases.csv>     Compare CSV examples, including file lists\n"
         "  check <file.dna>               Validate without running or preparing JIT\n"
         "  interpret <file.dna> [number ...]  Run the reference interpreter\n"
         "  verify <file.dna> [number ...]    Run JIT and interpreter; require identical results\n"
         "  dump <file.dna>                Show the normalized program\n"
         "  live <file.dna>                Run and reload in one process\n"
         "  emit <file.dna> <code.bin>     Export raw machine code (not an .exe)\n"
         "\nOptions after the file: --repair, --json\n"
         "  --repair restores safe '_' fields and quarantines ambiguous codons\n"
         "  --json is available for new, run, test, check, interpret, verify and emit\n"
         "  run/interpret: --input <numbers.txt> --output <result.txt>\n"
         "  Lists: one i32 per nonblank UTF-8 line; output must be a new file\n"
         "  PRINT writes bytes to stdout; --json reports them as \"printed\"\n"
         "CSV header: expected | arg1,expected | arg1,arg2,expected\n"
         "CSV with lists: add input before expected and expected_output after it\n"
         "List paths are relative to CSV; test compares in memory without writing files\n"
         "Live commands: run [number ...] | reload <file.dna> [--gate cases.csv] | quit\n"
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

/* Errors from the compiler/runtime read "path:line: text". For JSON consumers
   (often LLM agents) we attach the offending source line as "line_text" so the
   model sees what it wrote without re-reading the file. */
static void print_line_text(const char *message) {
    const char *colon = message;
    char path[1024];
    char line_buf[1200], *start;
    unsigned long line_no = 0;
    size_t length;
    FILE *f;
    for (;;) { /* Paths may contain ':' (drive letters); need ':' + digits + ': '. */
        colon = strchr(colon, ':');
        if (!colon) return;
        if (colon[1] >= '0' && colon[1] <= '9') {
            char *end = NULL;
            line_no = strtoul(colon + 1, &end, 10);
            if (end && end[0] == ':' && end[1] == ' ') break;
        }
        ++colon;
    }
    length = (size_t)(colon - message);
    if (!length || length >= sizeof(path)) return;
    memcpy(path, message, length); path[length] = '\0';
    f = bio_fopen(path, L"rb");
    if (!f) return;
    while (line_no && fgets(line_buf, sizeof(line_buf), f)) --line_no;
    fclose(f);
    if (line_no) return;
    start = line_buf;
    if ((unsigned char)start[0] == 0xef && (unsigned char)start[1] == 0xbb &&
        (unsigned char)start[2] == 0xbf) start += 3;
    length = strlen(start);
    while (length && (start[length - 1] == '\n' || start[length - 1] == '\r'))
        start[--length] = '\0';
    /* A damaged line is not valid UTF-8; never break the JSON with raw bytes. */
    {
        const unsigned char *s = (const unsigned char *)start;
        int valid = 1;
        while (*s && valid) {
            if (*s < 0x80) { ++s; continue; }
            if ((*s & 0xe0u) == 0xc0u && (s[1] & 0xc0u) == 0x80u && s[1]) s += 2;
            else if ((*s & 0xf0u) == 0xe0u && (s[1] & 0xc0u) == 0x80u && s[1] &&
                     (s[2] & 0xc0u) == 0x80u && s[2]) s += 3;
            else if ((*s & 0xf8u) == 0xf0u && (s[1] & 0xc0u) == 0x80u && s[1] &&
                     (s[2] & 0xc0u) == 0x80u && s[2] && (s[3] & 0xc0u) == 0x80u && s[3]) s += 4;
            else valid = 0;
        }
        if (!valid) return;
    }
    fputs(",\"line_text\":", stdout); cli_json_string(start);
}

static int failure(int json, const char *stage, const char *message, int code) {
    if (json) {
        fputs("{\"ok\":false,\"error\":{\"stage\":", stdout); cli_json_string(stage);
        fputs(",\"message\":", stdout); cli_json_string(message);
        print_line_text(message);
        puts("}}");
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
        strcmp(o->command, "interpret") && strcmp(o->command, "verify") && strcmp(o->command, "dump") &&
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
            if (strcmp(o->command, "run") && strcmp(o->command, "interpret") && strcmp(o->command, "verify")) {
                bio_error(e, "%s is supported only for run, interpret and verify", argv[i]); return 0;
            }
            if (*target || i + 1 == argc || !argv[i + 1][0] || !strncmp(argv[i + 1], "--", 2)) {
                bio_error(e, "%s requires exactly one file path and cannot be repeated", argv[i]); return 0;
            }
            *target = argv[++i];
        } else if ((!strcmp(o->command, "run") || !strcmp(o->command, "interpret") ||
                    !strcmp(o->command, "verify")) &&
                 o->argument_count < 4 && cli_parse_i32(argv[i], &o->arguments[o->argument_count]))
            ++o->argument_count;
        else {
            if (!strcmp(o->command, "run") || !strcmp(o->command, "interpret") || !strcmp(o->command, "verify"))
                bio_error(e, "unexpected argument '%s'; inputs must be at most four decimal i32 values", argv[i]);
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
               "\"code_bytes\":%zu,\"repairs\":%zu,\"quarantines\":%zu,\"arity\":%u,\"execution_steps\":",
               s.nodes, s.genes, s.live_genes, s.files, s.code_bytes, s.repairs, s.quarantines, s.arity);
        if (s.dynamic_steps) fputs("null", stdout);
        else printf("%" PRIu64, s.execution_steps);
        printf(",\"dynamic_steps\":%s,\"execution_limit\":%u,\"entry_offset\":%u}",
               s.dynamic_steps ? "true" : "false", BIO_EXECUTION_LIMIT, s.entry_offset);
    } else {
        printf("Valid: inputs=%u steps=", s.arity);
        if (s.dynamic_steps) fputs("runtime", stdout);
        else printf("%" PRIu64, s.execution_steps);
        printf(" limit=%u genes=%zu live=%zu files=%zu repairs=%zu quarantines=%zu\n",
               BIO_EXECUTION_LIMIT, s.genes, s.live_genes, s.files, s.repairs, s.quarantines);
    }
}

static void repair_report(const BioProgram *p, int json) {
    size_t i, count = bio_program_repair_count(p);
    if (!count) return;
    if (json) {
        fputs(",\"repair_log\":[", stdout);
        for (i = 0; i < count; ++i) {
            BioRepair repair;
            if (i) fputc(',', stdout);
            bio_program_repair_at(p, i, &repair);
            fputs("{\"file\":", stdout); cli_json_string(repair.file);
            printf(",\"line\":%u,\"kind\":", repair.line); cli_json_string(repair.kind);
            fputs(",\"detail\":", stdout); cli_json_string(repair.detail); fputc('}', stdout);
        }
        fputc(']', stdout);
    } else {
        for (i = 0; i < count; ++i) {
            BioRepair repair;
            bio_program_repair_at(p, i, &repair);
            fprintf(stderr, "repair: %s:%u: %s: %s\n", repair.file, repair.line, repair.kind, repair.detail);
        }
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

/* One backend outcome in a verify report. */
static void verify_outcome_json(int index, int ok, int32_t result, const BioError *error) {
    printf(",\"%s\":", index ? "interpret" : "run");
    if (ok) printf("{\"ok\":true,\"result\":%" PRId32 "}", result);
    else { fputs("{\"ok\":false,\"error\":", stdout); cli_json_string(error->message); putchar('}'); }
}

/* Inline JSON form of an output list: no temporary file needed by the caller. */
static void json_i32_list(const int32_t *values, size_t count) {
    size_t i;
    putchar('[');
    for (i = 0; i < count; ++i) { if (i) putchar(','); printf("%" PRId32, values[i]); }
    putchar(']');
}

/* Run the JIT and the reference interpreter on identical input and require
   identical observable behavior: result, output list, or the same fault. */
static int cli_verify(BioProgram *p, const Options *o, BioError *e) {
    BioStats s = bio_program_stats(p);
    BioIO io[2] = {{0}, {0}};
    BioError errors[2] = {{{0}}, {{0}}};
    int32_t *input = NULL, *outputs[2] = {NULL, NULL};
    int32_t results[2] = {0, 0};
    size_t input_count = 0, i;
    int ok[2], agree, code = 1;
    CliPrint printed[2] = {{0}, {0}};
    if (o->argument_count != s.arity) {
        bio_error(e, "main expects %u arguments, got %zu", s.arity, o->argument_count);
        return failure(o->json, "arguments", e->message, 2);
    }
    if (o->input_list && !cli_read_list(o->input_list, &input, &input_count, e))
        return failure(o->json, "file", e->message, 1);
    if (o->output_list || o->json) {
        outputs[0] = malloc(BIO_LIST_LIMIT * sizeof(*outputs[0]));
        outputs[1] = malloc(BIO_LIST_LIMIT * sizeof(*outputs[1]));
        if (!outputs[0] || !outputs[1]) {
            bio_error(e, "out of memory preparing output lists");
            free(input); free(outputs[0]); free(outputs[1]);
            return failure(o->json, "file", e->message, 1);
        }
    }
    for (i = 0; i < 2; ++i) {
        io[i].input = input; io[i].input_count = input_count;
        io[i].output = outputs[i];
        io[i].output_capacity = outputs[i] ? BIO_LIST_LIMIT : 0;
        io[i].print = cli_print_capture; io[i].print_user = &printed[i];
    }
    ok[0] = bio_program_run_io(p, o->arguments, o->argument_count, &io[0], &results[0], &errors[0]);
    ok[1] = bio_program_interpret_io(p, o->arguments, o->argument_count, &io[1], &results[1], &errors[1]);
    agree = ok[0] == ok[1];
    if (agree && ok[0]) {
        agree = results[0] == results[1] &&
            io[0].output_written == io[1].output_written &&
            io[0].output_count == io[1].output_count &&
            printed[0].length == printed[1].length &&
            (!printed[0].length || !memcmp(printed[0].data, printed[1].data, printed[0].length));
        for (i = 0; agree && i < io[0].output_count; ++i)
            agree = outputs[0][i] == outputs[1][i];
    }
    if (agree && !ok[0])
        agree = !strcmp(errors[0].message, errors[1].message);
    if (!agree) {
        if (o->json) {
            fputs("{\"ok\":false,\"verified\":false", stdout);
            for (i = 0; i < 2; ++i) verify_outcome_json((int)i, ok[i], results[i], &errors[i]);
            puts("}");
        } else {
            fputs("verify: DIVERGENCE between JIT and interpreter\n", stderr);
            for (i = 0; i < 2; ++i) {
                fprintf(stderr, "  %s: ", i ? "interpret" : "run");
                if (ok[i]) fprintf(stderr, "result=%" PRId32 "\n", results[i]);
                else fprintf(stderr, "%s\n", errors[i].message);
            }
        }
        goto done;
    }
    if (!ok[0]) {
        if (o->json) {
            fputs("{\"ok\":false,\"verified\":true,\"error\":{\"stage\":\"run\",\"message\":", stdout);
            cli_json_string(errors[0].message);
            print_line_text(errors[0].message);
            puts("}}");
        } else fprintf(stderr, "verified: JIT and interpreter agree on the failure\n%s\n", errors[0].message);
        goto done;
    }
    if (o->output_list) {
        if (!io[0].output_written) {
            bio_error(e, "program did not execute WRITE; output file was not created");
            code = failure(o->json, "file", e->message, 1); goto done;
        }
        if (!cli_write_list(o->output_list, outputs[0], io[0].output_count, e)) {
            code = failure(o->json, "file", e->message, 1); goto done;
        }
    }
    if (o->json) {
        printf("{\"ok\":true,\"verified\":true,\"result\":%" PRId32 ",", results[0]); stats(s, 1);
        if (o->output_list) {
            fputs(",\"output\":", stdout); cli_json_string(o->output_list);
            printf(",\"output_count\":%zu", io[0].output_count);
        }
        if (io[0].output_written) {
            fputs(",\"output_list\":", stdout); json_i32_list(outputs[0], io[0].output_count);
        }
        fputs(",\"printed\":", stdout); cli_json_bytes(printed[0].data, printed[0].length);
        repair_report(p, 1);
        puts("}");
    } else {
        if (printed[0].length) fwrite(printed[0].data, 1, printed[0].length, stdout);
        printf("%" PRId32 "\n", results[0]);
        fputs("verified: JIT and interpreter agree\n", stderr);
    }
    code = 0;
done:
    free(input); free(outputs[0]); free(outputs[1]);
    free(printed[0].data); free(printed[1].data);
    return code;
}

static int live(BioProgram *p, int repair, BioError *e) {
    BioRuntime *runtime = bio_runtime_create(p, e);
    char line[1200];
    bio_program_release(p); /* Transfer CLI ownership to the runtime, including on failure. */
    if (!runtime) return failure(0, "runtime", e->message, 1);
    puts("Ready. Commands: run [number ...] | reload <file.dna> [--gate cases.csv] | quit");
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
            int32_t args[4], result;
            size_t argc = 0;
            char *part = strtok(line + 3, " \t");
            int valid = 1;
            while (part) {
                if (argc == 4 || !cli_parse_i32(part, &args[argc])) { valid = 0; break; }
                ++argc; part = strtok(NULL, " \t");
            }
            if (!valid) fputs("run accepts at most four decimal i32 inputs\n", stderr);
            else {
                BioIO io = {0};
                io.print = cli_print_stdout;
                if (bio_runtime_run_io(runtime, args, argc, &io, &result, e)) printf("result=%" PRId32 "\n", result);
                else fprintf(stderr, "%s\n", e->message);
            }
        } else if (!strncmp(line, "reload ", 7) && line[7]) {
            char *path = strtok(line + 7, " \t");
            char *flag = strtok(NULL, " \t");
            const char *gate = NULL;
            int malformed = 0;
            if (flag) {
                if (!strcmp(flag, "--gate")) {
                    gate = strtok(NULL, " \t");
                    malformed = !gate || strtok(NULL, " \t") != NULL;
                } else malformed = 1;
            }
            if (malformed) fputs("expected: reload <file.dna> [--gate <cases.csv>]\n", stderr);
            else if (!gate) {
                if (bio_runtime_reload(runtime, path, repair, e)) puts("updated");
                else fprintf(stderr, "%s\nUpdate rejected; previous version is still active.\n", e->message);
            } else {
                /* Compile, prove the candidate against the CSV gate, then publish. */
                BioProgram *candidate = bio_compile(path, repair, e);
                if (!candidate)
                    fprintf(stderr, "%s\nUpdate rejected; previous version is still active.\n", e->message);
                else {
                    if (cli_test(candidate, gate, 0) != 0)
                        fputs("Gate rejected the update; previous version is still active.\n", stderr);
                    else if (bio_runtime_publish(runtime, candidate, e)) puts("updated (gate passed)");
                    else fprintf(stderr, "%s\nUpdate rejected; previous version is still active.\n", e->message);
                    bio_program_release(candidate);
                }
            }
        } else puts("expected: run [number ...] | reload <file.dna> [--gate cases.csv] | quit");
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
    if (s.repairs && !o.json) repair_report(p, 0);
    if (is_run || is_interpret) {
        int32_t result;
        if (o.argument_count != s.arity) {
            bio_error(&e, "main expects %u arguments, got %zu", s.arity, o.argument_count);
            code = failure(o.json, "arguments", e.message, 2);
        } else {
            BioIO io = {0};
            CliPrint printed = {0};
            int32_t *input = NULL, *output = NULL;
            size_t input_count = 0;
            int ok = 1;
            const char *stage = "file";
            if (o.input_list) ok = cli_read_list(o.input_list, &input, &input_count, &e);
            if (ok && (o.output_list || o.json)) {
                output = malloc(BIO_LIST_LIMIT * sizeof(*output));
                if (!output) { bio_error(&e, "out of memory preparing output list"); ok = 0; }
            }
            io.input = input; io.input_count = input_count;
            io.output = output; io.output_capacity = output ? BIO_LIST_LIMIT : 0;
            io.print = o.json ? cli_print_capture : cli_print_stdout;
            io.print_user = &printed;
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
                if (io.output_written) {
                    fputs(",\"output_list\":", stdout); json_i32_list(output, io.output_count);
                }
                fputs(",\"printed\":", stdout); cli_json_bytes(printed.data, printed.length);
                repair_report(p, 1);
                puts("}");
            } else printf("%" PRId32 "\n", result);
            free(input); free(output); free(printed.data);
        }
    } else if (!strcmp(o.command, "verify")) code = cli_verify(p, &o, &e);
    else if (!strcmp(o.command, "check")) {
        if (o.json) { fputs("{\"ok\":true,", stdout); stats(s, 1); repair_report(p, 1); puts("}"); }
        else stats(s, 0);
    } else if (!strcmp(o.command, "test")) code = cli_test(p, o.output, o.json);
    else if (!strcmp(o.command, "dump")) bio_program_dump(p, stdout);
    else if (!strcmp(o.command, "live")) return live(p, o.repair, &e);
    else if (!strcmp(o.command, "emit")) {
        if (!bio_program_write_code(p, o.output, &e)) code = failure(o.json, "file", e.message, 1);
        else if (o.json) { fputs("{\"ok\":true,", stdout); stats(s, 1); repair_report(p, 1); puts("}"); }
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
