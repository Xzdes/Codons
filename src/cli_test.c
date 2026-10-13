#include "internal.h"
#include "cli.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define CASE_LIMIT 1000u
#define CSV_LINE_SIZE 512u
#define CSV_BYTE_LIMIT (1024u * 1024u)

typedef struct {
    size_t line;
    int32_t arguments[4];
    int32_t expected;
    char input[CSV_LINE_SIZE];
    char expected_output[CSV_LINE_SIZE];
} TestCase;

typedef struct {
    const char *code;
    size_t line;
    unsigned column;
    BioError detail;
} CsvError;

static int csv_error(CsvError *e, const char *code, unsigned column, const char *message) {
    e->code = code;
    e->column = column;
    bio_error(&e->detail, "%s", message);
    return 0;
}

/* Bounded binary input preserves physical line numbers, BOM and UTF-8 checks. */
static int read_line(FILE *file, char *line, size_t *bytes, CsvError *e) {
    size_t length = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (++*bytes > CSV_BYTE_LIMIT) {
            csv_error(e, "file_limit", 0, "CSV exceeds the 1 MiB file limit"); return -1;
        }
        if (!ch) {
            csv_error(e, "encoding", 0, "CSV must be UTF-8 without NUL bytes"); return -1;
        }
        if (ch == '\n') break;
        if (length == CSV_LINE_SIZE - 1) {
            csv_error(e, "line_limit", 0, "CSV line exceeds 511 bytes"); return -1;
        }
        line[length++] = (char)ch;
    }
    if (ferror(file)) { csv_error(e, "read", 0, "cannot read CSV file"); return -1; }
    if (ch == EOF && !length) return 0;
    line[length] = '\0';
    if (length && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, (int)length, NULL, 0)) {
        csv_error(e, "encoding", 0, "CSV must contain valid UTF-8"); return -1;
    }
    if (length && line[length - 1] == '\r') line[length - 1] = '\0';
    return 1;
}

static char *trim(char *text) {
    char *end;
    while (*text == ' ' || *text == '\t') ++text;
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = '\0';
    return text;
}

/* Quoted paths may contain commas. Multiline fields and embedded quotes
   are not supported (quotes are not valid in ordinary Windows filenames). */
static int split_row(char *line, char **fields, unsigned wanted, unsigned *found, CsvError *e) {
    char *cursor = line;
    unsigned count = 0;
    for (;;) {
        char *field;
        int more;
        if (count == wanted)
            return csv_error(e, "column_count", count + 1, "too many columns; use commas and match the CSV header");
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '"') {
            field = ++cursor;
            while (*cursor && *cursor != '"') ++cursor;
            if (!*cursor)
                return csv_error(e, "quotes", count + 1, "unclosed quoted cell; each example must occupy one line");
            *cursor++ = '\0';
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            if (*cursor && *cursor != ',')
                return csv_error(e, "quotes", count + 1, "expected a comma or end of line after closing quote");
        } else {
            field = cursor;
            while (*cursor && *cursor != ',') {
                if (*cursor == '"')
                    return csv_error(e, "quotes", count + 1, "a quoted cell must start with a double quote");
                ++cursor;
            }
        }
        more = *cursor == ',';
        if (more) *cursor++ = '\0';
        fields[count++] = trim(field);
        if (!more) break;
    }
    *found = count;
    return 1;
}

static const char *arg_names[] = {"arg1", "arg2", "arg3", "arg4"};
static const char *scalar_headers[] = {
    "expected", "arg1,expected", "arg1,arg2,expected",
    "arg1,arg2,arg3,expected", "arg1,arg2,arg3,arg4,expected"
};
static const char *file_headers[] = {
    "input,expected,expected_output", "arg1,input,expected,expected_output",
    "arg1,arg2,input,expected,expected_output", "arg1,arg2,arg3,input,expected,expected_output",
    "arg1,arg2,arg3,arg4,input,expected,expected_output"
};

static int bad_header(unsigned arity, CsvError *e) {
    bio_error(&e->detail, "main expects %u inputs; CSV header must be %s or %s (comma-separated)",
              arity, scalar_headers[arity], file_headers[arity]);
    return 0;
}

static int load_cases(FILE *file, unsigned arity, TestCase *cases, size_t *count,
                      int *file_mode, CsvError *e) {
    char line[CSV_LINE_SIZE];
    size_t bytes = 0;
    int status, have_header = 0;
    unsigned columns = arity + 1;
    for (;;) {
        char *fields[8], *row;
        unsigned i, found = 0;
        ++e->line;
        status = read_line(file, line, &bytes, e);
        if (status < 0) return 0;
        if (!status) break;
        row = line;
        if (e->line == 1 && !strncmp(row, "\xef\xbb\xbf", 3)) row += 3;
        row = trim(row);
        if (!*row) continue;
        if (!split_row(row, fields, have_header ? columns : arity + 3, &found, e)) {
            if (!have_header && !strcmp(e->code, "column_count")) return bad_header(arity, e);
            return 0;
        }
        if (!have_header) {
            *file_mode = found == arity + 3;
            columns = arity + (*file_mode ? 3u : 1u);
            if (found != columns) {
                csv_error(e, "column_count", found < columns ? found + 1 : columns + 1, "");
                return bad_header(arity, e);
            }
            for (i = 0; i < columns; ++i) {
                const char *name = i < arity ? arg_names[i] :
                    *file_mode ? (i == arity ? "input" : i == arity + 1 ? "expected" : "expected_output") : "expected";
                if (!strcmp(fields[i], name)) continue;
                csv_error(e, "header", i + 1, "");
                return bad_header(arity, e);
            }
            have_header = 1;
            continue;
        }
        if (found != columns)
            return csv_error(e, "column_count", found + 1, "missing column; use commas and match the CSV header");
        if (*count == CASE_LIMIT)
            return csv_error(e, "case_limit", 0, "CSV exceeds the 1000-example limit; split it into smaller files");
        cases[*count].line = e->line;
        for (i = 0; i < columns; ++i) {
            if (*file_mode && (i == arity || i == arity + 2)) {
                if (!*fields[i]) return csv_error(e, "path", i + 1,
                    "list path must not be empty; use an empty file to represent an empty list");
                /* Each cell fits in the bounded source line, including its NUL. */
                strcpy(i == arity ? cases[*count].input : cases[*count].expected_output, fields[i]);
            } else {
                int32_t *value = i < arity ? &cases[*count].arguments[i] : &cases[*count].expected;
                if (cli_parse_i32(fields[i], value)) continue;
                csv_error(e, "integer", i + 1, "");
                bio_error(&e->detail, "%s must be a decimal i32 (-2147483648..2147483647); empty cells are not allowed",
                          i < arity ? arg_names[i] : "expected");
                return 0;
            }
        }
        ++*count;
    }
    e->line = 0;
    if (!have_header) {
        csv_error(e, "header", 0, "");
        bio_error(&e->detail, "missing CSV header; expected %s or %s", scalar_headers[arity], file_headers[arity]);
        return 0;
    }
    if (!*count) return csv_error(e, "empty", 0, "CSV contains no examples; add at least one row after the header");
    return 1;
}

static int report_csv_error(const char *path, const CsvError *e, int json) {
    if (json) {
        fputs("{\"ok\":false,\"error\":{\"stage\":\"csv\",\"code\":", stdout); cli_json_string(e->code);
        fputs(",\"file\":", stdout); cli_json_string(path);
        printf(",\"line\":%zu,\"column\":%u,\"message\":", e->line, e->column);
        cli_json_string(e->detail.message); puts("}}");
    } else {
        fprintf(stderr, "%s", path);
        if (e->line) fprintf(stderr, ":%zu", e->line);
        if (e->column) fprintf(stderr, ": column %u", e->column);
        fprintf(stderr, ": %s\n", e->detail.message);
    }
    return 1;
}

static void print_inputs(const TestCase *test, unsigned arity) {
    unsigned i;
    putchar('[');
    for (i = 0; i < arity; ++i) {
        if (i) putchar(',');
        printf("%" PRId32, test->arguments[i]);
    }
    putchar(']');
}

typedef struct {
    int ran;
    int32_t actual;
    BioError error;
    const char *error_stage;
    const char *error_field;
    char error_file[BIO_PATH_SIZE];
    int output_matched;
    size_t expected_count, actual_count, difference; /* difference is 1-based; zero means equal. */
    int expected_present, actual_present;
    int32_t expected_value, actual_value;
} CaseResult;

/* Resolve list paths against the CSV directory, independently of source imports and cwd. */
static int fixture_path(const char *name, const char *csv_path, char *out, BioError *error) {
    wchar_t wide[BIO_PATH_SIZE], full[BIO_PATH_SIZE];
    char parent[BIO_PATH_SIZE], combined[BIO_PATH_SIZE];
    const char *slash;
    DWORD length;
    size_t prefix;
    int absolute = name[0] == '/' || name[0] == '\\' || (strlen(name) > 1 && name[1] == ':');
    if (!absolute) {
        if (!bio_wide_path(csv_path, wide, BIO_PATH_SIZE)) goto bad;
        length = GetFullPathNameW(wide, BIO_PATH_SIZE, full, NULL);
        if (!length || length >= BIO_PATH_SIZE ||
            !WideCharToMultiByte(CP_UTF8, 0, full, -1, parent, BIO_PATH_SIZE, NULL, NULL)) goto bad;
        slash = strrchr(parent, '\\');
        prefix = slash ? (size_t)(slash - parent + 1) : 0;
        if (prefix + strlen(name) >= sizeof(combined)) goto bad;
        memcpy(combined, parent, prefix);
        strcpy(combined + prefix, name);
        name = combined;
    }
    if (!bio_wide_path(name, wide, BIO_PATH_SIZE)) goto bad;
    length = GetFullPathNameW(wide, BIO_PATH_SIZE, full, NULL);
    if (!length || length >= BIO_PATH_SIZE ||
        !WideCharToMultiByte(CP_UTF8, 0, full, -1, out, BIO_PATH_SIZE, NULL, NULL)) goto bad;
    return 1;
bad:
    bio_error(error, "cannot resolve list path relative to CSV (UTF-8 path limit: %u bytes): %s",
              BIO_PATH_SIZE - 1u, name);
    return 0;
}

static int read_fixture(const char *name, const char *field, const char *csv_path,
                        int32_t **values, size_t *count, CaseResult *result) {
    result->error_stage = "file";
    result->error_field = field;
    strcpy(result->error_file, name); /* name is limited to one CSV cell. */
    return fixture_path(name, csv_path, result->error_file, &result->error) &&
        cli_read_list(result->error_file, values, count, &result->error);
}

static void run_file_case(const BioProgram *program, const TestCase *test, unsigned arity,
                          const char *csv_path, CaseResult *result) {
    int32_t *input = NULL, *expected = NULL, *output = NULL;
    BioIO io = {0};
    size_t i, common;
    if (!read_fixture(test->input, "input", csv_path, &input, &io.input_count, result)) goto done;
    if (!read_fixture(test->expected_output, "expected_output", csv_path,
                       &expected, &result->expected_count, result)) goto done;
    result->error_stage = "run"; result->error_field = NULL;
    output = malloc(BIO_LIST_LIMIT * sizeof(*output));
    if (!output) { bio_error(&result->error, "out of memory comparing output list"); goto done; }
    io.input = input; io.output = output; io.output_capacity = BIO_LIST_LIMIT;
    /* The test harness grants printing but discards it: expected output is data. */
    io.print = cli_print_discard;
    if (!bio_program_run_io(program, test->arguments, arity, &io, &result->actual, &result->error)) goto done;
    if (!io.output_written) {
        bio_error(&result->error, "program did not execute WRITE; an empty result needs WRITE with count 0");
        goto done;
    }
    result->ran = 1; result->actual_count = io.output_count;
    common = io.output_count < result->expected_count ? io.output_count : result->expected_count;
    for (i = 0; i < common && output[i] == expected[i]; ++i) {}
    result->output_matched = i == common && io.output_count == result->expected_count;
    if (!result->output_matched) {
        result->difference = i + 1;
        result->expected_present = i < result->expected_count;
        result->actual_present = i < io.output_count;
        if (result->expected_present) result->expected_value = expected[i];
        if (result->actual_present) result->actual_value = output[i];
    }
done:
    free(input); free(expected); free(output);
}

static void print_output_comparison(const CaseResult *r, int json) {
    if (json) {
        printf(",\"output\":{\"matched\":%s,\"expected_count\":%zu,\"actual_count\":%zu",
               r->output_matched ? "true" : "false", r->expected_count, r->actual_count);
        if (r->difference) {
            printf(",\"difference\":{\"element\":%zu,\"expected\":", r->difference);
            if (r->expected_present) printf("%" PRId32, r->expected_value); else fputs("null", stdout);
            fputs(",\"actual\":", stdout);
            if (r->actual_present) printf("%" PRId32, r->actual_value); else fputs("null", stdout);
            putchar('}');
        }
        putchar('}');
    } else {
        printf("; output_count: expected=%zu actual=%zu", r->expected_count, r->actual_count);
        if (r->difference) {
            printf("; element %zu: expected=", r->difference);
            if (r->expected_present) printf("%" PRId32, r->expected_value); else fputs("<missing>", stdout);
            fputs(" actual=", stdout);
            if (r->actual_present) printf("%" PRId32, r->actual_value); else fputs("<missing>", stdout);
        }
    }
}

int cli_test(const BioProgram *program, const char *cases_path, int json) {
    BioStats stats = bio_program_stats(program);
    CsvError csv = {0};
    TestCase *cases;
    FILE *file;
    size_t count = 0, i, passed = 0, mismatches = 0, errors = 0;
    int loaded, file_mode = 0;
    file = bio_fopen(cases_path, L"rb");
    if (!file) {
        csv_error(&csv, "open", 0, "cannot open CSV file"); return report_csv_error(cases_path, &csv, json);
    }
    cases = calloc(CASE_LIMIT, sizeof(*cases));
    if (!cases) {
        fclose(file);
        csv_error(&csv, "memory", 0, "out of memory loading CSV examples");
        return report_csv_error(cases_path, &csv, json);
    }
    loaded = load_cases(file, stats.arity, cases, &count, &file_mode, &csv);
    if (fclose(file) && loaded) loaded = csv_error(&csv, "read", 0, "cannot finish reading CSV file");
    if (!loaded) {
        free(cases); return report_csv_error(cases_path, &csv, json);
    }
    if (json) {
        fputs("{\"file\":", stdout); cli_json_string(cases_path); fputs(",\"cases\":[", stdout);
    }
    for (i = 0; i < count; ++i) {
        CaseResult result = {0};
        int matched;
        result.error_stage = "run";
        if (file_mode) run_file_case(program, &cases[i], stats.arity, cases_path, &result);
        else result.ran = bio_program_run(program, cases[i].arguments, stats.arity, &result.actual, &result.error);
        matched = result.ran && result.actual == cases[i].expected && (!file_mode || result.output_matched);
        if (!result.ran) ++errors;
        else if (matched) ++passed;
        else ++mismatches;
        if (json) {
            if (i) putchar(',');
            printf("{\"line\":%zu,\"inputs\":", cases[i].line); print_inputs(&cases[i], stats.arity);
            printf(",\"expected\":%" PRId32 ",\"status\":\"%s\"", cases[i].expected,
                   !result.ran ? "error" : matched ? "passed" : "failed");
            if (file_mode) {
                fputs(",\"input\":", stdout); cli_json_string(cases[i].input);
                fputs(",\"expected_output\":", stdout); cli_json_string(cases[i].expected_output);
            }
            if (result.ran) {
                printf(",\"actual\":%" PRId32, result.actual);
                if (file_mode) print_output_comparison(&result, 1);
            } else {
                fputs(",\"error\":{\"stage\":", stdout); cli_json_string(result.error_stage);
                if (result.error_field) {
                    fputs(",\"field\":", stdout); cli_json_string(result.error_field);
                    fputs(",\"file\":", stdout); cli_json_string(result.error_file);
                }
                fputs(",\"message\":", stdout); cli_json_string(result.error.message); putchar('}');
            }
            putchar('}');
        } else {
            printf("%s %s:%zu: inputs=", !result.ran ? "ERROR" : matched ? "PASS" : "FAIL", cases_path, cases[i].line);
            print_inputs(&cases[i], stats.arity);
            printf(" expected=%" PRId32, cases[i].expected);
            if (result.ran) {
                printf(" actual=%" PRId32, result.actual);
                if (file_mode) print_output_comparison(&result, 0);
            } else {
                if (result.error_field) printf("; %s", result.error_field);
                printf("; %s", result.error.message);
            }
            putchar('\n');
        }
    }
    if (json)
        printf("],\"ok\":%s,\"summary\":{\"total\":%zu,\"passed\":%zu,\"failed\":%zu,"
               "\"mismatches\":%zu,\"errors\":%zu}}\n", passed == count ? "true" : "false",
               count, passed, mismatches + errors, mismatches, errors);
    else
        printf("Tests: %zu total, %zu passed, %zu failed (%zu mismatches, %zu %s).\n",
               count, passed, mismatches + errors, mismatches, errors,
               file_mode ? "file or execution errors" : "execution errors");
    free(cases);
    return passed == count ? 0 : 1;
}
