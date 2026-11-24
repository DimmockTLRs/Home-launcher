#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <SDL.h>
#include <ini.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <wchar.h>
#else
#include <unistd.h>
#endif
#include <launcher_config.h>
#include "launcher.h"
#include "config_save.h"
#include "util.h"

typedef struct {
    const char *key;
    const char *value;
    bool changed;
    bool found;
    size_t start;
    size_t end;
} SettingEdit;

typedef struct {
    const char *bytes;
    size_t size;
    size_t offset;
    size_t line_start;
    size_t first_chunk_length;
    const char *parser_buffer;
    size_t background_end;
    bool has_background_entry;
    SettingEdit *edits;
    size_t edit_count;
} ConfigScan;

typedef enum {
    EDIT_MODE,
    EDIT_LAYOUT,
    EDIT_DURATION,
    EDIT_TRANSITION,
    EDIT_IMAGE,
    EDIT_FOLDER,
    EDIT_COUNT
} EditIndex;

#ifdef _WIN32
typedef wchar_t PathChar;
#define BACKUP_SUFFIX L".bak"

static PathChar *native_path(const char *path)
{
    UINT encoding = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(encoding, flags, path, -1, NULL, 0);
    if (length == 0) {
        // Older command lines and configuration lookups may supply code-page paths.
        encoding = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(encoding, flags, path, -1, NULL, 0);
    }
    if (length == 0)
        return NULL;

    PathChar *wide = malloc((size_t)length * sizeof(*wide));
    if (wide != NULL && MultiByteToWideChar(encoding, flags, path, -1, wide, length) == 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static FILE *open_native(const PathChar *path)
{
    return _wfopen(path, L"rb");
}

static PathChar *path_with_suffix(const PathChar *path, const PathChar *suffix)
{
    size_t length = wcslen(path) + wcslen(suffix) + 1;
    PathChar *result = malloc(length * sizeof(*result));
    if (result != NULL) {
        wcscpy(result, path);
        wcscat(result, suffix);
    }
    return result;
}

static FILE *create_temp(const PathChar *path, PathChar **temp_path)
{
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        wchar_t suffix[64];
        swprintf(suffix, SDL_arraysize(suffix), L".tmp.%lu.%lu.%u",
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), attempt);
        PathChar *name = path_with_suffix(path, suffix);
        if (name == NULL)
            return NULL;
        int fd;
        errno_t error = _wsopen_s(&fd, name, _O_CREAT | _O_EXCL | _O_BINARY | _O_WRONLY,
            _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (error != 0) {
            free(name);
            if (error == EEXIST)
                continue;
            return NULL;
        }
        FILE *file = _fdopen(fd, "wb");
        if (file == NULL) {
            _close(fd);
            DeleteFileW(name);
            free(name);
            return NULL;
        }
        *temp_path = name;
        return file;
    }
    return NULL;
}

static void remove_temp(const PathChar *path)
{
    if (path != NULL)
        DeleteFileW(path);
}

static bool flush_file(FILE *file)
{
    return fflush(file) == 0 && _commit(_fileno(file)) == 0;
}
#else
typedef char PathChar;
#define BACKUP_SUFFIX ".bak"

static PathChar *native_path(const char *path)
{
    // Replace the target of a symlink rather than replacing the user's symlink.
    return realpath(path, NULL);
}

static FILE *open_native(const PathChar *path)
{
    return fopen(path, "rb");
}

static PathChar *path_with_suffix(const PathChar *path, const PathChar *suffix)
{
    size_t length = strlen(path) + strlen(suffix) + 1;
    PathChar *result = malloc(length);
    if (result != NULL) {
        strcpy(result, path);
        strcat(result, suffix);
    }
    return result;
}

static FILE *create_temp(const PathChar *path, PathChar **temp_path)
{
    char *name = path_with_suffix(path, ".tmp.XXXXXX");
    if (name == NULL)
        return NULL;
    int fd = mkstemp(name);
    if (fd < 0) {
        free(name);
        return NULL;
    }
    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        close(fd);
        unlink(name);
        free(name);
        return NULL;
    }
    *temp_path = name;
    return file;
}

static void remove_temp(const PathChar *path)
{
    if (path != NULL)
        unlink(path);
}

static bool flush_file(FILE *file)
{
    return fflush(file) == 0 && fsync(fileno(file)) == 0;
}
#endif

FILE *open_launcher_config(const char *path)
{
#ifdef _WIN32
    PathChar *name = native_path(path);
    if (name == NULL)
        return NULL;
    FILE *file = open_native(name);
    free(name);
    return file;
#else
    return open_native(path);
#endif
}

static char *read_config(FILE *file, size_t *size)
{
    if (fseek(file, 0, SEEK_END) != 0)
        return NULL;
    long length = ftell(file);
    if (length < 0 || (uintmax_t)length >= SIZE_MAX || fseek(file, 0, SEEK_SET) != 0)
        return NULL;

    *size = (size_t)length;
    char *bytes = malloc(*size + 1);
    if (bytes == NULL)
        return NULL;
    if (fread(bytes, 1, *size, file) != *size ||
        fgetc(file) != EOF || ferror(file) || memchr(bytes, '\0', *size) != NULL) {
        free(bytes);
        return NULL;
    }
    bytes[*size] = '\0';
    return bytes;
}

static bool config_matches(const PathChar *path, const char *bytes, size_t size)
{
    FILE *file = open_native(path);
    if (file == NULL)
        return false;
    char buffer[4096];
    size_t offset = 0;
    bool matches = true;
    while (offset < size) {
        size_t count = SDL_min(sizeof(buffer), size - offset);
        if (fread(buffer, 1, count, file) != count || memcmp(buffer, bytes + offset, count) != 0) {
            matches = false;
            break;
        }
        offset += count;
    }
    matches = matches && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return matches;
}

// Remember the start of each physical line even if inih reads it in several chunks.
static char *scan_line(char *buffer, int capacity, void *context)
{
    ConfigScan *scan = context;
    if (scan->offset == scan->size || capacity <= 1)
        return NULL;

    size_t count = SDL_min((size_t)capacity - 1, scan->size - scan->offset);
    const char *start = scan->bytes + scan->offset;
    const char *newline = memchr(start, '\n', count);
    if (newline != NULL)
        count = (size_t)(newline - start) + 1;
    if (scan->offset == 0 || start[-1] == '\n') {
        scan->line_start = scan->offset;
        scan->parser_buffer = buffer;
        scan->first_chunk_length = count;
    }

    memcpy(buffer, start, count);
    buffer[count] = '\0';
    scan->offset += count;
    return buffer;
}

static int locate_setting(void *context, const char *section, const char *name, const char *value)
{
    ConfigScan *scan = context;
    if (strcmp(section, "Background") != 0)
        return 1;

    const char *newline = strchr(scan->bytes + scan->line_start, '\n');
    scan->background_end = newline != NULL ? (size_t)(newline - scan->bytes) + 1 : scan->size;
    scan->has_background_entry = true;
    if (name == NULL || value == NULL)
        return 1;

    for (size_t i = 0; i < scan->edit_count; i++) {
        SettingEdit *edit = &scan->edits[i];
        if (!edit->changed || strcmp(edit->key, name) != 0)
            continue;

        // inih strips comments and whitespace inside its line buffer. The value's
        // position and length then identify the exact bytes we can replace.
        uintptr_t base = (uintptr_t)scan->parser_buffer;
        uintptr_t address = (uintptr_t)value;
        size_t length = strlen(value);
        edit->found = address >= base && address - base <= scan->first_chunk_length &&
            length <= scan->first_chunk_length - (size_t)(address - base);
        if (edit->found) {
            edit->start = scan->line_start + (size_t)(address - base);
            edit->end = edit->start + length;
            if (edit->value[0] == '\0') {
                // A blank continuation would be ignored and reveal its earlier
                // value. Add an explicit empty assignment after it instead.
                const char *line = scan->bytes + scan->line_start;
                size_t prefix = edit->start - scan->line_start;
                edit->found = memchr(line, '=', prefix) != NULL ||
                    memchr(line, ':', prefix) != NULL;
            }
        }
        break;
    }
    return 1;
}

static bool put_bytes(FILE *file, const char *data, size_t size)
{
    return fwrite(data, 1, size, file) == size;
}

static bool add_missing_settings(FILE *file, const ConfigScan *scan)
{
    const char *newline = strchr(scan->bytes, '\n');
    const char *line_break = newline != NULL && newline > scan->bytes && newline[-1] == '\r' ?
        "\r\n" : "\n";
    size_t at = scan->has_background_entry ? scan->background_end : scan->size;
    if (at > 0 && scan->bytes[at - 1] != '\n' && fputs(line_break, file) == EOF)
        return false;
    if (!scan->has_background_entry && fprintf(file, "[Background]%s", line_break) < 0)
        return false;
    for (size_t i = 0; i < scan->edit_count; i++) {
        const SettingEdit *edit = &scan->edits[i];
        if (edit->changed && !edit->found && fprintf(file, "%s=%s%s", edit->key, edit->value, line_break) < 0)
            return false;
    }
    return true;
}

static bool write_updated_config(FILE *file, ConfigScan *scan)
{
    SettingEdit *ordered[EDIT_COUNT];
    size_t count = 0;
    bool insert = false;
    for (size_t i = 0; i < scan->edit_count; i++) {
        SettingEdit *edit = &scan->edits[i];
        if (!edit->changed)
            continue;
        if (!edit->found) {
            insert = true;
            continue;
        }
        // The effective settings may appear in any order in the INI.
        size_t slot = count++;
        while (slot > 0 && ordered[slot - 1]->start > edit->start) {
            ordered[slot] = ordered[slot - 1];
            slot--;
        }
        ordered[slot] = edit;
    }

    size_t insertion = scan->has_background_entry ? scan->background_end : scan->size;
    size_t offset = 0;
    for (size_t i = 0; i <= count; i++) {
        size_t next = i < count ? ordered[i]->start : scan->size;
        if (insert && (insertion < next || i == count)) {
            if (insertion < offset || !put_bytes(file, scan->bytes + offset, insertion - offset) ||
                !add_missing_settings(file, scan))
                return false;
            insert = false;
            offset = insertion;
        }
        if (next < offset || !put_bytes(file, scan->bytes + offset, next - offset))
            return false;
        if (i < count) {
            if (fputs(ordered[i]->value, file) == EOF)
                return false;
            offset = ordered[i]->end;
        }
    }
    return true;
}

static const char *configured_path(const char *path)
{
    return path != NULL ? path : "";
}

static char *encode_config_path(const char *path)
{
    const char *value = configured_path(path);
    size_t length = strlen(value);
    // A line break would introduce a second INI record.
    if (strpbrk(value, "\r\n") != NULL || length > SIZE_MAX - 3)
        return NULL;
    if (length == 0)
        return SDL_strdup("");

    char *quoted = SDL_malloc(length + 3);
    if (quoted != NULL) {
        quoted[0] = '"';
        memcpy(quoted + 1, value, length);
        quoted[length + 1] = '"';
        quoted[length + 2] = '\0';
    }
    return quoted;
}

typedef struct {
    const char *image;
    const char *folder;
    bool image_matches;
    bool folder_matches;
} PathVerification;

static int verify_path_value(void *context, const char *section, const char *key, const char *value)
{
    PathVerification *paths = context;
    if (strcmp(section, "Background") != 0 || key == NULL || value == NULL)
        return 1;

    const char *expected;
    bool *matches;
    if (paths->image != NULL && strcmp(key, SETTING_BACKGROUND_IMAGE) == 0) {
        expected = paths->image;
        matches = &paths->image_matches;
    }
    else if (paths->folder != NULL && strcmp(key, SETTING_SLIDESHOW_DIRECTORY) == 0) {
        expected = paths->folder;
        matches = &paths->folder_matches;
    }
    else
        return 1;

    char *path = SDL_strdup(value);
    *matches = false;
    if (path != NULL) {
        clean_path(path);
        *matches = strcmp(path, expected) == 0;
        SDL_free(path);
    }
    return 1;
}

static bool verify_config_paths(const PathChar *path, const BackgroundSettings *settings,
    bool image_changed, bool folder_changed)
{
    PathVerification paths = {
        .image = image_changed ? configured_path(settings->image_path) : NULL,
        .folder = folder_changed ? configured_path(settings->folder_path) : NULL
    };
    FILE *file = open_native(path);
    if (file == NULL)
        return false;
    int result = ini_parse_file(file, verify_path_value, &paths);
    bool readable = !ferror(file);
    fclose(file);
    // The launcher ignores positive inih errors for unrelated legacy lines.
    return readable && result >= 0 &&
        (!image_changed || paths.image_matches) && (!folder_changed || paths.folder_matches);
}

bool save_background_config(const char *path, const BackgroundSettings *settings,
    const BackgroundSettings *previous, char *error, size_t error_size)
{
    char duration[20], transition[20];
    SDL_snprintf(duration, sizeof(duration), "%u", (unsigned)(settings->image_duration / 1000));
    SDL_snprintf(transition, sizeof(transition), "%u.%03u",
        (unsigned)(settings->transition_time / 1000), (unsigned)(settings->transition_time % 1000));
    SettingEdit edits[EDIT_COUNT] = {
        { .key = SETTING_BACKGROUND_MODE,
            .value = get_mode_setting(MODE_SETTING_BACKGROUND, settings->mode),
            .changed = settings->mode != previous->mode },
        { .key = SETTING_COLLAGE_LAYOUT,
            .value = get_mode_setting(MODE_SETTING_COLLAGE_LAYOUT, settings->layout),
            .changed = settings->layout != previous->layout },
        { .key = SETTING_SLIDESHOW_IMAGE_DURATION, .value = duration,
            .changed = settings->image_duration != previous->image_duration },
        { .key = SETTING_SLIDESHOW_TRANSITION_TIME, .value = transition,
            .changed = settings->transition_time != previous->transition_time },
        { .key = SETTING_BACKGROUND_IMAGE,
            .changed = strcmp(configured_path(settings->image_path), configured_path(previous->image_path)) != 0 },
        { .key = SETTING_SLIDESHOW_DIRECTORY,
            .changed = strcmp(configured_path(settings->folder_path), configured_path(previous->folder_path)) != 0 }
    };

    bool changed = false;
    for (size_t i = 0; i < SDL_arraysize(edits); i++)
        changed |= edits[i].changed;
    if (!changed)
        return true;

    bool saved = false;
    PathChar *name = NULL;
    PathChar *backup = NULL;
    PathChar *temp = NULL;
#ifndef _WIN32
    PathChar *backup_temp = NULL;
    struct stat original_stat;
#endif
    char *bytes = NULL;
    size_t size = 0;
    char *encoded_image = edits[EDIT_IMAGE].changed ? encode_config_path(settings->image_path) : NULL;
    char *encoded_folder = edits[EDIT_FOLDER].changed ? encode_config_path(settings->folder_path) : NULL;
    const char *path_error = "The selected photo or folder path could not be saved in the INI file.";
    const char *failure = path_error;
    if ((edits[EDIT_IMAGE].changed && encoded_image == NULL) ||
        (edits[EDIT_FOLDER].changed && encoded_folder == NULL))
        goto cleanup;
    edits[EDIT_IMAGE].value = encoded_image;
    edits[EDIT_FOLDER].value = encoded_folder;

    failure = "Could not read the existing configuration.";
    name = native_path(path);
    FILE *original = name != NULL ? open_native(name) : NULL;
    if (original == NULL)
        goto cleanup;
#ifndef _WIN32
    if (fstat(fileno(original), &original_stat) != 0 || !S_ISREG(original_stat.st_mode)) {
        fclose(original);
        goto cleanup;
    }
#endif
    bytes = read_config(original, &size);
    fclose(original);
    if (bytes == NULL)
        goto cleanup;

    ConfigScan scan = { .bytes = bytes, .size = size, .edits = edits, .edit_count = SDL_arraysize(edits) };
    if (ini_parse_stream(scan_line, &scan, locate_setting, &scan) < 0)
        goto cleanup;

    failure = "Could not write a temporary configuration in the original folder.";
    backup = path_with_suffix(name, BACKUP_SUFFIX);
    FILE *replacement = backup != NULL ? create_temp(name, &temp) : NULL;
    if (replacement == NULL)
        goto cleanup;
    bool wrote_replacement = write_updated_config(replacement, &scan);
#ifndef _WIN32
    wrote_replacement = wrote_replacement &&
        fchmod(fileno(replacement), original_stat.st_mode & 0777) == 0;
#endif
    wrote_replacement = wrote_replacement && flush_file(replacement);
    if (fclose(replacement) != 0 || !wrote_replacement)
        goto cleanup;

    // Quoted paths are still subject to inih's inline comments and line-size
    // limit. Test the full file before a backup or replace can affect the user.
    failure = path_error;
    if ((edits[EDIT_IMAGE].changed || edits[EDIT_FOLDER].changed) &&
        !verify_config_paths(temp, settings, edits[EDIT_IMAGE].changed, edits[EDIT_FOLDER].changed))
        goto cleanup;

#ifndef _WIN32
    failure = "Could not create a configuration backup in the original folder.";
    FILE *backup_file = create_temp(name, &backup_temp);
    if (backup_file == NULL)
        goto cleanup;
    bool wrote_backup = put_bytes(backup_file, bytes, size) &&
        fchmod(fileno(backup_file), original_stat.st_mode & 0777) == 0 && flush_file(backup_file);
    if (fclose(backup_file) != 0 || !wrote_backup)
        goto cleanup;
#endif

    failure = "The configuration changed while saving. Please try again.";
    if (!config_matches(name, bytes, size))
        goto cleanup;

    failure = "Could not replace the configuration. Check folder permissions.";
#ifdef _WIN32
    saved = ReplaceFileW(name, temp, backup, 0, NULL, NULL) != 0;
    if (!saved && GetLastError() == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
        // ReplaceFile may have moved the original to the backup on this error.
        // Restore its name when no intervening writer has recreated that path.
        if (!MoveFileExW(backup, name, MOVEFILE_WRITE_THROUGH))
            failure = "Could not restore the original configuration name. Check its .bak file.";
    }
#else
    saved = rename(backup_temp, backup) == 0 && rename(temp, name) == 0;
#endif

cleanup:
    if (error != NULL && error_size > 0)
        SDL_snprintf(error, error_size, "%s", saved ? "" : failure);
    remove_temp(temp);
#ifndef _WIN32
    remove_temp(backup_temp);
    free(backup_temp);
#endif
    free(bytes);
    free(temp);
    free(backup);
    free(name);
    SDL_free(encoded_image);
    SDL_free(encoded_folder);
    return saved;
}
