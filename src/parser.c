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
    length = GetFullPathNameA(name, BIO_PATH_SIZE, out, NULL);
    if (!length || length >= BIO_PATH_SIZE) {
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
    if (depth > BIO_IMPORT_DEPTH) {
        bio_error(e, "apoptosis: import depth exceeded (%u)", BIO_IMPORT_DEPTH); return 0;
    }
    if (!resolve_path(name, parent, path, e)) return 0;
    for (i = 0; i < p->file_count; ++i) {
        if (_stricmp(p->files[i].path, path) != 0) continue;
        if (p->files[i].state == 1) {
            bio_error(e, "apoptosis: cyclic import: %s", path); return 0;
        }
        return 1;
    }
    if (p->file_count == BIO_MAX_FILES) {
        bio_error(e, "apoptosis: file budget exceeded (%u)", BIO_MAX_FILES); return 0;
    }
    f = fopen(path, "rb");
    if (!f) { bio_error(e, "cannot open source: %s", path); return 0; }
    index = p->file_count++;
    strcpy(p->files[index].path, path);
    p->files[index].state = 1;
    location.file = index;
    while ((status = read_line(p, f, line, e)) > 0) {
        char *fields[4], *cursor = line, *comment;
        unsigned count = 0;
        CodonNode *n;
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
            (strcmp(fields[0], "RET") == 0 || strcmp(fields[0], "END") == 0)) {
            fields[count++] = "_";
            p->stats.repairs++;
        }
        if (count != 3) FAIL("expected exactly 3 fields; found %u", count);
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
            if (current->tail->op_code != OP_RET) FAIL("gene must end with RET before END");
            current = NULL;
            continue;
        }
        if (!current) FAIL("instruction outside a GENE");
        if (current->tail->op_code == OP_RET) FAIL("instruction after RET");
        n = bio_node_new(p, e);
        if (!n) goto fail;
        n->file = index; n->line = location.line;
        current->tail->next = n; current->tail = n;
        if (strcmp(fields[0], "CALL") == 0) {
            char *comma;
            unsigned argc = 0;
            int sym;
            if (!identifier(fields[1])) FAIL("invalid callee identifier");
            sym = symbol(p, fields[1]);
            if (sym < 0) FAIL("apoptosis: symbol budget exceeded");
            a = b = 0;
            if (strcmp(fields[2], "_") != 0) {
                comma = strchr(fields[2], ',');
                if (comma) {
                    *comma++ = '\0'; b = reg(comma); argc = 2;
                } else argc = 1;
                a = reg(fields[2]);
                if (a < 0 || b < 0) FAIL("CALL operands must be _, rN or rN,rM (r0..r7)");
            }
            n->op_code = OP_CALL; n->arg1 = (uint32_t)sym;
            n->arg2 = (argc << 16) | ((unsigned)b << 8) | (unsigned)a;
            continue;
        }
        a = reg(fields[1]);
        if (a < 0) FAIL("first operand must be r0..r7");
        n->arg1 = (uint32_t)a;
        if (strcmp(fields[0], "LOAD") == 0) {
            char *end;
            long long value;
            errno = 0;
            value = strtoll(fields[2], &end, 10);
            if (errno || *end || end == fields[2] || value < INT32_MIN || value > INT32_MAX)
                FAIL("LOAD requires a decimal i32 literal");
            n->op_code = OP_LOAD; n->arg2 = (uint32_t)value;
        } else if (strcmp(fields[0], "RET") == 0) {
            if (strcmp(fields[2], "_") != 0) FAIL("RET requires an unused '_' third field");
            n->op_code = OP_RET;
        } else {
            b = reg(fields[2]);
            if (b < 0) FAIL("second operand must be r0..r7");
            n->arg2 = (uint32_t)b;
            if (strcmp(fields[0], "MOV") == 0) n->op_code = OP_MOV;
            else if (strcmp(fields[0], "ADD") == 0) n->op_code = OP_ADD;
            else if (strcmp(fields[0], "SUB") == 0) n->op_code = OP_SUB;
            else if (strcmp(fields[0], "MUL") == 0) n->op_code = OP_MUL;
            else FAIL("unknown opcode: %s", fields[0]);
        }
    }
    if (status < 0) {
        char detail[384];
        snprintf(detail, sizeof(detail), "%.383s", e ? e->message : "source read failed");
        location.line++;
        FAIL("%s", detail);
    }
    if (current) FAIL("missing END _ _ at end of file");
    fclose(f);
    p->files[index].state = 2;
    return 1;
fail:
    fclose(f);
    return 0;
#undef FAIL
}

int bio_parse(BioProgram *p, const char *path, int repair, BioError *e) {
    int ok = parse_file(p, path, NULL, 0, repair, e);
    p->stats.files = p->file_count;
    p->stats.genes = p->gene_count;
    return ok;
}
