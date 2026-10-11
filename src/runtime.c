#include "internal.h"
#include <stdlib.h>

struct BioRuntime {
    SRWLOCK lock;
    BioProgram *active;
};

BioRuntime *bio_runtime_create(BioProgram *initial, BioError *e) {
    BioRuntime *runtime;
    if (!initial || !initial->code) { bio_error(e, "initial program with native code is required"); return NULL; }
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) { bio_error(e, "out of memory allocating runtime"); return NULL; }
    InitializeSRWLock(&runtime->lock);
    bio_program_retain(initial);
    runtime->active = initial;
    return runtime;
}

int bio_runtime_run(BioRuntime *runtime, const int32_t *arguments, size_t argument_count,
                    int32_t *result, BioError *e) {
    BioProgram *p;
    int ok;
    if (!runtime) { bio_error(e, "runtime is required"); return 0; }
    /* Retaining under the same lock as publication closes the free/acquire race. */
    AcquireSRWLockShared(&runtime->lock);
    p = runtime->active;
    bio_program_retain(p);
    ReleaseSRWLockShared(&runtime->lock);
    ok = bio_program_run(p, arguments, argument_count, result, e);
    bio_program_release(p);
    return ok;
}

int bio_runtime_reload(BioRuntime *runtime, const char *path, int repair, BioError *e) {
    BioProgram *old;
    BioProgram *candidate;
    unsigned i;
    if (!runtime) { bio_error(e, "runtime is required"); return 0; }
    candidate = bio_compile(path, repair, e);
    if (!candidate) return 0;
    AcquireSRWLockExclusive(&runtime->lock);
    old = runtime->active;
    /* Compare against the currently published version, even with concurrent writers. */
    for (i = 0; i < old->gene_count; ++i) {
        int found = bio_find_gene(candidate, old->genes[i].name);
        if (found < 0 || candidate->genes[found].arity != old->genes[i].arity) {
            bio_error(e, "repair rejected: incompatible signature or removed gene %s", old->genes[i].name);
            ReleaseSRWLockExclusive(&runtime->lock);
            bio_program_release(candidate);
            return 0;
        }
    }
    runtime->active = candidate;
    ReleaseSRWLockExclusive(&runtime->lock);
    /* Existing calls hold references; reclamation happens after their last return. */
    bio_program_release(old);
    return 1;
}

void bio_runtime_destroy(BioRuntime *runtime) {
    if (!runtime) return;
    bio_program_release(runtime->active);
    free(runtime);
}
