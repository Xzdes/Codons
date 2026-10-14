#include "internal.h"
#include <stdlib.h>
#ifndef _WIN32
#include <pthread.h>
#endif

struct BioRuntime {
#ifdef _WIN32
    SRWLOCK lock;
#else
    pthread_rwlock_t lock;
#endif
    BioProgram *active;
};

#ifdef _WIN32
#define BIO_LOCK_INIT(p) InitializeSRWLock(&(p)->lock)
#define BIO_LOCK_SHARED(p) AcquireSRWLockShared(&(p)->lock)
#define BIO_UNLOCK_SHARED(p) ReleaseSRWLockShared(&(p)->lock)
#define BIO_LOCK_EXCL(p) AcquireSRWLockExclusive(&(p)->lock)
#define BIO_UNLOCK_EXCL(p) ReleaseSRWLockExclusive(&(p)->lock)
#define BIO_LOCK_DESTROY(p) ((void)(p))
#else
#define BIO_LOCK_INIT(p) pthread_rwlock_init(&(p)->lock, NULL)
#define BIO_LOCK_SHARED(p) pthread_rwlock_rdlock(&(p)->lock)
#define BIO_UNLOCK_SHARED(p) pthread_rwlock_unlock(&(p)->lock)
#define BIO_LOCK_EXCL(p) pthread_rwlock_wrlock(&(p)->lock)
#define BIO_UNLOCK_EXCL(p) pthread_rwlock_unlock(&(p)->lock)
#define BIO_LOCK_DESTROY(p) pthread_rwlock_destroy(&(p)->lock)
#endif

BioRuntime *bio_runtime_create(BioProgram *initial, BioError *e) {
    BioRuntime *runtime;
#ifdef BIO_NO_JIT
    if (!initial) { bio_error(e, "initial program is required"); return NULL; }
#else
    if (!initial || !initial->code) { bio_error(e, "initial program with native code is required"); return NULL; }
#endif
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) { bio_error(e, "out of memory allocating runtime"); return NULL; }
    BIO_LOCK_INIT(runtime);
    bio_program_retain(initial);
    runtime->active = initial;
    return runtime;
}

int bio_runtime_run(BioRuntime *runtime, const int32_t *arguments, size_t argument_count,
                    int32_t *result, BioError *e) {
    return bio_runtime_run_io(runtime, arguments, argument_count, NULL, result, e);
}

int bio_runtime_run_io(BioRuntime *runtime, const int32_t *arguments, size_t argument_count,
                       BioIO *io, int32_t *result, BioError *e) {
    BioProgram *p;
    int ok;
    if (!runtime) { bio_error(e, "runtime is required"); return 0; }
    /* Retaining under the same lock as publication closes the free/acquire race. */
    BIO_LOCK_SHARED(runtime);
    p = runtime->active;
    bio_program_retain(p);
    BIO_UNLOCK_SHARED(runtime);
    ok = bio_program_run_io(p, arguments, argument_count, io, result, e);
    bio_program_release(p);
    return ok;
}

int bio_runtime_publish(BioRuntime *runtime, BioProgram *candidate, BioError *e) {
    BioProgram *old;
    unsigned i;
#ifdef BIO_NO_JIT
    if (!runtime || !candidate) {
        bio_error(e, "runtime and a candidate program are required"); return 0;
    }
#else
    if (!runtime || !candidate || !candidate->code) {
        bio_error(e, "runtime and a compiled candidate program are required"); return 0;
    }
#endif
    BIO_LOCK_EXCL(runtime);
    old = runtime->active;
    /* Compare against the currently published version, even with concurrent writers. */
    for (i = 0; i < old->gene_count; ++i) {
        int found = bio_find_gene(candidate, old->genes[i].name);
        if (found < 0 || candidate->genes[found].arity != old->genes[i].arity ||
            candidate->genes[found].has_array != old->genes[i].has_array) {
            bio_error(e, "repair rejected: incompatible signature or removed gene %s", old->genes[i].name);
            BIO_UNLOCK_EXCL(runtime);
            return 0;
        }
    }
    bio_program_retain(candidate);
    runtime->active = candidate;
    BIO_UNLOCK_EXCL(runtime);
    /* Existing calls hold references; reclamation happens after their last return. */
    bio_program_release(old);
    return 1;
}

int bio_runtime_reload(BioRuntime *runtime, const char *path, int repair, BioError *e) {
    BioProgram *candidate;
    int ok;
    if (!runtime) { bio_error(e, "runtime is required"); return 0; }
    candidate = bio_compile(path, repair, e);
    if (!candidate) return 0;
    ok = bio_runtime_publish(runtime, candidate, e);
    bio_program_release(candidate);
    return ok;
}

void bio_runtime_destroy(BioRuntime *runtime) {
    if (!runtime) return;
    bio_program_release(runtime->active);
    BIO_LOCK_DESTROY(runtime);
    free(runtime);
}
