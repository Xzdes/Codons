#include "internal.h"
#include "cli.h"
#include <ctype.h>
#include <inttypes.h>
#ifdef _WIN32
#include <io.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LIST_FILE_LIMIT (1024u * 1024u)

int cli_print_stdout(void *user, const unsigned char *bytes, size_t length) {
    (void)user;
    return fwrite(bytes, 1, length, stdout) == length;
}

int cli_print_discard(void *user, const unsigned char *bytes, size_t length) {
    (void)user; (void)bytes; (void)length;
    return 1;
}

int cli_print_capture(void *user, const unsigned char *bytes, size_t length) {
    CliPrint *buffer = user;
    size_t capacity;
    unsigned char *grown;
    /* The step budget already bounds the total; this is a memory backstop. */
    if (length > (size_t)BIO_EXECUTION_LIMIT || buffer->length > (size_t)BIO_EXECUTION_LIMIT - length)
        return 0;
    if (buffer->length + length > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < buffer->length + length) capacity *= 2;
        grown = realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown; buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
    return 1;
}

void cli_json_bytes(const unsigned char *bytes, size_t length) {
    size_t i;
    putchar('"');
    for (i = 0; i < length; ++i) {
        unsigned c = bytes[i];
        if (c == '"' || c == '\\') { putchar('\\'); putchar((int)c); }
        else if (c < 32) printf("\\u%04x", c);
        else putchar((int)c);
    }
    putchar('"');
}

int cli_read_list(const char *path, int32_t **values, size_t *count, BioError *e) {
    FILE *file = bio_fopen(path, "rb");
    int32_t *data = NULL;
    size_t used = 0, bytes = 0;
    unsigned line_number = 0;
    int ok = 0;
    if (!file) { bio_error(e, "cannot open input list: %s", path); return 0; }
#define LIST_FAIL(message) do { bio_error(e, "%s:%u: %s", path, line_number, message); goto done; } while (0)
#ifdef _WIN32
    {
        LARGE_INTEGER size;
        HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
        if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &size)) {
            bio_error(e, "input must be a regular disk file: %s", path); goto done;
        }
        if (size.QuadPart > LIST_FILE_LIMIT) {
            bio_error(e, "%s: input list exceeds the 1 MiB file limit", path); goto done;
        }
    }
#else
    {
        struct stat st;
        if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode)) {
            bio_error(e, "input must be a regular disk file: %s", path); goto done;
        }
        if (st.st_size > (off_t)LIST_FILE_LIMIT) {
            bio_error(e, "%s: input list exceeds the 1 MiB file limit", path); goto done;
        }
    }
#endif
    data = malloc(BIO_LIST_LIMIT * sizeof(*data));
    if (!data) { bio_error(e, "out of memory reading input list"); goto done; }
    for (;;) {
        char line[BIO_LINE_SIZE], *start, *end;
        size_t length = 0;
        int c;
        ++line_number;
        while ((c = fgetc(file)) != EOF && c != '\n') {
            if (++bytes > LIST_FILE_LIMIT) LIST_FAIL("input list exceeds the 1 MiB file limit");
            if (!c) LIST_FAIL("input list must be UTF-8 without NUL bytes");
        if (length + 1 == sizeof(line)) LIST_FAIL("input line exceeds 511 bytes");
        line[length++] = (char)c;
    }
    if (c == '\n' && ++bytes > LIST_FILE_LIMIT) LIST_FAIL("input list exceeds the 1 MiB file limit");
    if (ferror(file)) LIST_FAIL("cannot read input list");
    if (c == EOF && !length) break;
    line[length] = '\0';
#ifdef _WIN32
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, NULL, 0))
            LIST_FAIL("input list must be valid UTF-8");
#else
        if (!bio_utf8_valid(line)) LIST_FAIL("input list must be valid UTF-8");
#endif
        start = line;
        if (line_number == 1 && length >= 3 && (unsigned char)line[0] == 0xef &&
            (unsigned char)line[1] == 0xbb && (unsigned char)line[2] == 0xbf) start += 3;
        while (isspace((unsigned char)*start)) ++start;
        end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1])) --end;
        *end = '\0';
        if (!*start) continue;
        if (used == BIO_LIST_LIMIT) LIST_FAIL("input list exceeds 65536 numbers");
        if (!cli_parse_i32(start, &data[used])) LIST_FAIL("expected one decimal i32 number per nonblank line");
        ++used;
    }
    ok = 1;
done:
    if (fclose(file) && ok) { bio_error(e, "cannot finish reading input list: %s", path); ok = 0; }
    if (ok) { *values = data; *count = used; }
    else free(data);
    return ok;
#undef LIST_FAIL
}

static char *format_list(const int32_t *values, size_t count, size_t *used, BioError *e) {
    char *text;
    size_t i, n = 0;
    if (count > BIO_LIST_LIMIT) { bio_error(e, "output list exceeds %u numbers", BIO_LIST_LIMIT); return NULL; }
    text = malloc(count * 12u + 1u); /* -2147483648 plus LF, and one final NUL. */
    if (!text) { bio_error(e, "out of memory formatting output list"); return NULL; }
    for (i = 0; i < count; ++i)
        n += (size_t)snprintf(text + n, count * 12u + 1u - n, "%" PRId32 "\n", values[i]);
    *used = n;
    return text;
}

#ifdef _WIN32
int cli_write_list(const char *path, const int32_t *values, size_t count, BioError *e) {
    wchar_t wide[BIO_PATH_SIZE], full[BIO_PATH_SIZE], temporary[BIO_PATH_SIZE + 80];
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD length, written, error = 0;
    char *text;
    size_t used = 0, i;
    unsigned attempt;
    static volatile LONG serial;
    if (!bio_wide_path(path, wide, BIO_PATH_SIZE) ||
        (wide[0] == L'\\' && wide[1] == L'\\' && (wide[2] == L'?' || wide[2] == L'.') && wide[3] == L'\\')) {
        bio_error(e, "output requires a normal UTF-8 file path: %s", path); return 0;
    }
    length = GetFullPathNameW(wide, BIO_PATH_SIZE, full, NULL);
    if (!length || length >= BIO_PATH_SIZE) { bio_error(e, "cannot resolve output path: %s", path); return 0; }
    /* Alternate streams are not separate result files and could modify an existing file. */
    for (i = 0; full[i]; ++i) if (full[i] == L':' && i != 1) {
        bio_error(e, "output must be a file, not an alternate data stream: %s", path); return 0;
    }
    if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
        bio_error(e, "output already exists; choose a new path: %s", path); return 0;
    }
    text = format_list(values, count, &used, e);
    if (!text) return 0;
    for (attempt = 0; attempt < 64; ++attempt) {
        swprintf(temporary, sizeof(temporary) / sizeof(*temporary), L"%ls.codons-%lu-%lu.tmp", full,
                 GetCurrentProcessId(), (unsigned long)(DWORD)InterlockedIncrement(&serial));
        file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        free(text); bio_error(e, "cannot prepare output %s (Windows error %lu)", path, error); return 0;
    }
    error = 0;
    if (GetFileType(file) != FILE_TYPE_DISK) error = ERROR_INVALID_NAME;
    else if (!WriteFile(file, text, (DWORD)used, &written, NULL)) error = GetLastError();
    else if (written != used) error = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(file)) error = GetLastError();
    if (!CloseHandle(file) && !error) error = GetLastError();
    free(text);
    /* Same-directory rename publishes the complete file and never replaces a destination. */
    if (!error && !MoveFileExW(temporary, full, MOVEFILE_WRITE_THROUGH)) error = GetLastError();
    if (error) {
        DeleteFileW(temporary);
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            bio_error(e, "output already exists; choose a new path: %s", path);
        else bio_error(e, "cannot save output %s (Windows error %lu)", path, error);
        return 0;
    }
    return 1;
}
#else
int cli_write_list(const char *path, const int32_t *values, size_t count, BioError *e) {
    char temporary[BIO_PATH_SIZE + 80];
    char *text;
    size_t used = 0;
    unsigned attempt;
    static volatile long serial;
    int fd = -1, saved = 0;
    struct stat st;
    if (!bio_utf8_valid(path) || strlen(path) >= BIO_PATH_SIZE) {
        bio_error(e, "output requires a normal UTF-8 file path: %s", path); return 0;
    }
    if (!lstat(path, &st)) {
        bio_error(e, "output already exists; choose a new path: %s", path); return 0;
    }
    text = format_list(values, count, &used, e);
    if (!text) return 0;
    for (attempt = 0; attempt < 64; ++attempt) {
        snprintf(temporary, sizeof(temporary), "%s.codons-%ld-%ld.tmp", path,
                 (long)getpid(), ++serial);
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) break;
        if (errno != EEXIST) break;
    }
    if (fd < 0) { free(text); bio_error(e, "cannot prepare output %s", path); return 0; }
    if (write(fd, text, used) != (ssize_t)used || fsync(fd)) saved = errno ? errno : EIO;
    if (close(fd) && !saved) saved = errno;
    free(text);
    /* link() publishes atomically and FAILS when the destination exists. */
    if (!saved && link(temporary, path)) saved = errno;
    if (unlink(temporary) && !saved) saved = errno;
    if (saved) {
        if (saved == EEXIST) bio_error(e, "output already exists; choose a new path: %s", path);
        else bio_error(e, "cannot save output %s (error %d)", path, saved);
        return 0;
    }
    return 1;
}
#endif
