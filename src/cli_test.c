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
    int32_t arguments[2];
    int32_t expected;
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

/* Cells contain only header names or integers. Quoted cells are accepted;
   multiline fields and escaped quotes cannot represent either and are rejected. */
static int split_row(char *line, char **fields, unsigned wanted, CsvError *e) {
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
    if (count != wanted)
        return csv_error(e, "column_count", count + 1, "missing column; use commas and match the CSV header");
    return 1;
}

static int load_cases(FILE *file, unsigned arity, TestCase *cases, size_t *count, CsvError *e) {
    static const char *headers[] = {"expected", "arg1,expected", "arg1,arg2,expected"};
    char line[CSV_LINE_SIZE];
    size_t bytes = 0;
    int status, have_header = 0;
    for (;;) {
        char *fields[3], *row;
        unsigned i;
        ++e->line;
        status = read_line(file, line, &bytes, e);
        if (status < 0) return 0;
        if (!status) break;
        row = line;
        if (e->line == 1 && !strncmp(row, "\xef\xbb\xbf", 3)) row += 3;
        row = trim(row);
        if (!*row) continue;
        if (!split_row(row, fields, arity + 1, e)) {
            if (!have_header && !strcmp(e->code, "column_count"))
                bio_error(&e->detail, "main expects %u inputs; CSV header must be %s (comma-separated)", arity, headers[arity]);
            return 0;
        }
        if (!have_header) {
            for (i = 0; i <= arity; ++i) {
                const char *name = i == arity ? "expected" : i == 0 ? "arg1" : "arg2";
                if (!strcmp(fields[i], name)) continue;
                csv_error(e, "header", i + 1, "");
                bio_error(&e->detail, "main expects %u inputs; CSV header must be %s (comma-separated)", arity, headers[arity]);
                return 0;
            }
            have_header = 1;
            continue;
        }
        if (*count == CASE_LIMIT)
            return csv_error(e, "case_limit", 0, "CSV exceeds the 1000-example limit; split it into smaller files");
        cases[*count].line = e->line;
        for (i = 0; i <= arity; ++i) {
            int32_t *value = i == arity ? &cases[*count].expected : &cases[*count].arguments[i];
            if (cli_parse_i32(fields[i], value)) continue;
            csv_error(e, "integer", i + 1, "");
            bio_error(&e->detail, "%s must be a decimal i32 (-2147483648..2147483647); empty cells are not allowed",
                      i == arity ? "expected" : i == 0 ? "arg1" : "arg2");
            return 0;
        }
        ++*count;
    }
    e->line = 0;
    if (!have_header) {
        csv_error(e, "header", 0, "");
        bio_error(&e->detail, "missing CSV header; expected %s", headers[arity]);
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

int cli_test(const BioProgram *program, const char *cases_path, int json) {
    BioStats stats = bio_program_stats(program);
    CsvError csv = {0};
    TestCase *cases;
    FILE *file;
    size_t count = 0, i, passed = 0, mismatches = 0, errors = 0;
    int loaded;
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
    loaded = load_cases(file, stats.arity, cases, &count, &csv);
    if (fclose(file) && loaded) loaded = csv_error(&csv, "read", 0, "cannot finish reading CSV file");
    if (!loaded) {
        free(cases); return report_csv_error(cases_path, &csv, json);
    }
    if (json) {
        fputs("{\"file\":", stdout); cli_json_string(cases_path); fputs(",\"cases\":[", stdout);
    }
    for (i = 0; i < count; ++i) {
        BioError error = {{0}};
        int32_t actual = 0;
        int ran = bio_program_run(program, cases[i].arguments, stats.arity, &actual, &error);
        int matched = ran && actual == cases[i].expected;
        if (!ran) ++errors;
        else if (matched) ++passed;
        else ++mismatches;
        if (json) {
            if (i) putchar(',');
            printf("{\"line\":%zu,\"inputs\":", cases[i].line); print_inputs(&cases[i], stats.arity);
            printf(",\"expected\":%" PRId32 ",\"status\":\"%s\"", cases[i].expected,
                   !ran ? "error" : matched ? "passed" : "failed");
            if (ran) printf(",\"actual\":%" PRId32, actual);
            else {
                fputs(",\"error\":{\"stage\":\"run\",\"message\":", stdout);
                cli_json_string(error.message); putchar('}');
            }
            putchar('}');
        } else {
            printf("%s %s:%zu: inputs=", !ran ? "ERROR" : matched ? "PASS" : "FAIL", cases_path, cases[i].line);
            print_inputs(&cases[i], stats.arity);
            printf(" expected=%" PRId32, cases[i].expected);
            if (ran) printf(" actual=%" PRId32 "\n", actual);
            else printf("; %s\n", error.message);
        }
    }
    if (json)
        printf("],\"ok\":%s,\"summary\":{\"total\":%zu,\"passed\":%zu,\"failed\":%zu,"
               "\"mismatches\":%zu,\"errors\":%zu}}\n", passed == count ? "true" : "false",
               count, passed, mismatches + errors, mismatches, errors);
    else
        printf("Tests: %zu total, %zu passed, %zu failed (%zu mismatches, %zu execution errors).\n",
               count, passed, mismatches + errors, mismatches, errors);
    free(cases);
    return passed == count ? 0 : 1;
}
