#ifndef FLEX_LAUNCHER_WEATHER_H
#define FLEX_LAUNCHER_WEATHER_H

#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>

void init_weather(TTF_Font *font);
bool update_weather(Uint32 now);
void draw_weather(SDL_Renderer *renderer, int width, int height, bool clock_on_left);
void quit_weather(void);

#endif
