#include "internal.h"

static int require_reg(const BioProgram *p, const CodonNode *n, unsigned mask, unsigned r, BioError *e) {
    if (mask & (1u << r)) return 1;
    bio_node_error(p, n, e, "read of uninitialized register r%u", r);
    return 0;
}

static int validate_gene(BioProgram *p, BioGene *g, BioError *e) {
    unsigned initialized = (1u << g->arity) - 1u;
    CodonNode *n;
    for (n = g->head->next; n; n = n->next) {
        switch (n->op_code) {
        case OP_LOAD: initialized |= 1u << n->arg1; break;
        case OP_MOV:
            if (!require_reg(p, n, initialized, n->arg2, e)) return 0;
            initialized |= 1u << n->arg1;
            break;
        case OP_ADD: case OP_SUB: case OP_MUL:
            if (!require_reg(p, n, initialized, n->arg1, e) ||
                !require_reg(p, n, initialized, n->arg2, e)) return 0;
            break;
        case OP_RET:
            if (!require_reg(p, n, initialized, n->arg1, e)) return 0;
            break;
        case OP_CALL: {
            unsigned argc = n->arg2 >> 16;
            int target = bio_find_gene(p, p->symbols[n->arg1]);
            if (target < 0) {
                bio_node_error(p, n, e, "undefined gene: %s", p->symbols[n->arg1]); return 0;
            }
            if (p->genes[target].arity != argc) {
                bio_node_error(p, n, e, "gene %s expects %u arguments, got %u",
                               p->genes[target].name, p->genes[target].arity, argc); return 0;
            }
            if (argc > 0 && !require_reg(p, n, initialized, n->arg2 & 255u, e)) return 0;
            if (argc > 1 && !require_reg(p, n, initialized, (n->arg2 >> 8) & 255u, e)) return 0;
            n->arg1 = (uint32_t)target;
            n->bond = p->genes[target].head;
            initialized |= 1u; /* CALL's result is always r0; other registers survive. */
            break;
        }
        default: break;
        }
    }
    return 1;
}

/* DAG only in v0.1: prevent both cycles and excessive stack depth.
   Memoized longest paths also catch deep paths through already visited genes. */
static int check_calls(BioProgram *p, unsigned index, unsigned nesting, BioError *e) {
    BioGene *g = &p->genes[index];
    CodonNode *n;
    if (g->visit == 1) {
        bio_node_error(p, g->head, e, "apoptosis: recursive call cycle at %s", g->name); return 0;
    }
    if (nesting > BIO_MAX_DEPTH) {
        bio_node_error(p, g->head, e, "apoptosis: call depth exceeds %u", BIO_MAX_DEPTH); return 0;
    }
    if (g->visit == 2) return 1;
    g->visit = 1;
    g->depth = 1;
    for (n = g->head->next; n; n = n->next) {
        unsigned depth;
        if (n->op_code != OP_CALL) continue;
        if (!check_calls(p, n->arg1, nesting + 1, e)) return 0;
        depth = p->genes[n->arg1].depth + 1;
        if (depth > BIO_MAX_DEPTH) {
            bio_node_error(p, n, e, "apoptosis: call depth exceeds %u", BIO_MAX_DEPTH); return 0;
        }
        if (depth > g->depth) g->depth = depth;
    }
    g->visit = 2;
    return 1;
}

static void mark_live(BioProgram *p, unsigned index) {
    BioGene *g = &p->genes[index];
    CodonNode *n;
    if (g->live) return;
    g->live = 1;
    p->stats.live_genes++;
    for (n = g->head->next; n; n = n->next)
        if (n->op_code == OP_CALL) mark_live(p, n->arg1);
}

int bio_analyze(BioProgram *p, BioError *e) {
    unsigned i;
    int main_gene = bio_find_gene(p, "main");
    if (main_gene < 0) { bio_error(e, "missing entry gene main"); return 0; }
    p->main_gene = (unsigned)main_gene;
    if (p->genes[main_gene].arity != 0) {
        bio_node_error(p, p->genes[main_gene].head, e, "main must have signature i32/0"); return 0;
    }
    for (i = 0; i < p->gene_count; ++i) if (!validate_gene(p, &p->genes[i], e)) return 0;
    for (i = 0; i < p->gene_count; ++i) if (!check_calls(p, i, 1, e)) return 0;
    mark_live(p, p->main_gene);
    return 1;
}
