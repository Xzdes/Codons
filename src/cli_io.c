#include "internal.h"
#include "cli.h"
#include <ctype.h>
#include <inttypes.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LIST_FILE_LIMIT (1024u * 1024u)

int cli_read_list(const char *path, int32_t **values, size_t *count, BioError *e) {
    FILE *file = bio_fopen(path, L"rb");
    int32_t *data = NULL;
    size_t used = 0, bytes = 0;
    unsigned line_number = 0;
    LARGE_INTEGER size;
    HANDLE handle;
    int ok = 0;
    if (!file) { bio_error(e, "cannot open input list: %s", path); return 0; }
#define LIST_FAIL(message) do { bio_error(e, "%s:%u: %s", path, line_number, message); goto done; } while (0)
    handle = (HANDLE)_get_osfhandle(_fileno(file));
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &size)) {
        bio_error(e, "input must be a regular disk file: %s", path); goto done;
    }
    if (size.QuadPart > LIST_FILE_LIMIT) {
        bio_error(e, "%s: input list exceeds the 1 MiB file limit", path); goto done;
    }
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
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, NULL, 0))
            LIST_FAIL("input list must be valid UTF-8");
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
    if (count > BIO_LIST_LIMIT) { bio_error(e, "output list exceeds %u numbers", BIO_LIST_LIMIT); return 0; }
    text = malloc(count * 12u + 1u); /* -2147483648 plus LF, and one final NUL. */
    if (!text) { bio_error(e, "out of memory formatting output list"); return 0; }
    for (i = 0; i < count; ++i)
        used += (size_t)snprintf(text + used, count * 12u + 1u - used, "%" PRId32 "\n", values[i]);
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
