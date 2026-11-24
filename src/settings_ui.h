#ifndef FLEX_LAUNCHER_SETTINGS_UI_H
#define FLEX_LAUNCHER_SETTINGS_UI_H

#include <stdbool.h>
#include <SDL.h>

bool settings_open(void);
bool settings_is_open(void);
void settings_command(const char *command);
void settings_mouse(int x, int y, bool click);
void settings_save_completed(bool success, const char *error);
void settings_pump(void);
void settings_draw(SDL_Renderer *renderer);
void settings_quit(void);

#endif
