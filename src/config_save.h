#ifndef HOME_LAUNCHER_CONFIG_SAVE_H
#define HOME_LAUNCHER_CONFIG_SAVE_H

// Include launcher.h before this header.
typedef struct {
    ModeBackground mode;
    CollageLayout layout;
    Uint32 image_duration;
    Uint32 transition_time;
    const char *image_path;
    const char *folder_path;
} BackgroundSettings;

FILE *open_launcher_config(const char *path);

bool save_background_config(const char *path, const BackgroundSettings *settings,
    const BackgroundSettings *previous, char *error, size_t error_size);

#endif
