#include "internal.h"
#include <stdlib.h>
#include <string.h>

static int link_gene(BioProgram *p, BioGene *g, BioError *e) {
    CodonNode *labels[BIO_MAX_SYMBOLS] = {0};
    CodonNode *n;
    for (n = g->head->next; n; n = n->next) {
        if (n->op_code != OP_LABEL) continue;
        if (labels[n->arg1]) {
            if (p->repair_mode) {
                if (!bio_quarantine(p, n, "duplicate label has no unambiguous target", e)) return 0;
                continue;
            }
            bio_node_error(p, n, e, "duplicate label %s in gene %s", p->symbols[n->arg1], g->name);
            return 0;
        }
        labels[n->arg1] = n;
    }
    for (n = g->head->next; n; n = n->next) {
        if (n->op_code == OP_CALL) {
            unsigned argc = n->flow; /* Parse-time count; arg2 packs 8-bit registers. */
            int target = bio_find_gene(p, p->symbols[n->arg1]);
            if (target < 0) {
                if (p->repair_mode) {
                    if (!bio_quarantine(p, n, "CALL target is undefined", e)) return 0;
                    continue;
                }
                bio_node_error(p, n, e, "undefined gene: %s (declare it: GENE %s i32/N ... END _ _, or fix the name)",
                               p->symbols[n->arg1], p->symbols[n->arg1]); return 0;
            }
            if (p->genes[target].arity != argc) {
                if (p->repair_mode) {
                    if (!bio_quarantine(p, n, "CALL argument count does not match target", e)) return 0;
                    continue;
                }
                bio_node_error(p, n, e, "gene %s expects %u arguments, got %u",
                               p->genes[target].name, p->genes[target].arity, argc); return 0;
            }
            if (p->genes[target].has_array != (n->call_array != 0)) {
                if (p->repair_mode) {
                    if (!bio_quarantine(p, n, "CALL array argument does not match target", e)) return 0;
                    continue;
                }
                bio_node_error(p, n, e, "gene %s %s an array argument", p->genes[target].name,
                               p->genes[target].has_array ? "requires" : "does not accept");
                return 0;
            }
            if (n->call_array) {
                unsigned i, sym = n->call_array - 1u;
                for (i = 0; i < g->local_count && g->locals[i].symbol != sym; ++i) {}
                if (i == g->local_count) {
                    if (p->repair_mode) {
                        if (!bio_quarantine(p, n, "CALL array local is undefined", e)) return 0;
                        continue;
                    }
                    bio_node_error(p, n, e, "undefined local %s in gene %s", p->symbols[sym], g->name);
                    return 0;
                }
                if (!g->locals[i].is_array) {
                    if (p->repair_mode) {
                        if (!bio_quarantine(p, n, "CALL array argument names a scalar variable", e)) return 0;
                        continue;
                    }
                    bio_node_error(p, n, e, "%s is a variable; CALL requires an array", p->symbols[sym]);
                    return 0;
                }
                n->call_array = i + 1u;
            }
            n->arg1 = (uint32_t)target;
            n->bond = p->genes[target].head;
        } else if (n->op_code == OP_JMP || n->op_code == OP_JZ || n->op_code == OP_JNZ) {
            unsigned target = n->op_code == OP_JMP ? n->arg1 : n->arg2;
            n->bond = labels[target];
            if (!n->bond) {
                if (p->repair_mode) {
                    if (!bio_quarantine(p, n, "branch target label is undefined", e)) return 0;
                    continue;
                }
                bio_node_error(p, n, e, "undefined label %s in gene %s (labels are per-gene; add LABEL %s _)",
                               p->symbols[target], g->name, p->symbols[target]);
                return 0;
            }
        } else if (n->op_code >= OP_GET && n->op_code <= OP_WRITE) {
            unsigned i, target = n->op_code == OP_AGET ? n->arg2 >> 8 :
                (n->op_code == OP_SET || n->op_code == OP_ASET || n->op_code == OP_READ || n->op_code == OP_WRITE) ? n->arg1 : n->arg2;
            unsigned array = n->op_code == OP_AGET || n->op_code == OP_ASET || n->op_code == OP_LEN ||
                n->op_code == OP_READ || n->op_code == OP_WRITE;
            for (i = 0; i < g->local_count && g->locals[i].symbol != target; ++i) {}
            if (i == g->local_count) {
                if (p->repair_mode) {
                    if (!bio_quarantine(p, n, "local name is undefined", e)) return 0;
                    continue;
                }
                bio_node_error(p, n, e, "undefined local %s in gene %s (declare it after GENE, before the first "
                               "instruction: VAR %s 0 or ARRAY %s 8)", p->symbols[target], g->name,
                               p->symbols[target], p->symbols[target]);
                return 0;
            }
            if (g->locals[i].is_array != array) {
                if (p->repair_mode) {
                    if (!bio_quarantine(p, n, "local kind does not match the operation", e)) return 0;
                    continue;
                }
                bio_node_error(p, n, e, "%s is %s; this operation requires %s", p->symbols[target],
                               array ? "a variable" : "an array", array ? "an array" : "a variable");
                return 0;
            }
            if (n->op_code == OP_AGET) n->arg2 = (i << 8) | (n->arg2 & 255u);
            else if (n->op_code == OP_SET || n->op_code == OP_ASET || n->op_code == OP_READ || n->op_code == OP_WRITE) n->arg1 = i;
            else n->arg2 = i;
        }
    }
    return 1;
}

/* Initialization sets are bitsets: bits 0..reg_count-1 are r-registers,
   bits reg_count..+3 are the wide w0..w3. */
static void set_bit(uint64_t *set, unsigned bit) {
    set[bit >> 6] |= UINT64_C(1) << (bit & 63u);
}

static void definitions(const CodonNode *n, uint64_t *set, unsigned wide_base) {
    switch (n->op_code) {
    case OP_LOAD: case OP_MOV: case OP_ADD: case OP_SUB: case OP_MUL:
    case OP_EQ: case OP_LT: case OP_MIN: case OP_MAX:
    case OP_DIV: case OP_MOD: case OP_AND: case OP_OR: case OP_XOR: case OP_NOT:
    case OP_LAND: case OP_LOR: case OP_LXOR: case OP_LNOT:
    case OP_SHL: case OP_SHR: case OP_SAR:
    case OP_GET: case OP_AGET: case OP_LEN:
    case OP_SLOAD: case OP_SLEN: case OP_SGET: case OP_SCAT: case OP_SEQ:
    case OP_WEQ: case OP_WLT: case OP_NARROW:
        set_bit(set, n->arg1); break;
    case OP_CALL: set_bit(set, 0); break;
    case OP_READ: set_bit(set, n->arg2); break;
    case OP_WLOAD: case OP_WMOV: case OP_WADD: case OP_WSUB: case OP_WMUL:
    case OP_WDIV: case OP_WMOD: case OP_WAND: case OP_WOR: case OP_WXOR: case OP_WNOT:
    case OP_WSHL: case OP_WSHR: case OP_WSAR: case OP_WMIN: case OP_WMAX:
    case OP_WIDEN:
        set_bit(set, wide_base + n->arg1); break;
    default: break;
    }
}

static void reads(const BioProgram *p, const CodonNode *n, uint64_t *set, unsigned wide_base) {
    switch (n->op_code) {
    case OP_MOV: case OP_SET: case OP_WRITE: set_bit(set, n->arg2); break;
    case OP_AGET: set_bit(set, n->arg2 & 255u); break;
    case OP_ASET: set_bit(set, n->arg2 & 255u); set_bit(set, n->arg2 >> 8); break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_EQ: case OP_LT: case OP_MIN: case OP_MAX:
    case OP_DIV: case OP_MOD: case OP_AND: case OP_OR: case OP_XOR:
    case OP_LAND: case OP_LOR: case OP_LXOR: case OP_SHL: case OP_SHR: case OP_SAR:
        set_bit(set, n->arg1); set_bit(set, n->arg2); break;
    case OP_RET: case OP_JZ: case OP_JNZ: case OP_NOT: case OP_LNOT: case OP_PRINT:
        set_bit(set, n->arg1); break;
    case OP_SLEN: set_bit(set, n->arg2); break;
    case OP_SGET: case OP_SCAT: case OP_SEQ:
        set_bit(set, n->arg2 & 255u); set_bit(set, n->arg2 >> 8); break;
    case OP_WMOV: set_bit(set, wide_base + n->arg2); break;
    case OP_WIDEN: set_bit(set, n->arg2); break;
    case OP_NARROW: set_bit(set, wide_base + n->arg2); break;
    case OP_WADD: case OP_WSUB: case OP_WMUL: case OP_WDIV: case OP_WMOD:
    case OP_WAND: case OP_WOR: case OP_WXOR: case OP_WSHL: case OP_WSHR: case OP_WSAR:
    case OP_WMIN: case OP_WMAX:
        set_bit(set, wide_base + n->arg1); set_bit(set, wide_base + n->arg2); break;
    case OP_WNOT: set_bit(set, wide_base + n->arg1); break;
    case OP_WEQ: case OP_WLT:
        set_bit(set, wide_base + (n->arg2 & 255u)); set_bit(set, wide_base + (n->arg2 >> 8)); break;
    case OP_CALL: {
        /* After linking, the callee's arity says how many 8-bit fields are live. */
        unsigned argc = p->genes[n->arg1].arity, i;
        for (i = 0; i < argc; ++i) set_bit(set, (n->arg2 >> (8 * i)) & 255u);
        break;
    }
    default: break;
    }
}

/* A queue of existing nodes is enough: no new intermediate representation.
   Initialization sets live in a per-gene arena of words; the meet is a bitwise
   AND, so bits can only clear after first discovery. */
static int validate_flow(BioProgram *p, BioGene *g, CodonNode **queue, BioError *e) {
    size_t front = 0, back = 0, pending = 0, capacity = p->stats.nodes;
    CodonNode *entry = g->head->next, *n;
    unsigned node_count = 0, i, w;
    const unsigned bits = g->reg_count + 4u, words = (bits + 63u) / 64u;
    uint64_t *sets, *out, *need;
    int result = 1;
    if (!entry) { bio_node_error(p, g->head, e, "empty gene"); return 0; }
    for (n = entry; n; n = n->next) {
        n->reachable = 0;
        n->queued = 0;
        n->flow = node_count++;
    }
    sets = malloc((size_t)node_count * words * sizeof(*sets));
    out = malloc(words * sizeof(*out));
    need = malloc(words * sizeof(*need));
    if (!sets || !out || !need) {
        bio_node_error(p, g->head, e, "out of memory tracking register initialization");
        free(sets); free(out); free(need);
        return 0;
    }
    for (i = 0; i < node_count * words; ++i) sets[i] = UINT64_MAX; /* Top: all initialized. */
    memset(sets, 0, words * sizeof(*sets)); /* Entry: only the argument registers. */
    sets[0] = (1u << g->arity) - 1u;
    entry->reachable = entry->queued = 1;
    queue[back++ % capacity] = entry; ++pending;
    while (pending) {
        CodonNode *successors[2];
        unsigned count = 0;
        n = queue[front++ % capacity]; --pending; n->queued = 0;
        memcpy(out, sets + (size_t)n->flow * words, words * sizeof(*out));
        definitions(n, out, g->reg_count);
        if (n->op_code == OP_JMP) successors[count++] = n->bond;
        else if (n->op_code != OP_RET && n->op_code != OP_QUARANTINE) {
            successors[count++] = n->next;
            if (n->op_code == OP_JZ || n->op_code == OP_JNZ) successors[count++] = n->bond;
        }
        for (i = 0; i < count; ++i) {
            CodonNode *next = successors[i];
            uint64_t *target;
            int changed;
            if (!next) {
                if (p->repair_mode) {
                    if (!bio_quarantine(p, n, "reachable path ended without RET or JMP", e)) result = 0;
                    else result = 2;
                    goto done;
                }
                bio_node_error(p, n, e, "reachable path falls through END; it must end with RET or JMP");
                goto fail;
            }
            target = sets + (size_t)next->flow * words;
            changed = !next->reachable;
            for (w = 0; w < words; ++w) {
                uint64_t mask = target[w] & out[w];
                if (mask != target[w]) changed = 1;
                target[w] = mask;
            }
            next->reachable = 1;
            if (changed && !next->queued) {
                queue[back++ % capacity] = next; ++pending; next->queued = 1;
            }
        }
    }
    for (n = entry; n; n = n->next) {
        const uint64_t *have;
        unsigned r;
        if (!n->reachable) continue;
        memset(need, 0, words * sizeof(*need));
        reads(p, n, need, g->reg_count);
        have = sets + (size_t)n->flow * words;
        r = UINT32_MAX;
        for (w = 0; w < words && r == UINT32_MAX; ++w) {
            uint64_t missing = need[w] & ~have[w];
            if (missing) r = w * 64u + (unsigned)__builtin_ctzll(missing);
        }
        if (r != UINT32_MAX) {
            if (p->repair_mode) {
                if (!bio_quarantine(p, n, "instruction reads an uninitialized register", e)) result = 0;
                else result = 2;
                goto done;
            }
            if (r < g->reg_count)
                bio_node_error(p, n, e, "read of uninitialized register r%u on a reachable path "
                               "(write it before this point on every path, e.g. LOAD r%u 0 "
                               "right after GENE or before the loop)", r, r);
            else
                bio_node_error(p, n, e, "read of uninitialized wide register w%u on a reachable path "
                               "(write it before this point on every path, e.g. WLOAD w%u 0)",
                               r - g->reg_count, r - g->reg_count);
            goto fail;
        }
        if (n->op_code == OP_JMP || n->op_code == OP_JZ || n->op_code == OP_JNZ ||
            n->op_code == OP_READ || n->op_code == OP_WRITE)
            g->dynamic_steps = 1;
    }
    goto done;
fail:
    result = 0;
done:
    free(sets); free(out); free(need);
    return result;
}

/* Recursion is allowed: cycles make steps dynamic and depth/cells are bounded
   at run time (BIO_MAX_DEPTH active genes, BIO_MEMORY_CELLS of frame storage).
   The static sums below remain exact for acyclic edges; cycle edges skip them. */
static int check_calls(BioProgram *p, unsigned index, BioError *e) {
    BioGene *g = &p->genes[index];
    CodonNode *n;
    if (g->visit == 1) return 1; /* Back edge: the caller flags the cycle. */
    if (g->visit == 2) return 1;
    g->visit = 1; g->depth = 1;
    g->execution_steps = g->reg_base; /* Only declared cells are zeroed and charged. */
    g->memory_cells = g->local_cells;
    for (n = g->head->next; n; n = n->next) {
        if (n->reachable && n->op_code != OP_LABEL && n->op_code != OP_VAR &&
            n->op_code != OP_ARRAY && n->op_code != OP_PARAM && g->execution_steps <= BIO_EXECUTION_LIMIT)
            ++g->execution_steps;
        if (n->op_code == OP_CALL) {
            BioGene *callee = &p->genes[n->arg1];
            if (callee->visit == 1) {
                /* Cycle edge: runtime limits apply, static sums stop here. */
                p->has_recursion = 1;
                if (n->reachable) g->dynamic_steps = 1;
                continue;
            }
            if (!check_calls(p, n->arg1, e)) return 0;
            if (callee->depth + 1 > g->depth) g->depth = callee->depth + 1;
            if (n->reachable) {
                uint32_t cells = g->local_cells + callee->memory_cells;
                if (cells > g->memory_cells) g->memory_cells = cells;
                g->dynamic_steps |= callee->dynamic_steps;
                g->execution_steps += callee->execution_steps;
                if (g->execution_steps > BIO_EXECUTION_LIMIT) g->execution_steps = BIO_EXECUTION_LIMIT + 1u;
            }
        }
    }
    if (g->dynamic_steps) g->execution_steps = 0;
    g->visit = 2;
    return 1;
}

static void mark_live(BioProgram *p, unsigned index) {
    BioGene *g = &p->genes[index];
    CodonNode *n;
    if (g->live) return;
    g->live = 1; p->stats.live_genes++;
    for (n = g->head->next; n; n = n->next)
        if (n->reachable && n->op_code == OP_CALL) mark_live(p, n->arg1);
}

int bio_analyze(BioProgram *p, BioError *e) {
    unsigned i;
    CodonNode **queue;
    int main_gene = bio_find_gene(p, "main");
    if (main_gene < 0) { bio_error(e, "missing entry gene main"); return 0; }
    p->main_gene = (unsigned)main_gene;
    p->stats.arity = p->genes[main_gene].arity;
    for (i = 0; i < p->gene_count; ++i) if (!link_gene(p, &p->genes[i], e)) return 0;
    queue = malloc(p->stats.nodes * sizeof(*queue));
    if (!queue) { bio_error(e, "out of memory checking control flow"); return 0; }
    for (i = 0; i < p->gene_count; ++i) {
        int flow;
        do flow = validate_flow(p, &p->genes[i], queue, e); while (flow == 2);
        if (!flow) { free(queue); return 0; }
    }
    free(queue);
    for (i = 0; i < p->gene_count; ++i) if (!check_calls(p, i, e)) return 0;
    mark_live(p, p->main_gene);
    if (!p->has_recursion && p->genes[main_gene].memory_cells > BIO_MEMORY_CELLS) {
        bio_node_error(p, p->genes[main_gene].head, e,
                       "memory budget exceeded: active calls require more than %u i32 cells", BIO_MEMORY_CELLS);
        return 0;
    }
    p->stats.execution_steps = p->genes[main_gene].execution_steps;
    p->stats.dynamic_steps = p->genes[main_gene].dynamic_steps;
    if (!p->stats.dynamic_steps && p->stats.execution_steps > BIO_EXECUTION_LIMIT) {
        bio_node_error(p, p->genes[main_gene].head, e,
                       "execution budget exceeded: main requires more than %u steps", BIO_EXECUTION_LIMIT);
        return 0;
    }
    return 1;
}
