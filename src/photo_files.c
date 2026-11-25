#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include <SDL_syswm.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <SDL.h>
#include <launcher_config.h>
#include "launcher.h"
#include "photo_files.h"
#include "util.h"
#include "platform/slideshow.h"

#define PHOTO_CONFLICT_LIMIT 10000

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size != 0)
        SDL_strlcpy(error, message, size);
}

static bool fail(char *error, size_t size, const char *message)
{
    set_error(error, size, message);
    return false;
}

static const char *file_name(const char *path)
{
    const char *last = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (backslash != NULL && (last == NULL || backslash > last))
        last = backslash;
#endif
    return last != NULL ? last + 1 : path;
}

static char *join_utf8_path(const char *folder, const char *name)
{
    size_t folder_length = strlen(folder);
    size_t name_length = strlen(name);
    bool add_separator = folder_length != 0 && folder[folder_length - 1] != '/' &&
        folder[folder_length - 1] != PATH_SEPARATOR[0];
    if (folder_length > SIZE_MAX - name_length - 2)
        return NULL;

    char *path = malloc(folder_length + name_length + 2);
    if (path != NULL) {
        memcpy(path, folder, folder_length);
        if (add_separator)
            path[folder_length++] = PATH_SEPARATOR[0];
        memcpy(path + folder_length, name, name_length + 1);
    }
    return path;
}

static bool valid_photo_name(const char *name)
{
#ifdef _WIN32
    if (strchr(name, ':') != NULL)
        return false;
#endif
    return is_photo_filename(name);
}

void free_photo_list(PhotoList *files)
{
    for (int i = 0; i < files->count; i++)
        free(files->paths[i]);
    free(files->paths);
    *files = (PhotoList){0};
}

static bool write_photo(FILE *source, FILE *target)
{
    unsigned char bytes[64 * 1024];
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), source)) > 0) {
        if (fwrite(bytes, 1, count, target) != count)
            return false;
    }
    return !ferror(source) && fflush(target) == 0;
}

static char *copy_name(const char *name, int sequence)
{
    const char *extension = strrchr(name, '.');
    size_t length = strlen(name);
    char *destination = malloc(length + 16);
    if (destination == NULL)
        return NULL;

    if (sequence == 1)
        memcpy(destination, name, length + 1);
    else {
        size_t stem = (size_t)(extension - name);
        memcpy(destination, name, stem);
        SDL_snprintf(destination + stem, length + 16 - stem, " (%d)%s", sequence, extension);
    }
    return destination;
}

#ifdef _WIN32

static wchar_t *wide_path(const char *path)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (count == 0) {
        // Older launchers accepted configs written using the local ANSI code page.
        page = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(page, flags, path, -1, NULL, 0);
    }
    if (count == 0 || (size_t)count > SIZE_MAX / sizeof(wchar_t))
        return NULL;

    wchar_t *result = malloc((size_t)count * sizeof(*result));
    if (result != NULL && !MultiByteToWideChar(page, flags, path, -1, result, count)) {
        free(result);
        result = NULL;
    }
    return result;
}

static char *utf8_path(const wchar_t *path)
{
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
    if (bytes == 0)
        return NULL;
    char *result = malloc((size_t)bytes);
    if (result != NULL && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
            result, bytes, NULL, NULL)) {
        free(result);
        result = NULL;
    }
    return result;
}

static wchar_t *join_folder(const wchar_t *folder, const wchar_t *name)
{
    size_t folder_length = wcslen(folder);
    size_t name_length = wcslen(name);
    bool has_separator = folder_length != 0 &&
        (folder[folder_length - 1] == L'\\' || folder[folder_length - 1] == L'/');
    if (folder_length > SIZE_MAX / sizeof(wchar_t) - name_length - 2)
        return NULL;
    wchar_t *path = malloc((folder_length + name_length + 2) * sizeof(*path));
    if (path != NULL) {
        wmemcpy(path, folder, folder_length);
        if (!has_separator)
            path[folder_length++] = L'\\';
        wmemcpy(path + folder_length, name, name_length + 1);
    }
    return path;
}

static void windows_error(char *error, size_t size, const char *message, unsigned long code)
{
    if (error != NULL && size != 0)
        SDL_snprintf(error, size, "%s (Windows error %lu).", message, code);
}

static char *item_path(IShellItem *item)
{
    PWSTR path = NULL;
    if (FAILED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)))
        return NULL;
    char *converted = utf8_path(path);
    CoTaskMemFree(path);
    return converted;
}

typedef enum {
    BACKGROUND_FOLDER,
    BACKGROUND_PHOTOS,
    APP_EXECUTABLE,
    APP_ICON
} PickerKind;

static bool valid_executable_name(const char *name)
{
    const char *extension = strrchr(name, '.');
    return strchr(name, ':') == NULL && extension != NULL &&
        (!SDL_strcasecmp(extension, ".exe") || !SDL_strcasecmp(extension, ".lnk"));
}

static HRESULT get_chosen_path(IFileOpenDialog *dialog, PickerKind kind, char **path)
{
    IShellItem *item = NULL;
    HRESULT result = IFileOpenDialog_GetResult(dialog, &item);
    if (FAILED(result))
        return result;
    char *chosen = item_path(item);
    IShellItem_Release(item);
    if (chosen == NULL)
        return E_FAIL;
    if ((kind == APP_EXECUTABLE && !valid_executable_name(file_name(chosen))) ||
        (kind == APP_ICON && !valid_photo_name(file_name(chosen)))) {
        free(chosen);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    *path = chosen;
    return result;
}

static HRESULT get_chosen_photos(IFileOpenDialog *dialog, PhotoList *files)
{
    IShellItemArray *items = NULL;
    HRESULT result = IFileOpenDialog_GetResults(dialog, &items);
    if (FAILED(result))
        return result;

    DWORD count;
    result = IShellItemArray_GetCount(items, &count);
    if (SUCCEEDED(result)) {
        if (count == 0 || count > INT_MAX)
            result = E_FAIL;
        else {
            files->paths = calloc((size_t)count, sizeof(*files->paths));
            if (files->paths == NULL)
                result = E_OUTOFMEMORY;
            else
                files->capacity = (int)count;
        }
    }

    for (int i = 0; SUCCEEDED(result) && i < files->capacity; i++) {
        IShellItem *item = NULL;
        result = IShellItemArray_GetItemAt(items, (DWORD)i, &item);
        if (SUCCEEDED(result)) {
            char *path = item_path(item);
            IShellItem_Release(item);
            if (path == NULL)
                result = E_FAIL;
            else if (!valid_photo_name(file_name(path))) {
                free(path);
                result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            else
                files->paths[files->count++] = path;
        }
    }
    IShellItemArray_Release(items);
    if (FAILED(result))
        free_photo_list(files);
    return result;
}

static PhotoPickResult show_picker(SDL_Window *window, PickerKind kind, char **path,
    PhotoList *photos, char *error, size_t size)
{
    static const wchar_t *const titles[] = {
        [BACKGROUND_FOLDER] = L"Choose background folder",
        [BACKGROUND_PHOTOS] = L"Add photos",
        [APP_EXECUTABLE] = L"Choose an application",
        [APP_ICON] = L"Choose an app icon"
    };
    static const COMDLG_FILTERSPEC photo_types[] = {
        {L"Photos (JPEG, PNG, WebP)", L"*.jpg;*.jpeg;*.png;*.webp"}
    };
    static const COMDLG_FILTERSPEC executable_types[] = {
        {L"Applications and shortcuts", L"*.exe;*.lnk"}
    };
    set_error(error, size, "");
    SDL_SysWMinfo window_info;
    SDL_VERSION(&window_info.version);
    if (!SDL_GetWindowWMInfo(window, &window_info) || window_info.subsystem != SDL_SYSWM_WINDOWS) {
        set_error(error, size, "Could not find the launcher window for the Windows picker.");
        return PHOTO_PICK_ERROR;
    }

    HRESULT result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(result)) {
        set_error(error, size, "Could not start the Windows picker.");
        return PHOTO_PICK_ERROR;
    }

    IFileOpenDialog *dialog = NULL;
    result = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
        &IID_IFileOpenDialog, (void **)&dialog);
    if (SUCCEEDED(result)) {
        FILEOPENDIALOGOPTIONS options;
        result = IFileOpenDialog_GetOptions(dialog, &options);
        if (SUCCEEDED(result)) {
            options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
            options |= kind == BACKGROUND_FOLDER ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
            if (kind == BACKGROUND_PHOTOS)
                options |= FOS_ALLOWMULTISELECT;
            if (kind == APP_EXECUTABLE)
                options |= FOS_NODEREFERENCELINKS;
            result = IFileOpenDialog_SetOptions(dialog, options);
        }
        if (SUCCEEDED(result))
            result = IFileOpenDialog_SetTitle(dialog, titles[kind]);
        if (SUCCEEDED(result) && kind != BACKGROUND_FOLDER) {
            const COMDLG_FILTERSPEC *types = kind == APP_EXECUTABLE ? executable_types : photo_types;
            result = IFileOpenDialog_SetFileTypes(dialog, 1, types);
        }
        if (SUCCEEDED(result)) {
            result = IFileOpenDialog_Show(dialog, window_info.info.win.window);
            if (SUCCEEDED(result))
                result = kind == BACKGROUND_PHOTOS ? get_chosen_photos(dialog, photos) :
                    get_chosen_path(dialog, kind, path);
            SDL_RaiseWindow(window);
        }
        IFileOpenDialog_Release(dialog);
    }
    CoUninitialize();

    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return PHOTO_PICK_CANCELLED;
    if (FAILED(result)) {
        const char *message = "Could not open or read the Windows selection.";
        if (result == HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) {
            if (kind == APP_EXECUTABLE)
                message = "Select a Windows executable (.exe) or shortcut (.lnk).";
            else if (kind == APP_ICON)
                message = "Select a JPEG, PNG or WebP app icon.";
            else
                message = "Select JPEG, PNG or WebP photos.";
        }
        set_error(error, size, message);
        return PHOTO_PICK_ERROR;
    }
    return PHOTO_PICK_OK;
}

PhotoPickResult pick_photo_folder(SDL_Window *window, char **folder, char *error, size_t size)
{
    *folder = NULL;
    return show_picker(window, BACKGROUND_FOLDER, folder, NULL, error, size);
}

PhotoPickResult pick_photo_files(SDL_Window *window, PhotoList *files, char *error, size_t size)
{
    *files = (PhotoList){0};
    return show_picker(window, BACKGROUND_PHOTOS, NULL, files, error, size);
}

PhotoPickResult pick_app_executable(SDL_Window *window, char **path, char *error, size_t size)
{
    *path = NULL;
    return show_picker(window, APP_EXECUTABLE, path, NULL, error, size);
}

PhotoPickResult pick_app_icon(SDL_Window *window, char **path, char *error, size_t size)
{
    *path = NULL;
    return show_picker(window, APP_ICON, path, NULL, error, size);
}

static FILE *open_photo_source(const wchar_t *path)
{
    HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(handle);
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return NULL;
    }
    FILE *source = _fdopen(fd, "rb");
    if (source == NULL)
        _close(fd);
    return source;
}

static FILE *open_import_temp(const wchar_t *folder, wchar_t **temporary)
{
    for (unsigned int attempt = 0; attempt < 100; attempt++) {
        wchar_t name[96];
        swprintf(name, SDL_arraysize(name), L".home-launcher-%lu-%llu-%u.tmp",
            (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(), attempt);
        wchar_t *path = join_folder(folder, name);
        if (path == NULL)
            return NULL;

        HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD last = GetLastError();
            free(path);
            if (last == ERROR_FILE_EXISTS || last == ERROR_ALREADY_EXISTS)
                continue;
            return NULL;
        }
        *temporary = path;
        int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
        if (fd < 0) {
            CloseHandle(handle);
            return NULL;
        }
        FILE *output = _fdopen(fd, "wb");
        if (output == NULL)
            _close(fd);
        return output;
    }
    return NULL;
}

static bool publish_photo(const wchar_t *temp, const wchar_t *folder, const char *name,
    char **imported_path, char *error, size_t size)
{
    for (int sequence = 1; sequence <= PHOTO_CONFLICT_LIMIT; sequence++) {
        char *candidate = copy_name(name, sequence);
        wchar_t *native = candidate != NULL ? wide_path(candidate) : NULL;
        wchar_t *destination = native != NULL ? join_folder(folder, native) : NULL;
        free(candidate);
        free(native);
        if (destination == NULL)
            return fail(error, size, "Could not allocate an import filename.");

        char *published = imported_path != NULL ? utf8_path(destination) : NULL;
        if (imported_path != NULL && published == NULL) {
            free(destination);
            return fail(error, size, "Could not allocate an import filename.");
        }
        if (MoveFileExW(temp, destination, MOVEFILE_WRITE_THROUGH)) {
            free(destination);
            if (imported_path != NULL)
                *imported_path = published;
            return true;
        }
        DWORD last = GetLastError();
        bool conflict = last == ERROR_FILE_EXISTS || last == ERROR_ALREADY_EXISTS ||
            GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES;
        free(destination);
        free(published);
        if (!conflict) {
            windows_error(error, size, "Could not place the image in the destination folder", last);
            return false;
        }
    }
    return fail(error, size, "Too many photos already have this filename.");
}

static bool copy_photo_file(const char *source, const char *folder, char **imported_path,
    char *error, size_t size)
{
    const char *name = file_name(source);
    if (!valid_photo_name(name))
        return fail(error, size, "Select a JPEG, PNG or WebP image.");

    wchar_t *input = wide_path(source);
    wchar_t *directory = wide_path(folder);
    wchar_t *temporary = NULL;
    bool copied = false;
    FILE *photo = input != NULL ? open_photo_source(input) : NULL;
    if (photo == NULL)
        set_error(error, size, "Could not open the selected image.");
    else {
        FILE *target = directory != NULL ? open_import_temp(directory, &temporary) : NULL;
        if (target == NULL)
            set_error(error, size, "Could not write to the destination folder. Check that it exists and is writable.");
        else {
            bool written = write_photo(photo, target) && _commit(_fileno(target)) == 0;
            if (fclose(target) != 0 || !written)
                set_error(error, size, "Could not finish copying the image. Check available space.");
            else
                copied = publish_photo(temporary, directory, name, imported_path, error, size);
        }
        fclose(photo);
    }

    if (temporary != NULL && !copied)
        DeleteFileW(temporary);
    free(temporary);
    free(directory);
    free(input);
    return copied;
}

static bool create_icon_folder(const char *path)
{
    wchar_t *native = wide_path(path);
    if (native == NULL)
        return false;
    bool success = CreateDirectoryW(native, NULL) != 0;
    if (!success && GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD attributes = GetFileAttributesW(native);
        success = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    free(native);
    return success;
}

static bool normal_file(HANDLE handle, bool directory)
{
    FILE_ATTRIBUTE_TAG_INFO info;
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
        return false;

    // Refuse symlinks and junctions, while allowing OneDrive's cloud placeholders.
    bool alias = (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        (info.ReparseTag & 0x20000000u) != 0;
    return !alias && ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

static HANDLE open_directory(const wchar_t *path)
{
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle != INVALID_HANDLE_VALUE && !normal_file(handle, true)) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

static bool same_file(HANDLE first, HANDLE second)
{
    BY_HANDLE_FILE_INFORMATION a, b;
    return GetFileInformationByHandle(first, &a) && GetFileInformationByHandle(second, &b) &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

static wchar_t *full_windows_path(const wchar_t *path, wchar_t **name)
{
    DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0 || needed > 32768)
        return NULL;
    wchar_t *full = malloc((size_t)needed * sizeof(*full));
    if (full == NULL)
        return NULL;
    DWORD length = GetFullPathNameW(path, needed, full, name);
    if (length == 0 || length >= needed) {
        free(full);
        return NULL;
    }
    return full;
}

static wchar_t *parent_directory(const wchar_t *path)
{
    wchar_t *name = NULL;
    wchar_t *full = full_windows_path(path, &name);
    if (name == NULL) {
        free(full);
        return NULL;
    }
    if (name - full > 3 && name[-2] != L':')
        name[-1] = L'\0';
    else
        *name = L'\0';
    return full;
}

static wchar_t *comparable_windows_path(const char *path)
{
    wchar_t *native = wide_path(path);
    wchar_t *full = native != NULL ? full_windows_path(native, NULL) : NULL;
    free(native);
    if (full == NULL)
        return NULL;
    size_t length = wcslen(full);
    if (wcsncmp(full, L"\\\\?\\UNC\\", 8) == 0) {
        full[6] = L'\\';
        wmemmove(full, full + 6, length - 5);
    }
    else if (wcsncmp(full, L"\\\\?\\", 4) == 0)
        wmemmove(full, full + 4, length - 3);
    length = wcslen(full);
    while (length > 1 && (full[length - 1] == L'\\' || full[length - 1] == L'/'))
        full[--length] = L'\0';
    return full;
}

bool same_photo_path(const char *first, const char *second)
{
    if (first == NULL || second == NULL || first[0] == '\0' || second[0] == '\0')
        return false;
    if (strcmp(first, second) == 0)
        return true;
    wchar_t *a = comparable_windows_path(first);
    wchar_t *b = comparable_windows_path(second);
    bool same = a != NULL && b != NULL && CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
    free(a);
    free(b);
    return same;
}

bool remove_managed_photo(const char *path, const char *folder, char *error, size_t size)
{
    if (!valid_photo_name(file_name(path)))
        return fail(error, size, "This file is not a photo in the background folder.");

    wchar_t *photo = wide_path(path);
    wchar_t *base = wide_path(folder);
    wchar_t *parent = photo != NULL ? parent_directory(photo) : NULL;
    HANDLE managed = base != NULL ? open_directory(base) : INVALID_HANDLE_VALUE;
    HANDLE actual = parent != NULL ? open_directory(parent) : INVALID_HANDLE_VALUE;
    bool removed = false;
    if (managed == INVALID_HANDLE_VALUE || actual == INVALID_HANDLE_VALUE || !same_file(managed, actual))
        set_error(error, size, "Only photos directly inside the background folder can be removed.");
    else {
        HANDLE item = CreateFileW(photo, DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (item == INVALID_HANDLE_VALUE)
            windows_error(error, size, "Could not open the photo for removal", GetLastError());
        else {
            if (!normal_file(item, false))
                set_error(error, size, "Photo links and folders cannot be removed here.");
            else {
                FILE_DISPOSITION_INFO disposition = {.DeleteFile = TRUE};
                removed = SetFileInformationByHandle(item, FileDispositionInfo,
                    &disposition, sizeof(disposition)) != 0;
                if (!removed)
                    windows_error(error, size, "Could not remove the photo", GetLastError());
            }
            CloseHandle(item);
        }
    }
    if (managed != INVALID_HANDLE_VALUE)
        CloseHandle(managed);
    if (actual != INVALID_HANDLE_VALUE)
        CloseHandle(actual);
    free(parent);
    free(base);
    free(photo);
    return removed;
}

#else

static char *lexical_path(const char *path)
{
    char *full;
    if (path[0] == '/')
        full = strdup(path);
    else {
        char *cwd = getcwd(NULL, 0);
        if (cwd == NULL)
            return NULL;
        size_t size = strlen(cwd) + strlen(path) + 2;
        full = malloc(size);
        if (full != NULL)
            SDL_snprintf(full, size, "%s/%s", cwd, path);
        free(cwd);
    }
    if (full == NULL)
        return NULL;

    // Normalise separators and dot components without inspecting the filesystem.
    char *output = full + 1;
    const char *segment = full + 1;
    while (*segment != '\0') {
        size_t length = strcspn(segment, "/");
        if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            while (output > full + 1 && *--output != '/') {}
        }
        else if (length != 0 && !(length == 1 && segment[0] == '.')) {
            if (output > full + 1)
                *output++ = '/';
            memmove(output, segment, length);
            output += length;
        }
        segment += length;
        if (*segment == '/')
            segment++;
    }
    *output = '\0';
    return full;
}

bool same_photo_path(const char *first, const char *second)
{
    if (first == NULL || second == NULL || first[0] == '\0' || second[0] == '\0')
        return false;
    if (strcmp(first, second) == 0)
        return true;
    char *a = lexical_path(first);
    char *b = lexical_path(second);
    bool same = a != NULL && b != NULL && strcmp(a, b) == 0;
    free(a);
    free(b);
    return same;
}

PhotoPickResult pick_photo_folder(SDL_Window *window, char **folder, char *error, size_t size)
{
    (void)window;
    *folder = NULL;
    set_error(error, size, "The native folder picker is available on Windows.");
    return PHOTO_PICK_ERROR;
}

PhotoPickResult pick_photo_files(SDL_Window *window, PhotoList *files, char *error, size_t size)
{
    (void)window;
    *files = (PhotoList){0};
    set_error(error, size, "The native photo picker is available on Windows.");
    return PHOTO_PICK_ERROR;
}

PhotoPickResult pick_app_executable(SDL_Window *window, char **path, char *error, size_t size)
{
    (void)window;
    *path = NULL;
    set_error(error, size, "The native application picker is available on Windows.");
    return PHOTO_PICK_ERROR;
}

PhotoPickResult pick_app_icon(SDL_Window *window, char **path, char *error, size_t size)
{
    (void)window;
    *path = NULL;
    set_error(error, size, "The native icon picker is available on Windows.");
    return PHOTO_PICK_ERROR;
}

static FILE *open_photo_source(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat info;
    FILE *source = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) ? fdopen(fd, "rb") : NULL;
    if (source == NULL)
        close(fd);
    return source;
}

static FILE *open_import_temp(int folder, char *temporary, size_t size)
{
    for (unsigned int attempt = 0; attempt < 100; attempt++) {
        SDL_snprintf(temporary, size, ".home-launcher-%ld-%u-%u.tmp",
            (long)getpid(), (unsigned int)SDL_GetTicks(), attempt);
        int fd = openat(folder, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) {
            FILE *target = fdopen(fd, "wb");
            if (target == NULL)
                close(fd);
            return target;
        }
        if (errno != EEXIST)
            break;
    }
    temporary[0] = '\0';
    return NULL;
}

static bool publish_photo(int folder, const char *folder_path, const char *temporary,
    const char *name, char **imported_path, char *error, size_t size)
{
    for (int sequence = 1; sequence <= PHOTO_CONFLICT_LIMIT; sequence++) {
        char *candidate = copy_name(name, sequence);
        if (candidate == NULL)
            return fail(error, size, "Could not allocate an import filename.");
        char *published = imported_path != NULL ? join_utf8_path(folder_path, candidate) : NULL;
        if (imported_path != NULL && published == NULL) {
            free(candidate);
            return fail(error, size, "Could not allocate an import filename.");
        }
        bool moved = linkat(folder, temporary, folder, candidate, 0) == 0;
        int last = errno;
        free(candidate);
        if (moved) {
            if (imported_path != NULL)
                *imported_path = published;
            return true;
        }
        free(published);
        if (last != EEXIST)
            return fail(error, size, "Could not place the image in the destination folder.");
    }
    return fail(error, size, "Too many photos already have this filename.");
}

static bool copy_photo_file(const char *source, const char *folder, char **imported_path,
    char *error, size_t size)
{
    const char *name = file_name(source);
    if (!valid_photo_name(name))
        return fail(error, size, "Select a JPEG, PNG or WebP image.");
    FILE *photo = open_photo_source(source);
    if (photo == NULL)
        return fail(error, size, "Could not open the selected image.");

    int directory = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char temporary[96] = {0};
    FILE *target = directory >= 0 ? open_import_temp(directory, temporary, sizeof(temporary)) : NULL;
    bool copied = false;
    if (target == NULL)
        set_error(error, size, "Could not write to the destination folder. Check that it exists and is writable.");
    else {
        bool written = write_photo(photo, target) && fsync(fileno(target)) == 0;
        if (fclose(target) != 0 || !written)
            set_error(error, size, "Could not finish copying the image. Check available space.");
        else
            copied = publish_photo(directory, folder, temporary, name, imported_path, error, size);
    }
    if (temporary[0] != '\0')
        unlinkat(directory, temporary, 0);
    if (directory >= 0)
        close(directory);
    fclose(photo);
    return copied;
}

static bool create_icon_folder(const char *path)
{
    if (mkdir(path, 0700) == 0)
        return true;
    struct stat info;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int open_parent(const char *path, const char *name)
{
    if (name == path)
        return open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    size_t length = (size_t)(name - path);
    char *parent = malloc(length + 1);
    if (parent == NULL)
        return -1;

    memcpy(parent, path, length);
    parent[length] = '\0';
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    return directory;
}

bool remove_managed_photo(const char *path, const char *folder, char *error, size_t size)
{
    const char *name = file_name(path);
    if (!valid_photo_name(name))
        return fail(error, size, "This file is not a photo in the background folder.");

    int managed = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int actual = open_parent(path, name);
    struct stat expected, claimed, photo;
    bool removed = false;
    if (managed < 0 || actual < 0 || fstat(managed, &expected) != 0 || fstat(actual, &claimed) != 0 ||
        expected.st_dev != claimed.st_dev || expected.st_ino != claimed.st_ino)
        set_error(error, size, "Only photos directly inside the background folder can be removed.");
    else if (fstatat(managed, name, &photo, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(photo.st_mode))
        set_error(error, size, "Could not find a regular photo to remove.");
    else {
        removed = unlinkat(managed, name, 0) == 0;
        if (!removed)
            set_error(error, size, "Could not remove the photo. Check folder permissions.");
    }
    if (managed >= 0)
        close(managed);
    if (actual >= 0)
        close(actual);
    return removed;
}

#endif

bool copy_photo_into_folder(const char *source, const char *folder, char *error, size_t size)
{
    return copy_photo_file(source, folder, NULL, error, size);
}

char *app_icon_folder(char *error, size_t size)
{
    set_error(error, size, "");
    char *preferences = SDL_GetPrefPath("DimmockTLRs", "Home Launcher");
    if (preferences == NULL) {
        set_error(error, size, "Could not find the launcher data folder for app icons.");
        return NULL;
    }
    char *folder = join_utf8_path(preferences, "icons");
    SDL_free(preferences);
    if (folder == NULL || !create_icon_folder(folder)) {
        free(folder);
        set_error(error, size, "Could not create or open the app icon library.");
        return NULL;
    }
    return folder;
}

bool import_app_icon(const char *source, char **imported_path, char *error, size_t size)
{
    *imported_path = NULL;
    if (!valid_photo_name(file_name(source)))
        return fail(error, size, "Select a JPEG, PNG or WebP app icon.");

    char *folder = app_icon_folder(error, size);
    if (folder == NULL)
        return false;
    bool copied = copy_photo_file(source, folder, imported_path, error, size);
    free(folder);
    return copied;
}
