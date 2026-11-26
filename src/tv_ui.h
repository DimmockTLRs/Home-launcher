#ifndef FLEX_LAUNCHER_TV_UI_H
#define FLEX_LAUNCHER_TV_UI_H

#include "appearance.h"

// Uses the launcher menu and geometry types. Sizing follows the full page,
// while the returned rectangle centers the entries on the visible page.
SDL_Rect measure_tv_row(const Geometry *geometry, unsigned int page_capacity,
    unsigned int visible_count, const AppearanceSettings *appearance, int *card_width, int *gap);
void size_tv_row(Geometry *geometry);
bool apply_tv_appearance(void);
bool init_tv_ui(void);
void layout_tv_entries(Entry *entry, int count);
void reset_tv_motion(void);
void move_tv_focus(SDL_Rect previous, int target_x, int page_direction);
bool tv_motion_active(void);
int draw_tv_row(SDL_Renderer *renderer, const Entry *root, const Entry *selected, int count);
SDL_Rect fit_tv_texture(SDL_Texture *texture, SDL_Rect bounds);
void quit_tv_ui(void);

#endif
