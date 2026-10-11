#ifndef CODONS_CLI_H
#define CODONS_CLI_H

#include "bio.h"

void cli_json_string(const char *text);
int cli_parse_i32(const char *text, int32_t *value);
int cli_test(const BioProgram *program, const char *cases_path, int json);

#endif
