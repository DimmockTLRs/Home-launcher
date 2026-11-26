#include <stdbool.h>
#include <ctype.h>
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
    bool verified;
    size_t start;
    size_t end;
} SettingEdit;

typedef struct {
    const char *name;
    SettingEdit *edits;
    size_t count;
} SettingsSection;

typedef struct {
    const char *bytes;
    size_t size;
    size_t offset;
    size_t line_start;
    size_t first_chunk_length;
    const char *parser_buffer;
    size_t section_end;
    bool has_section_entry;
    bool tail_resets_key;
    const SettingsSection *section;
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

static bool scan_value_range(const ConfigScan *scan, const char *value, size_t *start, size_t *end)
{
    // inih strips comments and whitespace inside its line buffer. The value's
    // position and length then identify the exact bytes we can replace.
    uintptr_t base = (uintptr_t)scan->parser_buffer;
    uintptr_t address = (uintptr_t)value;
    size_t length = strlen(value);
    if (address < base || address - base > scan->first_chunk_length ||
        length > scan->first_chunk_length - (size_t)(address - base))
        return false;
    *start = scan->line_start + (size_t)(address - base);
    *end = *start + length;
    return true;
}

static size_t scan_line_end(const ConfigScan *scan)
{
    const char *newline = strchr(scan->bytes + scan->line_start, '\n');
    return newline != NULL ? (size_t)(newline - scan->bytes) + 1 : scan->size;
}

static int locate_setting(void *context, const char *section, const char *name, const char *value)
{
    ConfigScan *scan = context;
    if (strcmp(section, scan->section->name) != 0)
        return 1;

    scan->section_end = scan_line_end(scan);
    scan->has_section_entry = true;
    scan->tail_resets_key = name != NULL && name[0] == '\0';
    if (name == NULL || value == NULL)
        return 1;

    for (size_t i = 0; i < scan->section->count; i++) {
        SettingEdit *edit = &scan->section->edits[i];
        if (!edit->changed || strcmp(edit->key, name) != 0)
            continue;

        edit->found = scan_value_range(scan, value, &edit->start, &edit->end);
        if (edit->found) {
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

static const char *config_line_break(const char *bytes)
{
    const char *newline = strchr(bytes, '\n');
    return newline != NULL && newline > bytes && newline[-1] == '\r' ? "\r\n" : "\n";
}

static bool add_missing_settings(FILE *file, const ConfigScan *scan)
{
    const char *line_break = config_line_break(scan->bytes);
    size_t at = scan->has_section_entry ? scan->section_end : scan->size;
    if (at > 0 && scan->bytes[at - 1] != '\n' && fputs(line_break, file) == EOF)
        return false;
    if (!scan->has_section_entry && fprintf(file, "[%s]%s", scan->section->name, line_break) < 0)
        return false;
    for (size_t i = 0; i < scan->section->count; i++) {
        const SettingEdit *edit = &scan->section->edits[i];
        if (edit->changed && !edit->found && fprintf(file, "%s=%s%s", edit->key, edit->value, line_break) < 0)
            return false;
    }
    // Preserve whether inih treats the following indented lines as continuations.
    return at == scan->size || !scan->tail_resets_key || fprintf(file, "=%s", line_break) >= 0;
}

static bool write_updated_config(FILE *file, ConfigScan *scan)
{
    // Background has the largest set of edits we apply in one pass.
    SettingEdit *ordered[EDIT_COUNT];
    if (scan->section->count > SDL_arraysize(ordered))
        return false;
    size_t count = 0;
    bool insert = false;
    for (size_t i = 0; i < scan->section->count; i++) {
        SettingEdit *edit = &scan->section->edits[i];
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

    size_t insertion = scan->has_section_entry ? scan->section_end : scan->size;
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

typedef struct {
    const char *(*write)(FILE *file, const char *bytes, size_t size, void *context);
    bool (*verify)(FILE *file, void *context);
    const char *verification_error;
} ConfigEdit;

static bool update_config_file(const char *path, const ConfigEdit *edit, void *context,
    char *error, size_t error_size)
{
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
    const char *failure = "Could not read the existing configuration.";
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

    failure = "Could not write a temporary configuration in the original folder.";
    backup = path_with_suffix(name, BACKUP_SUFFIX);
    FILE *replacement = backup != NULL ? create_temp(name, &temp) : NULL;
    if (replacement == NULL)
        goto cleanup;
    const char *write_error = edit->write(replacement, bytes, size, context);
    bool wrote_replacement = write_error == NULL;
#ifndef _WIN32
    wrote_replacement = wrote_replacement &&
        fchmod(fileno(replacement), original_stat.st_mode & 0777) == 0;
#endif
    wrote_replacement = wrote_replacement && flush_file(replacement);
    if (write_error != NULL)
        failure = write_error;
    if (fclose(replacement) != 0 || !wrote_replacement)
        goto cleanup;

    if (edit->verify != NULL) {
        failure = edit->verification_error;
        FILE *candidate = open_native(temp);
        if (candidate == NULL)
            goto cleanup;
        bool verified = edit->verify(candidate, context) && !ferror(candidate);
        fclose(candidate);
        if (!verified)
            goto cleanup;
    }

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
    return saved;
}

typedef struct {
    SettingsSection section;
    PathVerification paths;
} BackgroundEdit;

static const char *write_section_edit(FILE *file, const char *bytes, size_t size, void *context)
{
    ConfigScan scan = { .bytes = bytes, .size = size, .section = context };
    if (ini_parse_stream(scan_line, &scan, locate_setting, &scan) < 0)
        return "Could not read the existing configuration.";
    if (!write_updated_config(file, &scan))
        return "Could not write the configuration settings.";
    return NULL;
}

static const char *write_background_edit(FILE *file, const char *bytes, size_t size, void *context)
{
    BackgroundEdit *edit = context;
    return write_section_edit(file, bytes, size, &edit->section);
}

static bool verify_background_edit(FILE *file, void *context)
{
    BackgroundEdit *edit = context;
    PathVerification *paths = &edit->paths;
    // Quoted paths still follow inih's inline comment and line length rules.
    return ini_parse_file(file, verify_path_value, paths) >= 0 &&
        (paths->image == NULL || paths->image_matches) && (paths->folder == NULL || paths->folder_matches);
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

    bool image_changed = edits[EDIT_IMAGE].changed, folder_changed = edits[EDIT_FOLDER].changed;
    char *image = image_changed ? encode_config_path(settings->image_path) : NULL;
    char *folder = folder_changed ? encode_config_path(settings->folder_path) : NULL;
    const char *path_error = "The selected photo or folder path could not be saved in the INI file.";
    bool saved = false;
    if ((image_changed && image == NULL) || (folder_changed && folder == NULL)) {
        if (error != NULL && error_size > 0)
            SDL_snprintf(error, error_size, "%s", path_error);
    } else {
        edits[EDIT_IMAGE].value = image;
        edits[EDIT_FOLDER].value = folder;
        BackgroundEdit background = {
            .section = { .name = "Background", .edits = edits, .count = SDL_arraysize(edits) },
            .paths = {
                .image = image_changed ? configured_path(settings->image_path) : NULL,
                .folder = folder_changed ? configured_path(settings->folder_path) : NULL
            }
        };
        const ConfigEdit edit = {
            .write = write_background_edit,
            .verify = image_changed || folder_changed ? verify_background_edit : NULL,
            .verification_error = path_error
        };
        saved = update_config_file(path, &edit, &background, error, error_size);
    }
    SDL_free(image);
    SDL_free(folder);
    return saved;
}

static int verify_setting_value(void *context, const char *section, const char *name, const char *value)
{
    SettingsSection *settings = context;
    if (strcmp(section, settings->name) != 0 || name == NULL || value == NULL)
        return 1;

    for (size_t i = 0; i < settings->count; i++) {
        SettingEdit *edit = &settings->edits[i];
        if (edit->changed && strcmp(name, edit->key) == 0) {
            edit->verified = strcmp(value, edit->value) == 0;
            break;
        }
    }
    return 1;
}

static bool verify_section_edit(FILE *file, void *context)
{
    SettingsSection *settings = context;
    if (ini_parse_file(file, verify_setting_value, settings) < 0)
        return false;
    for (size_t i = 0; i < settings->count; i++) {
        const SettingEdit *edit = &settings->edits[i];
        if (edit->changed && !edit->verified)
            return false;
    }
    return true;
}

bool save_appearance_config(const char *path, bool tv_layout, const AppearanceSettings *after,
    const AppearanceSettings *before, char *error, size_t error_size)
{
    bool row_changed = after->row_position != before->row_position;
    bool scale_changed = tv_layout && after->card_scale != before->card_scale;
    bool opacity_changed = tv_layout && after->card_opacity != before->card_opacity;
    if (!row_changed && !scale_changed && !opacity_changed) {
        if (error != NULL && error_size > 0)
            error[0] = '\0';
        return true;
    }

    bool auto_row = tv_layout && after->row_position == ROW_POSITION_AUTO;
    int row_min = tv_layout ? 0 : CLASSIC_ROW_MIN;
    int row_max = tv_layout ? 100 : CLASSIC_ROW_MAX;
    if ((row_changed && !auto_row && (after->row_position < row_min || after->row_position > row_max)) ||
        (scale_changed && (after->card_scale < TV_CARD_SCALE_MIN || after->card_scale > TV_CARD_SCALE_MAX)) ||
        (opacity_changed && (after->card_opacity < 0 || after->card_opacity > 100))) {
        if (error != NULL && error_size > 0)
            SDL_snprintf(error, error_size, "The appearance settings are outside the supported range.");
        return false;
    }

    char row[16], scale[16], opacity[16];
    SDL_snprintf(row, sizeof(row), "%d%%", after->row_position);
    SDL_snprintf(scale, sizeof(scale), "%d%%", after->card_scale);
    SDL_snprintf(opacity, sizeof(opacity), "%d%%", after->card_opacity);
    SettingEdit edits[] = {
        { .key = tv_layout ? SETTING_TV_ROW_POSITION : SETTING_VCENTER,
            .value = auto_row ? "Auto" : row, .changed = row_changed },
        { .key = SETTING_TV_CARD_SCALE, .value = scale, .changed = scale_changed },
        { .key = SETTING_TV_CARD_OPACITY, .value = opacity, .changed = opacity_changed }
    };
    SettingsSection section = { .name = "Layout", .edits = edits, .count = SDL_arraysize(edits) };
    const ConfigEdit edit = {
        .write = write_section_edit,
        .verify = verify_section_edit,
        .verification_error = "Could not verify the saved appearance settings."
    };
    return update_config_file(path, &edit, &section, error, error_size);
}

typedef struct {
    const char *name;
    const Entry *entries;
    const Entry *previous;
} MenuEdit;

typedef struct {
    const Entry *original;
    char *continuation_key;
    size_t start;
    size_t key_start;
    size_t value_start;
    size_t value_end;
    size_t end;
    size_t destination;
    bool empty_key;
    bool has_continuation;
} MenuRecord;

typedef struct {
    const Entry *entry;
    MenuRecord *source;
    bool unchanged;
} MenuTarget;

typedef struct {
    ConfigScan input;
    const char *name;
    const Entry *remaining;
    const char *failure;
    MenuRecord *records;
    size_t count;
    size_t capacity;
    size_t last_key;
    size_t menu_end;
    size_t next_key;
    bool tail_resets_key;
} MenuScan;

static const char *const menu_conflict = "The menu changed on disk. Restart the launcher before editing it.";
static const char *const menu_encoding_error = "An app's name, icon path or command could not be saved in the INI format.";

static bool same_entry(const Entry *first, const Entry *second)
{
    return first->title != NULL && second->title != NULL && first->icon_path != NULL &&
        second->icon_path != NULL && first->cmd != NULL && second->cmd != NULL &&
        strcmp(first->title, second->title) == 0 && strcmp(first->icon_path, second->icon_path) == 0 &&
        strcmp(first->cmd, second->cmd) == 0;
}

static bool valid_entry(const Entry *entry)
{
    return entry->title != NULL && entry->title[0] != '\0' && strpbrk(entry->title, ";\r\n") == NULL &&
        entry->icon_path != NULL && entry->icon_path[0] != '\0' && strpbrk(entry->icon_path, ";\r\n") == NULL &&
        entry->cmd != NULL && entry->cmd[0] != '\0' && strpbrk(entry->cmd, "\r\n") == NULL;
}

static MenuRecord *add_menu_record(MenuScan *scan)
{
    if (scan->count == scan->capacity) {
        size_t capacity = scan->capacity == 0 ? 8 : scan->capacity * 2;
        if (capacity < scan->capacity || capacity > SIZE_MAX / sizeof(*scan->records))
            return NULL;
        MenuRecord *records = SDL_realloc(scan->records, capacity * sizeof(*records));
        if (records == NULL)
            return NULL;
        scan->records = records;
        scan->capacity = capacity;
    }
    return &scan->records[scan->count++];
}

static int scan_menu_entry(void *context, const char *section, const char *key, const char *value)
{
    MenuScan *scan = context;
    if (scan->failure != NULL || strcmp(section, scan->name) != 0 || key == NULL || value == NULL) {
        scan->last_key = SIZE_MAX;
        return 1;
    }

    size_t start = scan->input.line_start;
    size_t end = scan_line_end(&scan->input);
    scan->menu_end = end;
    size_t value_start, value_end;
    if (!scan_value_range(&scan->input, value, &value_start, &value_end)) {
        scan->failure = "Could not locate the menu entries in the INI file.";
        return 1;
    }

    size_t key_start = start;
    while (key_start < value_start && isspace((unsigned char)scan->input.bytes[key_start]))
        key_start++;
    bool continuation = key_start == value_start;
    scan->tail_resets_key = !continuation && key[0] == '\0';
    if (!continuation) {
        scan->last_key = SIZE_MAX;
        if (strncmp(key, "Entry", 5) == 0 && key[5] >= '1' && key[5] <= '9') {
            char *suffix;
            unsigned long long number = strtoull(key + 5, &suffix, 10);
            if (*suffix == '\0' && number < SIZE_MAX / 2 && number >= scan->next_key)
                scan->next_key = (size_t)number + 1;
        }
    } else if (scan->last_key != SIZE_MAX) {
        scan->records[scan->last_key].has_continuation = true;
    }

    Entry parsed;
    if (!parse_menu_entry(value, &parsed))
        return 1;
    bool matches = scan->remaining != NULL && same_entry(&parsed, scan->remaining);
    clear_menu_entry(&parsed);
    if (!matches) {
        scan->failure = menu_conflict;
        return 1;
    }

    MenuRecord *record = add_menu_record(scan);
    if (record == NULL) {
        scan->failure = "Could not allocate memory for the menu configuration.";
        return 1;
    }
    *record = (MenuRecord){
        .original = scan->remaining, .start = start, .key_start = key_start, .end = end,
        .value_start = value_start, .value_end = value_end, .destination = SIZE_MAX,
        .empty_key = scan->tail_resets_key,
        .continuation_key = continuation ? SDL_strdup(key) : NULL
    };
    if (continuation && record->continuation_key == NULL)
        scan->failure = "Could not allocate memory for the menu configuration.";
    if (!continuation)
        scan->last_key = scan->count - 1;
    scan->remaining = scan->remaining->next;
    return 1;
}

static MenuTarget *match_menu_records(MenuScan *scan, const Entry *entries, size_t count)
{
    if (count == 0 || count > SIZE_MAX / sizeof(MenuTarget))
        return NULL;
    MenuTarget *targets = SDL_calloc(count, sizeof(*targets));
    if (targets == NULL)
        return NULL;

    // Match unchanged entries first, including repeated entries, so we can use
    // their original keys, quoting, and comments even if the user moves them.
    size_t index = 0;
    for (const Entry *entry = entries; entry != NULL; entry = entry->next, index++) {
        targets[index].entry = entry;
        for (size_t i = 0; i < scan->count; i++) {
            MenuRecord *record = &scan->records[i];
            if (record->destination == SIZE_MAX && same_entry(entry, record->original)) {
                record->destination = index;
                targets[index].source = record;
                targets[index].unchanged = true;
                break;
            }
        }
    }

    // Use the remaining INI records for edits before creating new assignment keys.
    size_t next = 0;
    for (size_t i = 0; i < count; i++) {
        if (targets[i].source != NULL)
            continue;
        while (next < scan->count && scan->records[next].destination != SIZE_MAX)
            next++;
        if (next == scan->count)
            break;
        MenuRecord *record = &scan->records[next++];
        record->destination = i;
        targets[i].source = record;
    }
    return targets;
}

static bool write_menu_target(FILE *file, MenuScan *scan, const MenuTarget *target,
    bool moved, bool dedent)
{
    const char *bytes = scan->input.bytes;
    const MenuRecord *record = target->source;
    if (record == NULL || (moved && record->empty_key)) {
        if (fprintf(file, "Entry%zu=", scan->next_key++) < 0)
            return false;
    } else if (moved && record->continuation_key != NULL) {
        if (fprintf(file, "%s=", record->continuation_key) < 0)
            return false;
    } else {
        size_t start = moved || dedent ? record->key_start : record->start;
        if (!put_bytes(file, bytes + start, record->value_start - start))
            return false;
    }

    if (target->unchanged) {
        if (!put_bytes(file, bytes + record->value_start, record->value_end - record->value_start))
            return false;
    } else {
        const Entry *entry = target->entry;
        char *path = encode_config_path(entry->icon_path);
        bool written = path != NULL && fprintf(file, "%s;%s;%s", entry->title, path, entry->cmd) >= 0;
        SDL_free(path);
        if (!written)
            return false;
    }

    if (record != NULL && !put_bytes(file, bytes + record->value_end, record->end - record->value_end))
        return false;
    return !moved || (record != NULL && bytes[record->end - 1] == '\n') ||
        fputs(config_line_break(bytes), file) != EOF;
}

static bool omit_menu_record(FILE *file, const char *bytes, const MenuRecord *record, bool keep_key)
{
    size_t suffix = record->value_end;
    while (suffix < record->end && isspace((unsigned char)bytes[suffix]))
        suffix++;
    bool has_comment = suffix < record->end && bytes[suffix] == ';';
    if (keep_key) {
        if (record->continuation_key != NULL) {
            if (fprintf(file, "%s=", record->continuation_key) < 0)
                return false;
        } else if (!put_bytes(file, bytes + record->start, record->value_start - record->start)) {
            return false;
        }
    }
    if (record->destination == SIZE_MAX && has_comment)
        return put_bytes(file, bytes + record->value_end, record->end - record->value_end);
    if (keep_key && bytes[record->end - 1] == '\n') {
        size_t newline = record->end - 1;
        if (newline > record->value_end && bytes[newline - 1] == '\r')
            newline--;
        return put_bytes(file, bytes + newline, record->end - newline);
    }
    return true;
}

static bool write_menu_records(FILE *file, MenuScan *scan, const MenuTarget *targets, size_t count)
{
    const char *bytes = scan->input.bytes;
    size_t next = 0, offset = 0;
    for (size_t i = 0; i < scan->count; i++) {
        const MenuRecord *record = &scan->records[i];
        if (!put_bytes(file, bytes + offset, record->start - offset))
            return false;

        size_t destination = record->destination;
        if (destination == SIZE_MAX || destination < next) {
            // A following continuation still needs its key. An empty menu
            // likewise needs one assignment so the launcher creates it.
            bool keep_key = record->empty_key || record->has_continuation || (count == 0 && i + 1 == scan->count);
            if (!omit_menu_record(file, bytes, record, keep_key))
                return false;
        } else {
            bool inserting = next < destination;
            while (next < destination) {
                if (!write_menu_target(file, scan, &targets[next++], true, false))
                    return false;
            }
            bool dedent = inserting && record->continuation_key == NULL;
            if (!write_menu_target(file, scan, &targets[next++], false, dedent))
                return false;
        }
        offset = record->end;
    }

    size_t tail = scan->menu_end != 0 ? scan->menu_end : scan->input.size;
    if (!put_bytes(file, bytes + offset, tail - offset))
        return false;
    if (next < count) {
        const char *line_break = config_line_break(bytes);
        if (tail != 0 && bytes[tail - 1] != '\n' && fputs(line_break, file) == EOF)
            return false;
        if (scan->menu_end == 0 && fprintf(file, "[%s]%s", scan->name, line_break) < 0)
            return false;
        while (next < count) {
            if (!write_menu_target(file, scan, &targets[next++], true, false))
                return false;
        }
        // An empty key resets continuation parsing for any following raw lines.
        if (tail < scan->input.size && scan->tail_resets_key && fprintf(file, "=%s", line_break) < 0)
            return false;
    }
    return put_bytes(file, bytes + tail, scan->input.size - tail);
}

static const char *write_menu_edit(FILE *file, const char *bytes, size_t size, void *context)
{
    const MenuEdit *edit = context;
    MenuScan scan = {
        .input = { .bytes = bytes, .size = size }, .name = edit->name, .remaining = edit->previous,
        .last_key = SIZE_MAX, .next_key = 1
    };
    size_t count = 0;
    for (const Entry *entry = edit->entries; entry != NULL; entry = entry->next)
        count++;

    int result = ini_parse_stream(scan_line, &scan.input, scan_menu_entry, &scan);
    const char *failure = scan.failure;
    if (failure == NULL && result < 0)
        failure = "Could not read the existing configuration.";
    if (failure == NULL && scan.remaining != NULL)
        failure = menu_conflict;

    MenuTarget *targets = NULL;
    if (failure == NULL && count != 0) {
        targets = match_menu_records(&scan, edit->entries, count);
        if (targets == NULL)
            failure = "Could not allocate memory for the menu configuration.";
        else {
            for (size_t i = 0; i < count; i++) {
                if (!targets[i].unchanged && !valid_entry(targets[i].entry)) {
                    failure = menu_encoding_error;
                    break;
                }
            }
        }
    }
    if (failure == NULL && !write_menu_records(file, &scan, targets, count))
        failure = "Could not write the menu configuration.";

    SDL_free(targets);
    for (size_t i = 0; i < scan.count; i++)
        SDL_free(scan.records[i].continuation_key);
    SDL_free(scan.records);
    return failure;
}

typedef struct {
    const char *name;
    const Entry *remaining;
    bool found;
    bool mismatch;
} MenuVerification;

static int verify_menu_entry(void *context, const char *section, const char *key, const char *value)
{
    MenuVerification *menu = context;
    if (menu->mismatch || strcmp(section, menu->name) != 0 || key == NULL || value == NULL)
        return 1;

    menu->found = true; // Any assignment keeps an empty menu addressable.
    Entry parsed;
    if (parse_menu_entry(value, &parsed)) {
        if (menu->remaining != NULL && same_entry(&parsed, menu->remaining))
            menu->remaining = menu->remaining->next;
        else
            menu->mismatch = true;
        clear_menu_entry(&parsed);
    }
    return 1;
}

static bool verify_menu_edit(FILE *file, void *context)
{
    const MenuEdit *edit = context;
    MenuVerification menu = { .name = edit->name, .remaining = edit->entries };
    return ini_parse_file(file, verify_menu_entry, &menu) >= 0 &&
        !menu.mismatch && menu.remaining == NULL && menu.found;
}

bool save_menu_config(const char *path, const char *menu_name, const Entry *entries,
    const Entry *previous, char *error, size_t error_size)
{
    if (menu_name == NULL || menu_name[0] == '\0' || strpbrk(menu_name, "]\r\n") != NULL) {
        if (error != NULL && error_size > 0)
            SDL_snprintf(error, error_size, "The menu name could not be saved in the INI file.");
        return false;
    }

    bool unchanged = true;
    const Entry *old = previous;
    for (const Entry *entry = entries; entry != NULL; entry = entry->next) {
        if (old == NULL || !same_entry(entry, old))
            unchanged = false;
        if (old != NULL)
            old = old->next;
    }
    if (unchanged && old == NULL) {
        if (error != NULL && error_size > 0)
            error[0] = '\0';
        return true;
    }
    MenuEdit menu = { .name = menu_name, .entries = entries, .previous = previous };
    const ConfigEdit edit = {
        .write = write_menu_edit, .verify = verify_menu_edit, .verification_error = menu_encoding_error
    };
    return update_config_file(path, &edit, &menu, error, error_size);
}
