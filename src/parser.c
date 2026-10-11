#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int identifier(const char *s) {
    size_t i, len = strlen(s);
    if (!len || len >= BIO_NAME_SIZE) return 0;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') continue;
        if (i && c >= '0' && c <= '9') continue;
        return 0;
    }
    return 1;
}

static int reg(const char *s) {
    return strlen(s) == 2 && s[0] == 'r' && s[1] >= '0' && s[1] <= '7' ? s[1] - '0' : -1;
}

static int same_path(const char *a, const char *b) {
    wchar_t left[BIO_PATH_SIZE], right[BIO_PATH_SIZE];
    return bio_wide_path(a, left, BIO_PATH_SIZE) && bio_wide_path(b, right, BIO_PATH_SIZE) &&
        CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

static int symbol(BioProgram *p, const char *name) {
    unsigned i;
    for (i = 0; i < p->symbol_count; ++i)
        if (strcmp(p->symbols[i], name) == 0) return (int)i;
    if (p->symbol_count == BIO_MAX_SYMBOLS) return -1;
    strcpy(p->symbols[p->symbol_count], name);
    return (int)p->symbol_count++;
}

/* Imports resolve against the importing file, never the process working directory. */
static int resolve_path(const char *name, const char *parent, char *out, BioError *e) {
    char combined[BIO_PATH_SIZE];
    wchar_t input[BIO_PATH_SIZE], resolved[BIO_PATH_SIZE];
    DWORD length;
    int absolute = name[0] == '/' || name[0] == '\\' || (strlen(name) > 1 && name[1] == ':');
    if (parent && !absolute) {
        const char *slash = strrchr(parent, '\\');
        size_t prefix = slash ? (size_t)(slash - parent + 1) : 0;
        if (prefix + strlen(name) >= sizeof(combined)) {
            bio_error(e, "import path too long"); return 0;
        }
        memcpy(combined, parent, prefix);
        strcpy(combined + prefix, name);
        name = combined;
    }
    if (!bio_wide_path(name, input, BIO_PATH_SIZE)) {
        bio_error(e, "source path must be valid UTF-8 and fit the path limit"); return 0;
    }
    length = GetFullPathNameW(input, BIO_PATH_SIZE, resolved, NULL);
    if (!length || length >= BIO_PATH_SIZE ||
        !WideCharToMultiByte(CP_UTF8, 0, resolved, -1, out, BIO_PATH_SIZE, NULL, NULL)) {
        bio_error(e, "cannot resolve source path: %s", name); return 0;
    }
    return 1;
}

/* Counts even whitespace/comments toward the input budget, rejects embedded NUL. */
static int read_line(BioProgram *p, FILE *f, char *line, BioError *e) {
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (++p->source_bytes > BIO_MAX_SOURCE_BYTES) {
            bio_error(e, "apoptosis: source byte budget exceeded"); return -1;
        }
        if (c == 0) { bio_error(e, "NUL byte in source"); return -1; }
        if (c == '\n') break;
        if (n + 1 == BIO_LINE_SIZE) { bio_error(e, "source line too long"); return -1; }
        line[n++] = (char)c;
    }
    if (ferror(f)) { bio_error(e, "source read failed"); return -1; }
    line[n] = '\0';
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, NULL, 0)) {
        bio_error(e, "source must be valid UTF-8"); return -1;
    }
    return c == EOF && n == 0 ? 0 : 1;
}

static int parse_file(BioProgram *p, const char *name, const char *parent,
                      unsigned depth, int repair, BioError *e) {
    char path[BIO_PATH_SIZE], line[BIO_LINE_SIZE];
    FILE *f;
    unsigned index, i;
    BioGene *current = NULL;
    CodonNode location = {0};
    int status;
#define FAIL(...) do { bio_node_error(p, &location, e, __VA_ARGS__); goto fail; } while (0)
#define QUARANTINE(...) do { \
    if (repair && current && n) { \
        char repair_detail[384]; \
        snprintf(repair_detail, sizeof(repair_detail), __VA_ARGS__); \
        if (!bio_quarantine(p, n, repair_detail, e)) goto fail; \
        continue; \
    } \
    FAIL(__VA_ARGS__); \
} while (0)
    if (depth > BIO_IMPORT_DEPTH) {
        bio_error(e, "apoptosis: import depth exceeded (%u)", BIO_IMPORT_DEPTH); return 0;
    }
    if (!resolve_path(name, parent, path, e)) return 0;
    for (i = 0; i < p->file_count; ++i) {
        if (!same_path(p->files[i].path, path)) continue;
        if (p->files[i].state == 1) {
            bio_error(e, "apoptosis: cyclic import: %s", path); return 0;
        }
        return 1;
    }
    if (p->file_count == BIO_MAX_FILES) {
        bio_error(e, "apoptosis: file budget exceeded (%u)", BIO_MAX_FILES); return 0;
    }
    f = bio_fopen(path, L"rb");
    if (!f) { bio_error(e, "cannot open source: %s", path); return 0; }
    index = p->file_count++;
    strcpy(p->files[index].path, path);
    p->files[index].state = 1;
    location.file = index;
    while ((status = read_line(p, f, line, e)) > 0) {
        char *fields[4], *cursor = line, *comment;
        unsigned count = 0;
        CodonNode *n, *previous;
        int a, b;
        location.line++;
        if (location.line == 1 && strlen(line) >= 3 &&
            (unsigned char)line[0] == 0xef && (unsigned char)line[1] == 0xbb &&
            (unsigned char)line[2] == 0xbf) cursor += 3;
        comment = strchr(cursor, '#');
        if (comment) *comment = '\0';
        while (*cursor) {
            while (isspace((unsigned char)*cursor)) ++cursor;
            if (!*cursor) break;
            if (count == 4) break;
            fields[count++] = cursor;
            while (*cursor && !isspace((unsigned char)*cursor)) ++cursor;
            if (*cursor) *cursor++ = '\0';
        }
        if (!count) continue;
        if (repair && count == 2 &&
            (strcmp(fields[0], "RET") == 0 || strcmp(fields[0], "END") == 0 ||
             strcmp(fields[0], "NOT") == 0 || strcmp(fields[0], "LNOT") == 0 ||
             strcmp(fields[0], "LABEL") == 0 || strcmp(fields[0], "JMP") == 0 ||
             strcmp(fields[0], "IMPORT") == 0)) {
            fields[count++] = "_";
            if (!bio_record_repair(p, &location, "fill", "inserted an unused '_' third field", e)) goto fail;
        }
        if (count != 3) {
            if (repair && current) {
                previous = current->tail;
                n = bio_node_new(p, e);
                if (!n) goto fail;
                n->file = index; n->line = location.line;
                previous->next = n; current->tail = n;
                if (!bio_quarantine(p, n, "codon does not contain exactly three fields", e)) goto fail;
                continue;
            }
            FAIL("expected exactly 3 fields; found %u", count);
        }
        if (strcmp(fields[0], "IMPORT") == 0) {
            if (current || strcmp(fields[2], "_") != 0)
                FAIL("IMPORT requires '<path> _' outside a GENE");
            if (!parse_file(p, fields[1], path, depth + 1, repair, e)) goto fail;
            continue;
        }
        if (strcmp(fields[0], "GENE") == 0) {
            unsigned arity;
            if (current) FAIL("nested GENE; close previous gene with END _ _");
            if (!identifier(fields[1])) FAIL("invalid gene identifier");
            if (bio_find_gene(p, fields[1]) >= 0) FAIL("duplicate gene: %s", fields[1]);
            if (strcmp(fields[2], "i32") == 0 || strcmp(fields[2], "i32/0") == 0) arity = 0;
            else if (strcmp(fields[2], "i32/1") == 0) arity = 1;
            else if (strcmp(fields[2], "i32/2") == 0) arity = 2;
            else FAIL("signature must be i32, i32/0, i32/1 or i32/2");
            if (p->gene_count == BIO_MAX_GENES) FAIL("apoptosis: gene budget exceeded");
            current = &p->genes[p->gene_count++];
            strcpy(current->name, fields[1]);
            current->arity = arity;
            n = bio_node_new(p, e);
            if (!n) goto fail;
            n->op_code = OP_GENE; n->arg1 = p->gene_count - 1; n->arg2 = arity;
            n->file = index; n->line = location.line;
            current->head = current->tail = n;
            continue;
        }
        if (strcmp(fields[0], "END") == 0) {
            if (!current || strcmp(fields[1], "_") || strcmp(fields[2], "_"))
                FAIL("END requires '_ _' inside a GENE");
            if (current->head == current->tail) {
                if (!repair) FAIL("empty gene; add a body before END");
                n = bio_node_new(p, e);
                if (!n) goto fail;
                n->file = index; n->line = location.line;
                current->tail->next = n; current->tail = n;
                if (!bio_quarantine(p, n, "empty gene body", e)) goto fail;
            }
            current = NULL;
            continue;
        }
        if (!current) FAIL("instruction outside a GENE");
        if (current->tail->op_code == OP_RET && strcmp(fields[0], "LABEL"))
            FAIL("instruction after RET; start another branch with LABEL");
        previous = current->tail;
        n = bio_node_new(p, e);
        if (!n) goto fail;
        n->file = index; n->line = location.line;
        current->tail->next = n; current->tail = n;
        if (!strcmp(fields[0], "QUARANTINE")) {
            if (strcmp(fields[1], "_") || strcmp(fields[2], "_"))
                QUARANTINE("QUARANTINE requires '_ _'");
            if (!bio_quarantine(p, n, "explicit quarantine codon", e)) goto fail;
            continue;
        }
        if (!strcmp(fields[0], "VAR") || !strcmp(fields[0], "ARRAY") || !strcmp(fields[0], "PARAM")) {
            char *end;
            long long value;
            int sym, parameter = !strcmp(fields[0], "PARAM");
            int is_array = parameter || !strcmp(fields[0], "ARRAY");
            BioLocal *local;
            if (previous->op_code != OP_GENE && previous->op_code != OP_VAR &&
                previous->op_code != OP_ARRAY && previous->op_code != OP_PARAM)
                QUARANTINE("VAR, ARRAY and PARAM declarations must precede all instructions and labels");
            if (!identifier(fields[1]) || reg(fields[1]) >= 0 || !strcmp(fields[1], "_"))
                QUARANTINE("invalid local name: use an identifier other than r0..r7 or _");
            if (parameter) {
                if (strcmp(fields[2], "r0")) QUARANTINE("PARAM requires '<name> r0'; r0 is the array length");
                if (!strcmp(current->name, "main")) QUARANTINE("main cannot declare PARAM");
                if (!current->arity) QUARANTINE("PARAM requires i32/1 or i32/2; r0 supplies the array length");
                if (current->has_array) QUARANTINE("only one PARAM array is allowed per gene");
                value = 0;
            } else {
                errno = 0;
                value = strtoll(fields[2], &end, 10);
                if (errno || *end || end == fields[2] || value < INT32_MIN || value > INT32_MAX)
                    QUARANTINE("%s requires a decimal i32 literal", fields[0]);
                if (is_array && (value < 1 || value > BIO_MAX_LOCAL_CELLS))
                    QUARANTINE("ARRAY length must be in 1..%u", BIO_MAX_LOCAL_CELLS);
            }
            sym = symbol(p, fields[1]);
            if (sym < 0) QUARANTINE("symbol budget exceeded");
            for (i = 0; i < current->local_count; ++i)
                if (current->locals[i].symbol == (uint32_t)sym) QUARANTINE("duplicate local declaration");
            if (current->local_count == BIO_MAX_LOCALS) QUARANTINE("local declaration limit exceeded");
            if (current->local_cells + (is_array ? (uint32_t)value : 1u) > BIO_MAX_LOCAL_CELLS)
                QUARANTINE("local storage limit exceeded");
            local = &current->locals[current->local_count++];
            local->symbol = (uint32_t)sym; local->offset = current->local_cells;
            local->count = is_array ? (uint32_t)value : 1u;
            local->initial = is_array ? 0 : (uint32_t)value;
            local->is_array = (unsigned)is_array;
            local->is_parameter = (unsigned)parameter;
            if (parameter) current->has_array = 1;
            current->local_cells += local->count;
            n->op_code = parameter ? OP_PARAM : is_array ? OP_ARRAY : OP_VAR;
            n->arg1 = (uint32_t)sym; n->arg2 = (uint32_t)value;
            continue;
        }
        if (!strcmp(fields[0], "READ") || !strcmp(fields[0], "WRITE")) {
            int sym;
            a = reg(fields[2]);
            if (!identifier(fields[1]) || a < 0) QUARANTINE("%s requires '<array> rN' (r0..r7)", fields[0]);
            sym = symbol(p, fields[1]);
            if (sym < 0) QUARANTINE("symbol budget exceeded");
            n->op_code = !strcmp(fields[0], "READ") ? OP_READ : OP_WRITE;
            n->arg1 = (uint32_t)sym; n->arg2 = (uint32_t)a;
            continue;
        }
        if (!strcmp(fields[0], "GET") || !strcmp(fields[0], "SET") ||
            !strcmp(fields[0], "AGET") || !strcmp(fields[0], "ASET") || !strcmp(fields[0], "LEN")) {
            int sym, store = !strcmp(fields[0], "SET") || !strcmp(fields[0], "ASET");
            int array = !strcmp(fields[0], "AGET") || !strcmp(fields[0], "ASET");
            char *local_name = store ? fields[1] : fields[2];
            char *comma = array ? strchr(fields[2], ',') : NULL;
            a = b = -1;
            if (array) {
                if (!comma) QUARANTINE("%s requires a comma-separated third field", fields[0]);
                *comma++ = '\0';
                b = reg(comma);
            }
            a = reg(store ? fields[2] : fields[1]);
            if (a < 0 || (array && b < 0) || !identifier(local_name))
                QUARANTINE("invalid %s operands: use a local name and registers r0..r7", fields[0]);
            sym = symbol(p, local_name);
            if (sym < 0) QUARANTINE("symbol budget exceeded");
            if (store) {
                n->op_code = array ? OP_ASET : OP_SET;
                n->arg1 = (uint32_t)sym;
                n->arg2 = (uint32_t)a | (array ? (uint32_t)b << 8 : 0);
            } else {
                n->op_code = array ? OP_AGET : !strcmp(fields[0], "LEN") ? OP_LEN : OP_GET;
                n->arg1 = (uint32_t)a;
                n->arg2 = array ? ((uint32_t)sym << 8) | (uint32_t)b : (uint32_t)sym;
            }
            continue;
        }
        if (!strcmp(fields[0], "LABEL") || !strcmp(fields[0], "JMP")) {
            int sym;
            if (!identifier(fields[1]) || strcmp(fields[2], "_"))
                QUARANTINE("LABEL and JMP require '<name> _'");
            sym = symbol(p, fields[1]);
            if (sym < 0) QUARANTINE("symbol budget exceeded");
            n->op_code = !strcmp(fields[0], "LABEL") ? OP_LABEL : OP_JMP;
            n->arg1 = (uint32_t)sym;
            continue;
        }
        if (!strcmp(fields[0], "JZ") || !strcmp(fields[0], "JNZ")) {
            int sym;
            a = reg(fields[1]);
            if (a < 0 || !identifier(fields[2])) QUARANTINE("JZ and JNZ require 'rN <label>'");
            sym = symbol(p, fields[2]);
            if (sym < 0) QUARANTINE("symbol budget exceeded");
            n->op_code = !strcmp(fields[0], "JZ") ? OP_JZ : OP_JNZ;
            n->arg1 = (uint32_t)a; n->arg2 = (uint32_t)sym;
            continue;
        }
        if (strcmp(fields[0], "CALL") == 0) {
            char *comma, *arguments = fields[2];
            unsigned argc = 0;
            int sym;
            if (!identifier(fields[1])) QUARANTINE("invalid CALL target name");
            sym = symbol(p, fields[1]);
            if (sym < 0) QUARANTINE("symbol budget exceeded");
            a = b = 0;
            if (strcmp(arguments, "_") != 0) {
                comma = strchr(arguments, ',');
                if (comma) *comma = '\0';
                if (reg(arguments) < 0) {
                    int array_symbol;
                    if (!identifier(arguments) || !strcmp(arguments, "_") || !comma)
                        QUARANTINE("invalid CALL operands");
                    array_symbol = symbol(p, arguments);
                    if (array_symbol < 0) QUARANTINE("symbol budget exceeded");
                    n->call_array = (uint32_t)array_symbol + 1u;
                    arguments = comma + 1;
                    comma = strchr(arguments, ',');
                }
                if (comma) {
                    *comma++ = '\0'; b = reg(comma); argc = 2;
                } else argc = 1;
                a = reg(arguments);
                if (a < 0 || b < 0) QUARANTINE("invalid CALL operands");
            }
            n->op_code = OP_CALL; n->arg1 = (uint32_t)sym;
            n->arg2 = (argc << 16) | ((unsigned)b << 8) | (unsigned)a;
            continue;
        }
        a = reg(fields[1]);
        if (a < 0) QUARANTINE("first operand must be r0..r7");
        n->arg1 = (uint32_t)a;
        if (strcmp(fields[0], "LOAD") == 0) {
            char *end;
            long long value;
            errno = 0;
            value = strtoll(fields[2], &end, 10);
            if (errno || *end || end == fields[2] || value < INT32_MIN || value > INT32_MAX)
                QUARANTINE("LOAD requires a decimal i32 literal");
            n->op_code = OP_LOAD; n->arg2 = (uint32_t)value;
        } else if (strcmp(fields[0], "RET") == 0) {
            if (strcmp(fields[2], "_") != 0) QUARANTINE("RET requires an unused '_' third field");
            n->op_code = OP_RET;
        } else if (!strcmp(fields[0], "NOT") || !strcmp(fields[0], "LNOT")) {
            if (strcmp(fields[2], "_")) QUARANTINE("NOT/LNOT requires an unused '_' third field");
            n->op_code = !strcmp(fields[0], "NOT") ? OP_NOT : OP_LNOT;
        } else {
            b = reg(fields[2]);
            if (b < 0) QUARANTINE("second operand must be a register");
            n->arg2 = (uint32_t)b;
            if (strcmp(fields[0], "MOV") == 0) n->op_code = OP_MOV;
            else if (strcmp(fields[0], "ADD") == 0) n->op_code = OP_ADD;
            else if (strcmp(fields[0], "SUB") == 0) n->op_code = OP_SUB;
            else if (strcmp(fields[0], "MUL") == 0) n->op_code = OP_MUL;
            else if (strcmp(fields[0], "EQ") == 0) n->op_code = OP_EQ;
            else if (strcmp(fields[0], "LT") == 0) n->op_code = OP_LT;
            else if (strcmp(fields[0], "MIN") == 0) n->op_code = OP_MIN;
            else if (strcmp(fields[0], "MAX") == 0) n->op_code = OP_MAX;
            else if (strcmp(fields[0], "DIV") == 0) n->op_code = OP_DIV;
            else if (strcmp(fields[0], "MOD") == 0) n->op_code = OP_MOD;
            else if (strcmp(fields[0], "AND") == 0) n->op_code = OP_AND;
            else if (strcmp(fields[0], "OR") == 0) n->op_code = OP_OR;
            else if (strcmp(fields[0], "XOR") == 0) n->op_code = OP_XOR;
            else if (strcmp(fields[0], "LAND") == 0) n->op_code = OP_LAND;
            else if (strcmp(fields[0], "LOR") == 0) n->op_code = OP_LOR;
            else if (strcmp(fields[0], "LXOR") == 0) n->op_code = OP_LXOR;
            else if (strcmp(fields[0], "SHL") == 0) n->op_code = OP_SHL;
            else if (strcmp(fields[0], "SHR") == 0) n->op_code = OP_SHR;
            else if (strcmp(fields[0], "SAR") == 0) n->op_code = OP_SAR;
            else QUARANTINE("unknown opcode: %s", fields[0]);
        }
    }
    if (status < 0) {
        char detail[384];
        snprintf(detail, sizeof(detail), "%.383s", e ? e->message : "source read failed");
        location.line++;
        FAIL("%s", detail);
    }
    if (current) {
        CodonNode end_site = {0};
        CodonNode *missing;
        if (!repair) FAIL("missing END _ _ at end of file");
        end_site.file = index; end_site.line = location.line + 1u;
        if (!bio_record_repair(p, &end_site, "close", "closed a gene at end of file", e)) goto fail;
        if (current->head == current->tail) {
            missing = bio_node_new(p, e);
            if (!missing) goto fail;
            missing->file = index; missing->line = end_site.line;
            current->tail->next = missing; current->tail = missing;
            if (!bio_quarantine(p, missing, "empty gene body", e)) goto fail;
        }
        current = NULL;
    }
    fclose(f);
    p->files[index].state = 2;
    return 1;
fail:
    fclose(f);
    return 0;
#undef FAIL
#undef QUARANTINE
}

int bio_parse(BioProgram *p, const char *path, int repair, BioError *e) {
    int ok = parse_file(p, path, NULL, 0, repair, e);
    p->stats.files = p->file_count;
    p->stats.genes = p->gene_count;
    return ok;
}
