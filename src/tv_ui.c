#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include "image.h"
#include "tv_ui.h"

#define TV_TRANSITION_MS 170u

extern Config config;
extern Geometry geo;

static struct {
    int margin;
    int gap;
    int card_width;
    int card_height;
    SDL_Texture *card;
    SDL_Texture *focused;
    SDL_Texture *scrim;
    SDL_Rect scrim_rect;
    bool moving;
    int slide_from;
    int focus_from;
    int focus_to;
    Uint32 started;
} tv;

void size_tv_row(Geometry *geometry)
{
    int width = geometry->screen_width;
    int height = geometry->screen_height;
    tv.margin = width * 5 / 100;
    tv.gap = SDL_max(2, width / 64);
    int available = width - 2 * tv.margin;
    int minimum = SDL_max(1, height / 9);
    int capacity = SDL_max(1, (available + tv.gap) / (minimum + tv.gap));
    config.max_buttons = SDL_min(config.max_buttons, (unsigned int)capacity);
    int count = (int)config.max_buttons;
    tv.card_width = SDL_max(1, SDL_min(height * 34 / 100, (available - (count - 1) * tv.gap) / count));
    tv.card_height = SDL_max(1, tv.card_width * 10 / 17);
    geometry->y_margin = height - height * 6 / 100 - tv.card_height;
    geometry->x_advance = tv.card_width + tv.gap;
    tv.scrim_rect = (SDL_Rect){0, height / 2, width, height - height / 2};
}

static SDL_Texture *card_texture(bool focused)
{
    char svg[400];
    int border = focused ? SDL_max(1, tv.card_height / 44) : 0;
    int inset = (border + 1) / 2;
    SDL_snprintf(svg, sizeof(svg),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\">"
        "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" "
        "fill=\"%s\" fill-opacity=\"%s\" stroke=\"#C1DAFF\" stroke-width=\"%d\"/></svg>",
        tv.card_width, tv.card_height, inset, inset, SDL_max(1, tv.card_width - 2 * inset),
        SDL_max(1, tv.card_height - 2 * inset), SDL_max(1, tv.card_height / 8),
        focused ? "#293C56" : "#111926", focused ? "0.98" : "0.83", border);
    return rasterize_svg(svg, -1, -1, NULL);
}

bool init_tv_ui(void)
{
    tv.card = card_texture(false);
    tv.focused = card_texture(true);

    // Build the scrim once. The narrow source texture stretches across the TV.
    SDL_Surface *gradient = SDL_CreateRGBSurfaceWithFormat(0, 1, 128, 32, SDL_PIXELFORMAT_RGBA32);
    if (gradient != NULL) {
        for (int y = 0; y < gradient->h; y++) {
            int alpha = 222 * y * y / ((gradient->h - 1) * (gradient->h - 1));
            SDL_Rect pixel = {0, y, 1, 1};
            SDL_FillRect(gradient, &pixel, SDL_MapRGBA(gradient->format, 6, 11, 20, (Uint8)alpha));
        }
        tv.scrim = load_texture(gradient);
    }
    return tv.card != NULL && tv.focused != NULL && tv.scrim != NULL;
}

SDL_Rect fit_tv_texture(SDL_Texture *texture, SDL_Rect bounds)
{
    int width, height;
    if (texture == NULL || SDL_QueryTexture(texture, NULL, NULL, &width, &height) != 0 ||
        width <= 0 || height <= 0)
        return bounds;
    if ((Sint64)width * bounds.h > (Sint64)height * bounds.w) {
        int h = SDL_max(1, (int)((Sint64)height * bounds.w / width));
        bounds.y += (bounds.h - h) / 2;
        bounds.h = h;
    }
    else {
        int w = SDL_max(1, (int)((Sint64)width * bounds.h / height));
        bounds.x += (bounds.w - w) / 2;
        bounds.w = w;
    }
    return bounds;
}

void layout_tv_entries(Entry *entry, int count)
{
    int row_width = count * tv.card_width + (count - 1) * tv.gap;
    geo.x_margin = (geo.screen_width - row_width) / 2;
    geo.num_buttons = count;
    int pad = SDL_max(1, tv.card_height / 10);
    int label_height = config.titles_enabled ? SDL_max(1, tv.card_height / 5) : 0;
    int icon_limit = SDL_max(1, tv.card_height - 2 * pad - label_height -
        (config.titles_enabled ? pad / 2 : 0));
    int logo_size = SDL_min(icon_limit, SDL_min((int)config.icon_size, tv.card_width - 2 * pad));
    logo_size = SDL_max(1, logo_size);

    for (int i = 0; i < count; i++) {
        int x = geo.x_margin + i * geo.x_advance;
        entry->card_rect = (SDL_Rect){x, geo.y_margin, tv.card_width, tv.card_height};
        entry->icon_rect = (SDL_Rect){x + (tv.card_width - logo_size) / 2,
            geo.y_margin + pad + (icon_limit - logo_size) / 2, logo_size, logo_size};
        if (config.titles_enabled) {
            SDL_Rect label = {x + pad, geo.y_margin + tv.card_height - pad - label_height,
                SDL_max(1, tv.card_width - 2 * pad), label_height};
            entry->text_rect = fit_tv_texture(entry->title_texture, label);
        }
        entry = entry->next;
    }
}

void reset_tv_motion(void)
{
    tv.moving = false;
}

static float motion_remaining(Uint32 now)
{
    Uint32 elapsed = now - tv.started;
    if (elapsed >= TV_TRANSITION_MS) {
        tv.moving = false;
        return 0;
    }
    float remaining = 1.0f - (float)elapsed / TV_TRANSITION_MS;
    return remaining * remaining * remaining;
}

void move_tv_focus(SDL_Rect previous, int target_x, int page_direction)
{
    Uint32 now = SDL_GetTicks();
    int origin = previous.x;
    if (page_direction == 0 && tv.moving && tv.slide_from == 0)
        origin = tv.focus_to + (int)((tv.focus_from - tv.focus_to) * motion_remaining(now));
    tv.focus_from = origin;
    tv.focus_to = target_x;
    tv.slide_from = page_direction * tv.card_width / 3;
    tv.started = now;
    tv.moving = true;
}

bool tv_motion_active(void)
{
    return tv.moving;
}

int draw_tv_row(SDL_Renderer *renderer, const Entry *root, const Entry *selected, int count)
{
    SDL_RenderCopy(renderer, tv.scrim, NULL, &tv.scrim_rect);
    SDL_Rect clip = {tv.margin, 0, geo.screen_width - 2 * tv.margin, geo.screen_height};
    SDL_RenderSetClipRect(renderer, &clip);

    float remaining = tv.moving ? motion_remaining(SDL_GetTicks()) : 0;
    int offset = (int)(tv.slide_from * remaining);
    for (int i = 0; i < count; i++) {
        SDL_Rect rect = root->card_rect;
        rect.x += offset;
        SDL_RenderCopy(renderer, tv.card, NULL, &rect);
        root = root->next;
    }
    if (config.highlight) {
        SDL_Rect focus = selected->card_rect;
        if (tv.slide_from == 0)
            focus.x += (int)((tv.focus_from - focus.x) * remaining);
        focus.x += offset;
        SDL_RenderCopy(renderer, tv.focused, NULL, &focus);
    }
    return offset;
}

void quit_tv_ui(void)
{
    SDL_DestroyTexture(tv.card);
    SDL_DestroyTexture(tv.focused);
    SDL_DestroyTexture(tv.scrim);
}
