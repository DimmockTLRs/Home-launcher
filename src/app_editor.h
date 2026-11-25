#ifndef FLEX_LAUNCHER_APP_EDITOR_H
#define FLEX_LAUNCHER_APP_EDITOR_H

#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>

// Include launcher.h before this header.
bool app_editor_open(const Entry *entries, const Entry *stop,
    TTF_Font *heading, TTF_Font *body, TTF_Font *small);
bool app_editor_is_open(void);
bool app_editor_is_typing(void);
void app_editor_command(const char *command);
void app_editor_mouse(int x, int y, bool click);
void app_editor_draw(SDL_Renderer *renderer);
void app_editor_key(SDL_Keycode key);
void app_editor_text(const char *text);
void app_editor_pump(void);
void app_editor_quit(void);
void app_editor_save_completed(bool success, const char *error);

#endif
