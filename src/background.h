#ifndef FLEX_LAUNCHER_BACKGROUND_H
#define FLEX_LAUNCHER_BACKGROUND_H

#include <stdbool.h>
#include <SDL.h>

bool background_rotates(void);
void init_background(SDL_Renderer *renderer);
bool update_background(SDL_Renderer *renderer, Uint32 now, bool paused);
void draw_background(SDL_Renderer *renderer);
void restart_background_interval(Uint32 now);
void quit_background(void);

#endif
