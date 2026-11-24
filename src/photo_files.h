#ifndef HOME_LAUNCHER_PHOTO_FILES_H
#define HOME_LAUNCHER_PHOTO_FILES_H

// Include launcher.h first for PhotoList.
typedef enum {
    PHOTO_PICK_OK,
    PHOTO_PICK_CANCELLED,
    PHOTO_PICK_ERROR
} PhotoPickResult;

// Call the native pickers on the SDL window thread. Returned paths use free().
PhotoPickResult pick_photo_folder(SDL_Window *window, char **folder, char *error, size_t size);
PhotoPickResult pick_photo_files(SDL_Window *window, PhotoList *files, char *error, size_t size);
void free_photo_list(PhotoList *files);
bool same_photo_path(const char *first, const char *second);

// These filesystem operations can run on a worker.
bool copy_photo_into_folder(const char *source, const char *folder, char *error, size_t size);
bool remove_managed_photo(const char *path, const char *folder, char *error, size_t size);

#endif
