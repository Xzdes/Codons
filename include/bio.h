#ifndef BIO_H
#define BIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BIO_VERSION "0.17.0"
#define BIO_EXECUTION_LIMIT 1000000u
#define BIO_LIST_LIMIT 65536u

typedef struct BioProgram BioProgram;
typedef struct BioRuntime BioRuntime;

typedef struct {
    char message[1536];
} BioError;

typedef struct {
    size_t nodes;
    size_t genes;
    size_t live_genes;
    size_t files;
    size_t code_bytes;
    size_t repairs;
    size_t quarantines;
    unsigned arity;
    uint64_t execution_steps;
    unsigned dynamic_steps; /* execution_steps is unknown (zero) when this is nonzero. */
    uint32_t entry_offset;
} BioStats;

/* A repair is retained with the program. The returned string pointers remain
   valid until bio_program_release(program). */
typedef struct {
    const char *file;
    uint32_t line;
    const char *kind;
    const char *detail;
} BioRepair;

/* Optional lists for one invocation. NULL pointers mean unavailable channels.
   For an available empty input or zero-capacity output, pass a non-NULL pointer.
   Counts/capacities may not exceed BIO_LIST_LIMIT. Input is read-only.
   Each READ copies the same input; the last WRITE supplies the output.
   On success output_count/output_written describe that WRITE (both zero if absent).
   On failure result, output data, output_count and output_written stay unchanged.
   Concurrent calls must use separate BioIO/output storage. */
typedef struct {
    const int32_t *input;
    size_t input_count;
    int32_t *output;
    size_t output_capacity;
    size_t output_count;
    int output_written;
    /* Host capability for the PRINT codon. NULL forbids printing: a program that
       reaches PRINT then fails with "PRINT requires a host print capability".
       The callback receives raw bytes (not NUL-terminated, not necessarily text)
       and returns nonzero on success; a zero return stops the run with an error.
       Printing charges one step plus one step per byte against the shared budget,
       and nothing is delivered when the budget check fails beforehand. */
    int (*print)(void *user, const unsigned char *bytes, size_t length);
    void *print_user;
} BioIO;

/* With repair disabled, source is strict. With repair enabled, unambiguous
   omitted '_' fields are restored and ambiguous codons become a safe runtime
   quarantine. The source file is never changed. */
BioProgram *bio_compile(const char *path, int repair, BioError *error);
/* Load and validate without allocating executable memory. Paths are UTF-8. */
BioProgram *bio_load(const char *path, int repair, BioError *error);
void bio_program_release(BioProgram *program);
BioStats bio_program_stats(const BioProgram *program);
/* Channel usage of live code: bit 1 = a reachable READ, bit 2 = a reachable
   WRITE. Lets hosts scaffold matching CSV tests or I/O plumbing. */
unsigned bio_program_channels(const BioProgram *program);
size_t bio_program_repair_count(const BioProgram *program);
int bio_program_repair_at(const BioProgram *program, size_t index, BioRepair *repair);
/* Return 1 on success; on failure return 0 and leave result unchanged.
   arguments may be NULL only when argument_count is zero.
   Each invocation has BIO_EXECUTION_LIMIT steps, shared by its nested calls.
   Owned locals are private to each call; initialization consumes one step per i32 cell.
   Internal PARAM calls borrow a checked prefix of the caller's array without copying. */
int bio_program_run(const BioProgram *program, const int32_t *arguments, size_t argument_count,
                    int32_t *result, BioError *error);
int bio_program_interpret(const BioProgram *program, const int32_t *arguments, size_t argument_count,
                          int32_t *result, BioError *error);
int bio_program_run_io(const BioProgram *program, const int32_t *arguments, size_t argument_count,
                       BioIO *io, int32_t *result, BioError *error);
int bio_program_interpret_io(const BioProgram *program, const int32_t *arguments, size_t argument_count,
                             BioIO *io, int32_t *result, BioError *error);
void bio_program_dump(const BioProgram *program, FILE *out);
int bio_program_write_code(const BioProgram *program, const char *path, BioError *error);

/* Runtime retains the initial program. Calls and reloads may overlap.
   Destroy only after all calls/reloads have returned. */
BioRuntime *bio_runtime_create(BioProgram *initial, BioError *error);
int bio_runtime_run(BioRuntime *runtime, const int32_t *arguments, size_t argument_count,
                    int32_t *result, BioError *error);
int bio_runtime_run_io(BioRuntime *runtime, const int32_t *arguments, size_t argument_count,
                       BioIO *io, int32_t *result, BioError *error);
int bio_runtime_reload(BioRuntime *runtime, const char *path, int repair, BioError *error);
/* Publish an already-compiled candidate after the caller's own verification
   (for example a test-suite gate). Same signature compatibility contract as
   bio_runtime_reload. On success the runtime retains its own reference; the
   caller keeps theirs and must release it. */
int bio_runtime_publish(BioRuntime *runtime, BioProgram *candidate, BioError *error);
void bio_runtime_destroy(BioRuntime *runtime);

#endif
