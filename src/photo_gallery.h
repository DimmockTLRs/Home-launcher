#ifndef HOME_LAUNCHER_PHOTO_GALLERY_H
#define HOME_LAUNCHER_PHOTO_GALLERY_H

#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>

// Include launcher.h before this header.
void open_photo_gallery(ModeBackground *mode, char **folder, char **image,
    TTF_Font *heading, TTF_Font *body, TTF_Font *small);
bool photo_gallery_is_open(void);
void photo_gallery_command(const char *command);
void photo_gallery_mouse(int x, int y, bool click);
void draw_photo_gallery(SDL_Renderer *renderer);
void pump_photo_gallery(void);
void quit_photo_gallery(void);

#endif
