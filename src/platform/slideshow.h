#ifndef FLEX_LAUNCHER_SLIDESHOW_H
#define FLEX_LAUNCHER_SLIDESHOW_H

#include <limits.h>
#include <stdint.h>

static inline bool is_photo_filename(const char *name)
{
    const char *extension = strrchr(name, '.');
    return extension != NULL &&
        (!SDL_strcasecmp(extension, ".jpg") || !SDL_strcasecmp(extension, ".jpeg") ||
         !SDL_strcasecmp(extension, ".png") || !SDL_strcasecmp(extension, ".webp"));
}

static inline bool append_photo_path(PhotoList *files, const char *folder, const char *name)
{
    if (files->count == files->capacity) {
        if (files->capacity > INT_MAX / 2)
            return false;
        int capacity = files->capacity == 0 ? 64 : files->capacity * 2;
        if ((size_t)capacity > SIZE_MAX / sizeof(*files->paths))
            return false;
        char **paths = realloc(files->paths, (size_t)capacity * sizeof(*files->paths));
        if (paths == NULL)
            return false;
        files->paths = paths;
        files->capacity = capacity;
    }

    size_t length = strlen(folder);
    const char *separator = PATH_SEPARATOR;
    if (length && (folder[length - 1] == separator[0] || folder[length - 1] == '/'))
        separator = "";
    size_t size = length + strlen(separator) + strlen(name) + 1;
    char *path = malloc(size);
    if (path == NULL)
        return false;
    SDL_snprintf(path, size, "%s%s%s", folder, separator, name);
    files->paths[files->count++] = path;
    return true;
}

#endif
