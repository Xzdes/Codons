#include "internal.h"
#include <stdlib.h>

struct BioRuntime {
    SRWLOCK lock;
    BioProgram *active;
};

BioRuntime *bio_runtime_create(BioProgram *initial, BioError *e) {
    BioRuntime *runtime;
    if (!initial) { bio_error(e, "initial program is required"); return NULL; }
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) { bio_error(e, "out of memory allocating runtime"); return NULL; }
    InitializeSRWLock(&runtime->lock);
    bio_program_retain(initial);
    runtime->active = initial;
    return runtime;
}

int32_t bio_runtime_run(BioRuntime *runtime) {
    BioProgram *p;
    int32_t result;
    /* Retaining under the same lock as publication closes the free/acquire race. */
    AcquireSRWLockShared(&runtime->lock);
    p = runtime->active;
    bio_program_retain(p);
    ReleaseSRWLockShared(&runtime->lock);
    result = bio_program_run(p);
    bio_program_release(p);
    return result;
}

int bio_runtime_reload(BioRuntime *runtime, const char *path, int repair, BioError *e) {
    BioProgram *old;
    BioProgram *candidate = bio_compile(path, repair, e);
    unsigned i;
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
