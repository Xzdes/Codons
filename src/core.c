#include "internal.h"
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void bio_error(BioError *e, const char *format, ...) {
    va_list ap;
    int length;
    if (!e) return;
    va_start(ap, format);
    length = vsnprintf(e->message, sizeof(e->message), format, ap);
    va_end(ap);
    if (length >= (int)sizeof(e->message)) {
        size_t end = strlen(e->message), start = end - 1;
        unsigned char lead;
        size_t needed;
        while (start && ((unsigned char)e->message[start] & 0xc0u) == 0x80u) --start;
        lead = (unsigned char)e->message[start];
        needed = lead < 0x80u ? 1u : lead < 0xe0u ? 2u : lead < 0xf0u ? 3u : 4u;
        if (end - start < needed) e->message[start] = '\0';
    }
}

void bio_node_error(const BioProgram *p, const CodonNode *n, BioError *e, const char *format, ...) {
    char detail[384];
    va_list ap;
    va_start(ap, format);
    vsnprintf(detail, sizeof(detail), format, ap);
    va_end(ap);
    bio_error(e, "%s:%u: %s", p->files[n->file].path, n->line, detail);
}

int bio_record_repair(BioProgram *p, const CodonNode *n, const char *kind, const char *detail, BioError *e) {
    BioRepairEntry *record;
    if (p->repair_count == BIO_MAX_REPAIRS) {
        bio_node_error(p, n, e, "apoptosis: repair log exceeded %u entries", BIO_MAX_REPAIRS);
        return 0;
    }
    record = &p->repair_log[p->repair_count++];
    record->file = n->file;
    record->line = n->line;
    snprintf(record->kind, sizeof(record->kind), "%s", kind);
    snprintf(record->detail, sizeof(record->detail), "%s", detail);
    ++p->stats.repairs;
    if (!strcmp(kind, "quarantine")) ++p->stats.quarantines;
    return 1;
}

int bio_quarantine(BioProgram *p, CodonNode *n, const char *detail, BioError *e) {
    if (!bio_record_repair(p, n, "quarantine", detail, e)) return 0;
    n->op_code = OP_QUARANTINE;
    n->arg1 = n->arg2 = n->call_array = 0;
    n->bond = NULL;
    return 1;
}

CodonNode *bio_node_new(BioProgram *p, BioError *e) {
    CodonNode *n;
    if (p->stats.nodes == BIO_MAX_NODES) {
        bio_error(e, "apoptosis: node budget exceeded (%u)", BIO_MAX_NODES);
        return NULL;
    }
    if (!p->arena || p->arena->used == BIO_NODES_PER_BLOCK) {
        ArenaBlock *block = calloc(1, sizeof(*block));
        if (!block) { bio_error(e, "out of memory allocating arena"); return NULL; }
        block->next = p->arena;
        p->arena = block;
    }
    n = &p->arena->nodes[p->arena->used++];
    n->index = (uint32_t)p->stats.nodes++;
    return n;
}

void bio_program_retain(BioProgram *p) { InterlockedIncrement(&p->references); }

void bio_program_release(BioProgram *p) {
    if (!p || InterlockedDecrement(&p->references) != 0) return;
    if (p->unwind_registered) RtlDeleteFunctionTable(p->unwind_table);
    if (p->code) VirtualFree(p->code, 0, MEM_RELEASE);
    free(p->unwind_table);
    free(p->literals);
    free(p->literal_pool);
    while (p->arena) {
        ArenaBlock *next = p->arena->next;
        free(p->arena);
        p->arena = next;
    }
    free(p);
}

int bio_wide_path(const char *path, wchar_t *out, size_t capacity) {
    return path && capacity <= INT_MAX &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, (int)capacity) > 0;
}

FILE *bio_fopen(const char *path, const wchar_t *mode) {
    wchar_t wide[BIO_PATH_SIZE];
    if (!bio_wide_path(path, wide, BIO_PATH_SIZE)) return NULL;
    return _wfopen(wide, mode);
}

BioProgram *bio_load(const char *path, int repair, BioError *e) {
    BioProgram *p = calloc(1, sizeof(*p));
    if (e) e->message[0] = '\0';
    if (!p) { bio_error(e, "out of memory allocating program"); return NULL; }
    p->references = 1;
    p->repair_mode = repair ? 1u : 0u;
    if (!path || !bio_parse(p, path, repair, e) || !bio_analyze(p, e)) {
        if (!path) bio_error(e, "source path is required");
        bio_program_release(p);
        return NULL;
    }
    return p;
}

BioProgram *bio_compile(const char *path, int repair, BioError *e) {
    BioProgram *p = bio_load(path, repair, e);
    if (p && !bio_jit(p, e)) { bio_program_release(p); return NULL; }
    return p;
}

BioStats bio_program_stats(const BioProgram *p) { return p->stats; }

size_t bio_program_repair_count(const BioProgram *p) { return p ? p->repair_count : 0; }

int bio_program_repair_at(const BioProgram *p, size_t index, BioRepair *repair) {
    const BioRepairEntry *entry;
    if (!p || !repair || index >= p->repair_count) return 0;
    entry = &p->repair_log[index];
    repair->file = p->files[entry->file].path;
    repair->line = entry->line;
    repair->kind = entry->kind;
    repair->detail = entry->detail;
    return 1;
}

int bio_find_gene(const BioProgram *p, const char *name) {
    unsigned i;
    for (i = 0; i < p->gene_count; ++i)
        if (strcmp(p->genes[i].name, name) == 0) return (int)i;
    return -1;
}

static int validate_arguments(const BioProgram *p, const int32_t *args, size_t argc,
                              int32_t *result, BioError *e) {
    if (e) e->message[0] = '\0';
    if (!p || !result || (argc && !args)) {
        bio_error(e, "program, result and supplied arguments are required"); return 0;
    }
    if (argc != p->stats.arity) {
        bio_error(e, "main expects %u arguments, got %zu", p->stats.arity, argc); return 0;
    }
    return 1;
}

static int execution_ok(const BioProgram *p, const BioExecution *execution, BioError *e) {
    const ArenaBlock *block;
    const char *message;
    char detail[160];
    if (execution->failed_node == UINT32_MAX) return 1;
    switch (execution->fault) {
    case BIO_FAULT_BUDGET:
        snprintf(detail, sizeof(detail), "execution budget exhausted (limit: %u steps)", BIO_EXECUTION_LIMIT);
        message = detail; break;
    case BIO_FAULT_DIV_ZERO: message = "division by zero (DIV/MOD require a nonzero divisor)"; break;
    case BIO_FAULT_DIV_OVERFLOW: message = "division overflow: -2147483648 / -1 does not fit i32"; break;
    case BIO_FAULT_SHIFT: message = "shift count must be in 0..31"; break;
    case BIO_FAULT_BOUNDS:
        if (execution->bound)
            snprintf(detail, sizeof(detail), "array index %ld out of bounds: valid indices are 0..%u (length %u)",
                     (long)execution->bad_index, execution->bound - 1u, execution->bound);
        else snprintf(detail, sizeof(detail), "array index %ld out of bounds: array is empty (length 0)",
                      (long)execution->bad_index);
        message = detail; break;
    case BIO_FAULT_INPUT: message = "READ requires an input list; use --input <file> or BioIO.input"; break;
    case BIO_FAULT_OUTPUT: message = "WRITE requires an output list; use --output <file> or BioIO.output"; break;
    case BIO_FAULT_READ_SIZE:
        snprintf(detail, sizeof(detail), "READ input has %ld numbers; array capacity is %u (no data was truncated)",
                 (long)execution->bad_index, execution->bound);
        message = detail; break;
    case BIO_FAULT_WRITE_SIZE:
        snprintf(detail, sizeof(detail), "WRITE count %ld out of bounds: expected 0..%u",
                 (long)execution->bad_index, execution->bound);
        message = detail; break;
    case BIO_FAULT_OUTPUT_SIZE:
        snprintf(detail, sizeof(detail), "WRITE count %ld exceeds output capacity %u",
                 (long)execution->bad_index, execution->bound);
        message = detail; break;
    case BIO_FAULT_ARGUMENT_SIZE:
        snprintf(detail, sizeof(detail), "CALL array length %ld out of bounds: expected 0..%u",
                 (long)execution->bad_index, execution->bound);
        message = detail; break;
    case BIO_FAULT_STRING_HANDLE:
        snprintf(detail, sizeof(detail), "invalid string handle %ld (%u handles exist in this invocation)",
                 (long)execution->bad_index, execution->bound);
        message = detail; break;
    case BIO_FAULT_STRING_BOUNDS:
        if (execution->bound)
            snprintf(detail, sizeof(detail), "string index %ld out of bounds: valid indices are 0..%u (length %u)",
                     (long)execution->bad_index, execution->bound - 1u, execution->bound);
        else snprintf(detail, sizeof(detail), "string index %ld out of bounds: the string is empty (length 0)",
                      (long)execution->bad_index);
        message = detail; break;
    case BIO_FAULT_STRING_BUDGET:
        snprintf(detail, sizeof(detail), "string memory exhausted: need %ld more bytes, %u left in the invocation pool",
                 (long)execution->bad_index, execution->bound);
        message = detail; break;
    case BIO_FAULT_STRING_TABLE:
        snprintf(detail, sizeof(detail), "string handle table full (%u entries per invocation)",
                 execution->bound);
        message = detail; break;
    case BIO_FAULT_PRINT_CAPABILITY:
        message = "PRINT requires a host print capability"; break;
    case BIO_FAULT_PRINT_FAILED:
        message = "the host print callback failed"; break;
    case BIO_FAULT_QUARANTINE:
        message = "quarantined repaired codon reached; inspect the repair report"; break;
    default: message = "unknown execution error"; break;
    }
    for (block = p->arena; block; block = block->next) {
        uint32_t first = block->nodes[0].index;
        if (execution->failed_node >= first && execution->failed_node - first < block->used) {
            bio_node_error(p, &block->nodes[execution->failed_node - first], e, "%s", message);
            return 0;
        }
    }
    bio_error(e, "%s", message);
    return 0;
}

static int32_t signed_value(uint32_t bits) {
    int32_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t interpreter_fault(BioExecution *execution, const CodonNode *n, BioFault fault) {
    execution->failed_node = n->index;
    execution->fault = (uint32_t)fault;
    return 0;
}

/* Both backends call this one helper, so charging and faults cannot diverge.
   The whole byte count is checked against the budget before anything is delivered. */
int bio_print_helper(BioExecution *execution, uint32_t handle, uint32_t node) {
    const BioString *s;
    if (!execution->print) {
        execution->failed_node = node; execution->fault = BIO_FAULT_PRINT_CAPABILITY;
        return 0;
    }
    if (handle >= execution->string_count) {
        execution->bad_index = signed_value(handle); execution->bound = execution->string_count;
        execution->failed_node = node; execution->fault = BIO_FAULT_STRING_HANDLE;
        return 0;
    }
    s = &execution->strings[handle];
    if (execution->remaining < s->length) {
        execution->failed_node = node; execution->fault = BIO_FAULT_BUDGET;
        return 0;
    }
    execution->remaining -= s->length;
    if (s->length && !execution->print(execution->print_user, s->data, s->length)) {
        execution->failed_node = node; execution->fault = BIO_FAULT_PRINT_FAILED;
        return 0;
    }
    return 1;
}

static uint32_t interpret_gene(const BioProgram *p, unsigned gene, const uint32_t *args,
                               BioExecution *execution, uint32_t *memory,
                               uint32_t *array, uint32_t array_count) {
    uint32_t r[8] = {0};
    const BioGene *g = &p->genes[gene];
    unsigned i;
    const CodonNode *n;
    for (i = 0; i < g->arity; ++i) r[i] = args[i];
    for (i = 0; i < g->local_cells; ++i) {
        if (!execution->remaining) return interpreter_fault(execution, g->head, BIO_FAULT_BUDGET);
        --execution->remaining;
        memory[i] = 0;
    }
    for (i = 0; i < g->local_count; ++i)
        if (!g->locals[i].is_array) memory[g->locals[i].offset] = g->locals[i].initial;
    n = g->head->next;
    while (n) {
        const CodonNode *next = n->next;
        if (n->op_code != OP_LABEL && n->op_code != OP_VAR && n->op_code != OP_ARRAY && n->op_code != OP_PARAM) {
            if (!execution->remaining) return interpreter_fault(execution, n, BIO_FAULT_BUDGET);
            --execution->remaining;
        }
        switch (n->op_code) {
        case OP_READ: case OP_WRITE: {
            const BioLocal *local = &g->locals[n->arg1];
            uint32_t capacity = local->is_parameter ? array_count : local->count;
            uint32_t *data = local->is_parameter ? array : memory + local->offset;
            int reading = n->op_code == OP_READ;
            uint32_t count = reading ? execution->input_count : r[n->arg2];
            if (reading ? !execution->input : !execution->output)
                return interpreter_fault(execution, n, reading ? BIO_FAULT_INPUT : BIO_FAULT_OUTPUT);
            execution->bad_index = signed_value(count); execution->bound = capacity;
            if (count > capacity)
                return interpreter_fault(execution, n, reading ? BIO_FAULT_READ_SIZE : BIO_FAULT_WRITE_SIZE);
            if (!reading && count > execution->output_capacity) {
                execution->bound = execution->output_capacity;
                return interpreter_fault(execution, n, BIO_FAULT_OUTPUT_SIZE);
            }
            if (execution->remaining < count) return interpreter_fault(execution, n, BIO_FAULT_BUDGET);
            execution->remaining -= count;
            if (reading) {
                if (count) memcpy(data, execution->input, (size_t)count * sizeof(*data));
                r[n->arg2] = count;
            } else {
                if (count) memcpy(execution->output, data, (size_t)count * sizeof(*data));
                execution->output_count = count; execution->output_written = 1;
            }
            break;
        }
        case OP_GET: r[n->arg1] = memory[g->locals[n->arg2].offset]; break;
        case OP_SET: memory[g->locals[n->arg1].offset] = r[n->arg2]; break;
        case OP_LEN: r[n->arg1] = g->locals[n->arg2].is_parameter ? array_count : g->locals[n->arg2].count; break;
        case OP_AGET: case OP_ASET: {
            const BioLocal *local = &g->locals[n->op_code == OP_AGET ? n->arg2 >> 8 : n->arg1];
            uint32_t capacity = local->is_parameter ? array_count : local->count;
            uint32_t *data = local->is_parameter ? array : memory + local->offset;
            uint32_t index = r[n->arg2 & 255u];
            if (index >= capacity) {
                execution->bad_index = signed_value(index); execution->bound = capacity;
                return interpreter_fault(execution, n, BIO_FAULT_BOUNDS);
            }
            if (n->op_code == OP_AGET) r[n->arg1] = data[index];
            else data[index] = r[n->arg2 >> 8];
            break;
        }
        case OP_LOAD: r[n->arg1] = n->arg2; break;
        case OP_MOV: r[n->arg1] = r[n->arg2]; break;
        case OP_ADD: r[n->arg1] += r[n->arg2]; break;
        case OP_SUB: r[n->arg1] -= r[n->arg2]; break;
        case OP_MUL: r[n->arg1] *= r[n->arg2]; break;
        case OP_DIV: case OP_MOD: {
            int64_t a = signed_value(r[n->arg1]), b = signed_value(r[n->arg2]), value;
            if (!b) return interpreter_fault(execution, n, BIO_FAULT_DIV_ZERO);
            value = n->op_code == OP_DIV ? a / b : a % b;
            if (value > INT32_MAX) return interpreter_fault(execution, n, BIO_FAULT_DIV_OVERFLOW);
            r[n->arg1] = (uint32_t)value;
            break;
        }
        case OP_AND: r[n->arg1] &= r[n->arg2]; break;
        case OP_OR: r[n->arg1] |= r[n->arg2]; break;
        case OP_XOR: r[n->arg1] ^= r[n->arg2]; break;
        case OP_NOT: r[n->arg1] = ~r[n->arg1]; break;
        case OP_LAND: r[n->arg1] = !!r[n->arg1] & !!r[n->arg2]; break;
        case OP_LOR: r[n->arg1] = !!r[n->arg1] | !!r[n->arg2]; break;
        case OP_LXOR: r[n->arg1] = !!r[n->arg1] ^ !!r[n->arg2]; break;
        case OP_LNOT: r[n->arg1] = !r[n->arg1]; break;
        case OP_SHL: case OP_SHR: case OP_SAR: {
            uint32_t value = r[n->arg1], count = r[n->arg2];
            if (count > 31) return interpreter_fault(execution, n, BIO_FAULT_SHIFT);
            if (n->op_code == OP_SHL) value <<= count;
            else {
                uint32_t sign = value & UINT32_C(0x80000000);
                value >>= count;
                /* Avoid implementation-defined signed shifts and a shift by 32. */
                if (n->op_code == OP_SAR && sign && count) value |= UINT32_MAX << (32u - count);
            }
            r[n->arg1] = value;
            break;
        }
        case OP_EQ: r[n->arg1] = r[n->arg1] == r[n->arg2]; break;
        case OP_LT: r[n->arg1] = signed_value(r[n->arg1]) < signed_value(r[n->arg2]); break;
        case OP_MIN:
            if (signed_value(r[n->arg1]) > signed_value(r[n->arg2])) r[n->arg1] = r[n->arg2];
            break;
        case OP_MAX:
            if (signed_value(r[n->arg1]) < signed_value(r[n->arg2])) r[n->arg1] = r[n->arg2];
            break;
        case OP_CALL: {
            unsigned argc = n->arg2 >> 16, i;
            uint32_t passed_args[4] = {0, 0, 0, 0};
            uint32_t *passed = NULL;
            for (i = 0; i < argc; ++i) passed_args[i] = r[(n->arg2 >> (3 * i)) & 7u];
            if (n->call_array) {
                const BioLocal *local = &g->locals[n->call_array - 1u];
                uint32_t capacity = local->is_parameter ? array_count : local->count;
                if (passed_args[0] > capacity) {
                    execution->bad_index = signed_value(passed_args[0]); execution->bound = capacity;
                    return interpreter_fault(execution, n, BIO_FAULT_ARGUMENT_SIZE);
                }
                passed = local->is_parameter ? array : memory + local->offset;
            }
            r[0] = interpret_gene(p, n->arg1, passed_args, execution,
                                  g->local_cells ? memory + g->local_cells : memory,
                                  passed, n->call_array ? passed_args[0] : 0);
            if (execution->failed_node != UINT32_MAX) return 0;
            break;
        }
        case OP_SLOAD: r[n->arg1] = n->arg2; break; /* Literal handle: its table index. */
        case OP_SLEN: {
            uint32_t h = r[n->arg2];
            if (h >= execution->string_count) {
                execution->bad_index = signed_value(h); execution->bound = execution->string_count;
                return interpreter_fault(execution, n, BIO_FAULT_STRING_HANDLE);
            }
            r[n->arg1] = execution->strings[h].length;
            break;
        }
        case OP_SGET: {
            uint32_t h = r[n->arg2 & 255u], index;
            const BioString *s;
            if (h >= execution->string_count) {
                execution->bad_index = signed_value(h); execution->bound = execution->string_count;
                return interpreter_fault(execution, n, BIO_FAULT_STRING_HANDLE);
            }
            s = &execution->strings[h];
            index = r[n->arg2 >> 8];
            execution->bad_index = signed_value(index); execution->bound = s->length;
            if (index >= s->length) return interpreter_fault(execution, n, BIO_FAULT_STRING_BOUNDS);
            r[n->arg1] = s->data[index];
            break;
        }
        case OP_SCAT: {
            uint32_t ha = r[n->arg2 & 255u], hb = r[n->arg2 >> 8];
            const BioString *sa, *sb;
            uint32_t need;
            if (ha >= execution->string_count || hb >= execution->string_count) {
                execution->bad_index = signed_value(ha >= execution->string_count ? ha : hb);
                execution->bound = execution->string_count;
                return interpreter_fault(execution, n, BIO_FAULT_STRING_HANDLE);
            }
            sa = &execution->strings[ha]; sb = &execution->strings[hb];
            need = sa->length + sb->length; /* Lengths are bounded by the pool; no overflow. */
            if (execution->string_count == BIO_MAX_STRINGS) {
                execution->bound = BIO_MAX_STRINGS;
                return interpreter_fault(execution, n, BIO_FAULT_STRING_TABLE);
            }
            if (need > execution->pool_capacity - execution->pool_used) {
                execution->bad_index = (int32_t)need;
                execution->bound = execution->pool_capacity - execution->pool_used;
                return interpreter_fault(execution, n, BIO_FAULT_STRING_BUDGET);
            }
            if (execution->remaining < need)
                return interpreter_fault(execution, n, BIO_FAULT_BUDGET);
            execution->remaining -= need;
            if (need) {
                memcpy(execution->string_pool + execution->pool_used, sa->data, sa->length);
                memcpy(execution->string_pool + execution->pool_used + sa->length, sb->data, sb->length);
            }
            execution->strings[execution->string_count].data = execution->string_pool + execution->pool_used;
            execution->strings[execution->string_count].length = need;
            execution->pool_used += need;
            r[n->arg1] = execution->string_count++;
            break;
        }
        case OP_SEQ: {
            uint32_t ha = r[n->arg2 & 255u], hb = r[n->arg2 >> 8];
            const BioString *sa, *sb;
            uint32_t charge;
            if (ha >= execution->string_count || hb >= execution->string_count) {
                execution->bad_index = signed_value(ha >= execution->string_count ? ha : hb);
                execution->bound = execution->string_count;
                return interpreter_fault(execution, n, BIO_FAULT_STRING_HANDLE);
            }
            sa = &execution->strings[ha]; sb = &execution->strings[hb];
            charge = (sa->length < sb->length ? sa->length : sb->length) + 1u;
            if (execution->remaining < charge)
                return interpreter_fault(execution, n, BIO_FAULT_BUDGET);
            execution->remaining -= charge;
            r[n->arg1] = sa->length == sb->length &&
                (!sa->length || !memcmp(sa->data, sb->data, sa->length));
            break;
        }
        case OP_PRINT:
            if (!bio_print_helper(execution, r[n->arg1], n->index)) return 0;
            break;
        case OP_RET: return r[n->arg1];
        case OP_QUARANTINE: return interpreter_fault(execution, n, BIO_FAULT_QUARANTINE);
        case OP_JMP: next = n->bond; break;
        case OP_JZ: if (!r[n->arg1]) next = n->bond; break;
        case OP_JNZ: if (r[n->arg1]) next = n->bond; break;
        default: break;
        }
        n = next;
    }
    return 0; /* Validation rejects reachable fallthrough. */
}

static int execute(const BioProgram *p, const int32_t *args, size_t argc,
                   BioIO *io, int32_t *result, BioError *e, int native) {
    typedef int32_t (*Entry)(int32_t, int32_t, int32_t, int32_t, BioExecution *, uint32_t *);
    int32_t value;
    uint32_t *memory = NULL;
    uint8_t *pool = NULL;
    BioString *strings = NULL;
    BioExecution execution = {0};
    int ok;
    if (!validate_arguments(p, args, argc, result, e)) return 0;
    if (native && !p->code) { bio_error(e, "program has no native code; use bio_compile"); return 0; }
    if (io && (io->input_count > BIO_LIST_LIMIT || io->output_capacity > BIO_LIST_LIMIT ||
               (!io->input && io->input_count) || (!io->output && io->output_capacity))) {
        bio_error(e, "invalid BioIO: lists need valid pointers and counts/capacities in 0..%u", BIO_LIST_LIMIT);
        return 0;
    }
    execution.remaining = BIO_EXECUTION_LIMIT; execution.failed_node = UINT32_MAX;
    if (p->genes[p->main_gene].memory_cells) {
        memory = malloc((size_t)p->genes[p->main_gene].memory_cells * sizeof(*memory));
        if (!memory) { bio_error(e, "out of memory allocating local storage"); return 0; }
    }
    if (io) {
        execution.input = io->input; execution.input_count = (uint32_t)io->input_count;
        execution.output_capacity = (uint32_t)io->output_capacity;
        execution.print = io->print; execution.print_user = io->print_user;
        if (io->output) {
            /* Keep all writes private until the entire invocation succeeds. */
            execution.output = malloc((io->output_capacity ? io->output_capacity : 1u) * sizeof(*execution.output));
            if (!execution.output) { free(memory); bio_error(e, "out of memory staging output list"); return 0; }
        }
    }
    if (p->uses_strings) {
        /* The JIT cannot allocate, so the table and pool exist before entry. */
        strings = malloc(BIO_MAX_STRINGS * sizeof(*strings));
        pool = malloc(BIO_STRING_BYTES);
        if (!strings || !pool) {
            free(strings); free(pool); free(execution.output); free(memory);
            bio_error(e, "out of memory preparing string storage"); return 0;
        }
        if (p->literal_count)
            memcpy(strings, p->literals, p->literal_count * sizeof(*strings));
        execution.strings = strings; execution.string_count = p->literal_count;
        execution.string_pool = pool; execution.pool_capacity = BIO_STRING_BYTES;
    }
    if (native) {
        Entry fn;
        unsigned char *address = p->code + p->genes[p->main_gene].code_offset;
        _Static_assert(sizeof(fn) == sizeof(address), "Windows x64 function pointer size");
        memcpy(&fn, &address, sizeof(fn));
        value = fn(argc > 0 ? args[0] : 0, argc > 1 ? args[1] : 0,
                   argc > 2 ? args[2] : 0, argc > 3 ? args[3] : 0, &execution, memory);
    } else {
        uint32_t entry_args[4] = {0, 0, 0, 0};
        size_t i;
        for (i = 0; i < argc; ++i) entry_args[i] = (uint32_t)args[i];
        value = signed_value(interpret_gene(p, p->main_gene, entry_args, &execution, memory, NULL, 0));
    }
    ok = execution_ok(p, &execution, e);
    if (ok) {
        if (io) {
            if (execution.output_count) memcpy(io->output, execution.output, (size_t)execution.output_count * sizeof(*io->output));
            io->output_count = execution.output_count; io->output_written = (int)execution.output_written;
        }
        *result = value;
    }
    free(execution.output);
    free(memory);
    free(strings);
    free(pool);
    return ok;
}

int bio_program_run_io(const BioProgram *p, const int32_t *args, size_t argc,
                       BioIO *io, int32_t *result, BioError *e) {
    return execute(p, args, argc, io, result, e, 1);
}

int bio_program_interpret_io(const BioProgram *p, const int32_t *args, size_t argc,
                             BioIO *io, int32_t *result, BioError *e) {
    return execute(p, args, argc, io, result, e, 0);
}

int bio_program_run(const BioProgram *p, const int32_t *args, size_t argc,
                    int32_t *result, BioError *e) {
    return bio_program_run_io(p, args, argc, NULL, result, e);
}

int bio_program_interpret(const BioProgram *p, const int32_t *args, size_t argc,
                          int32_t *result, BioError *e) {
    return bio_program_interpret_io(p, args, argc, NULL, result, e);
}

void bio_program_dump(const BioProgram *p, FILE *out) {
    static const char *names[] = {
        "GENE", "LOAD", "MOV", "ADD", "SUB", "MUL", "CALL", "RET", "EQ", "LT", "MIN", "MAX",
        "LABEL", "JMP", "JZ", "JNZ", "DIV", "MOD", "AND", "OR", "XOR", "NOT",
        "LAND", "LOR", "LXOR", "LNOT", "SHL", "SHR", "SAR",
        "VAR", "ARRAY", "GET", "SET", "AGET", "ASET", "LEN", "READ", "WRITE", "PARAM", "QUARANTINE",
        "SLOAD", "SLEN", "SGET", "SCAT", "SEQ", "PRINT"
    };
    unsigned i;
    for (i = 0; i < p->gene_count; ++i) {
        const BioGene *g = &p->genes[i];
        const CodonNode *n;
        fprintf(out, "GENE %s i32/%u # %s\n", g->name, g->arity, g->live ? "live" : "pruned");
        for (n = g->head->next; n; n = n->next) {
            if (n->op_code == OP_QUARANTINE)
                fputs("QUARANTINE _ _", out);
            else if (n->op_code == OP_CALL) {
                unsigned argc = n->arg2 >> 16, k;
                fprintf(out, "CALL %s ", p->genes[n->arg1].name);
                if (n->call_array) fprintf(out, "%s,", p->symbols[g->locals[n->call_array - 1u].symbol]);
                if (!argc) fputc('_', out);
                else for (k = 0; k < argc; ++k)
                    fprintf(out, "%sr%u", k ? "," : "", (n->arg2 >> (3 * k)) & 7u);
            } else if (n->op_code == OP_PARAM)
                fprintf(out, "PARAM %s r0", p->symbols[n->arg1]);
            else if (n->op_code == OP_VAR || n->op_code == OP_ARRAY)
                fprintf(out, "%s %s %ld", names[n->op_code], p->symbols[n->arg1], (long)signed_value(n->arg2));
            else if (n->op_code == OP_GET || n->op_code == OP_LEN)
                fprintf(out, "%s r%u %s", names[n->op_code], n->arg1, p->symbols[g->locals[n->arg2].symbol]);
            else if (n->op_code == OP_SET || n->op_code == OP_READ || n->op_code == OP_WRITE)
                fprintf(out, "%s %s r%u", names[n->op_code], p->symbols[g->locals[n->arg1].symbol], n->arg2);
            else if (n->op_code == OP_AGET)
                fprintf(out, "AGET r%u %s,r%u", n->arg1, p->symbols[g->locals[n->arg2 >> 8].symbol], n->arg2 & 255u);
            else if (n->op_code == OP_ASET)
                fprintf(out, "ASET %s r%u,r%u", p->symbols[g->locals[n->arg1].symbol], n->arg2 & 255u, n->arg2 >> 8);
            else if (n->op_code == OP_SLOAD) {
                const BioString *s = &p->literals[n->arg2];
                uint32_t k;
                fprintf(out, "SLOAD r%u \"", n->arg1);
                for (k = 0; k < s->length; ++k) {
                    uint8_t c = s->data[k];
                    if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
                    else if (c == '\n') fputs("\\n", out);
                    else if (c == '\t') fputs("\\t", out);
                    else if (c >= 32 && c < 127) fputc(c, out);
                    else fprintf(out, "\\x%02x", c);
                }
                fputc('"', out);
            }
            else if (n->op_code == OP_SGET || n->op_code == OP_SCAT || n->op_code == OP_SEQ)
                fprintf(out, "%s r%u r%u,r%u", names[n->op_code], n->arg1, n->arg2 & 255u, n->arg2 >> 8);
            else if (n->op_code == OP_LOAD)
                fprintf(out, "LOAD r%u %ld", n->arg1, (long)signed_value(n->arg2));
            else if (n->op_code == OP_RET || n->op_code == OP_NOT || n->op_code == OP_LNOT ||
                     n->op_code == OP_PRINT)
                fprintf(out, "%s r%u _", names[n->op_code], n->arg1);
            else if (n->op_code == OP_LABEL || n->op_code == OP_JMP)
                fprintf(out, "%s %s _", names[n->op_code], p->symbols[n->arg1]);
            else if (n->op_code == OP_JZ || n->op_code == OP_JNZ)
                fprintf(out, "%s r%u %s", names[n->op_code], n->arg1, p->symbols[n->arg2]);
            else fprintf(out, "%s r%u r%u", names[n->op_code], n->arg1, n->arg2);
            fprintf(out, " # line %u\n", n->line);
        }
        fputs("END _ _\n\n", out);
    }
}

int bio_program_write_code(const BioProgram *p, const char *path, BioError *e) {
    FILE *f;
    int ok;
    if (!p || !p->code) { bio_error(e, "program has no native code"); return 0; }
    f = bio_fopen(path, L"wb");
    if (!f) { bio_error(e, "cannot write %s", path); return 0; }
    ok = fwrite(p->code, 1, p->stats.code_bytes, f) == p->stats.code_bytes;
    if (fclose(f) != 0) ok = 0;
    if (!ok) bio_error(e, "failed writing %s", path);
    return ok;
}
