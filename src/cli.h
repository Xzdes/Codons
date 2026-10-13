#ifndef CODONS_CLI_H
#define CODONS_CLI_H

#include "bio.h"

void cli_json_string(const char *text);
/* JSON string literal for arbitrary bytes: escapes '"', '\\' and bytes < 32;
   other bytes pass through unchanged, so printed text should be UTF-8. */
void cli_json_bytes(const unsigned char *bytes, size_t length);
int cli_parse_i32(const char *text, int32_t *value);
int cli_test(const BioProgram *program, const char *cases_path, int json);
int cli_read_list(const char *path, int32_t **values, size_t *count, BioError *error);
int cli_write_list(const char *path, const int32_t *values, size_t count, BioError *error);

/* PRINT channels for CLI hosts: stream to stdout, drop (test harness),
   or accumulate for --json and verify reports. */
typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} CliPrint;
int cli_print_stdout(void *user, const unsigned char *bytes, size_t length);
int cli_print_discard(void *user, const unsigned char *bytes, size_t length);
int cli_print_capture(void *user, const unsigned char *bytes, size_t length);

#endif
