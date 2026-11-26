#ifndef FLEX_LAUNCHER_APPEARANCE_UI_H
#define FLEX_LAUNCHER_APPEARANCE_UI_H

#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "appearance.h"

bool appearance_ui_open(const AppearanceSettings *settings, bool tv_layout,
    TTF_Font *heading, TTF_Font *body, TTF_Font *small);
bool appearance_ui_is_open(void);
void appearance_ui_command(const char *command);
void appearance_ui_mouse(int x, int y, bool click);
void appearance_ui_draw(SDL_Renderer *renderer);
void appearance_ui_save_completed(bool success, const char *error);
void appearance_ui_quit(void);

#endif
