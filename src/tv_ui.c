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
    int skin_width;
    int skin_height;
    int skin_opacity;
    SDL_Texture *card;
    SDL_Texture *focused;
    SDL_Texture *scrim;
    SDL_Rect scrim_rect;
    bool floating_scrim;
    bool moving;
    int slide_from;
    int focus_from;
    int focus_to;
    Uint32 started;
} tv;

SDL_Rect measure_tv_row(const Geometry *geometry, unsigned int page_capacity,
    unsigned int visible_count, const AppearanceSettings *appearance, int *card_width, int *gap)
{
    int width = geometry->screen_width;
    int height = geometry->screen_height;
    if (width <= 0 || height <= 0 || page_capacity == 0 || visible_count == 0) {
        if (card_width != NULL)
            *card_width = 0;
        if (gap != NULL)
            *gap = 0;
        return (SDL_Rect){0};
    }

    int side_margin = width / 20;
    int spacing = SDL_max(2, width / 64);
    int available = width - 2 * side_margin;
    // Room for at least one pixel per slot, even in a very small preview.
    int fit_capacity = SDL_max(1, (available + spacing) / (1 + spacing));
    int slots = (int)SDL_min(page_capacity, (unsigned int)fit_capacity);
    int count = (int)SDL_min(visible_count, (unsigned int)slots);
    int per_slot = (available - (slots - 1) * spacing) / slots;
    int original = SDL_max(1, SDL_min((int)((Sint64)height * 34 / 100), per_slot));
    int scale = SDL_clamp(appearance->card_scale, TV_CARD_SCALE_MIN, TV_CARD_SCALE_MAX);
    int card_w = SDL_max(1, SDL_min(per_slot, (int)((Sint64)original * scale / 100)));
    int card_h = SDL_max(1, (int)((Sint64)card_w * 10 / 17));
    int row_w = count * card_w + (count - 1) * spacing;
    int vertical_margin = (int)((Sint64)height * 6 / 100);
    int lowest_y = height - vertical_margin - card_h;
    int y = lowest_y;
    if (appearance->row_position != ROW_POSITION_AUTO) {
        int position = SDL_clamp(appearance->row_position, 0, 100);
        int center = (int)((Sint64)height * position / 100);
        y = SDL_clamp(center - card_h / 2, vertical_margin, lowest_y);
    }
    if (card_width != NULL)
        *card_width = card_w;
    if (gap != NULL)
        *gap = spacing;
    return (SDL_Rect){(width - row_w) / 2, y, row_w, card_h};
}

static void update_tv_geometry(Geometry *geometry)
{
    SDL_Rect row = measure_tv_row(geometry, config.max_buttons, config.max_buttons,
        &config.appearance, &tv.card_width, &tv.gap);
    int width = geometry->screen_width;
    int height = geometry->screen_height;
    tv.margin = width / 20;
    tv.card_height = row.h;
    geometry->y_margin = row.y;
    geometry->x_advance = tv.card_width + tv.gap;

    tv.floating_scrim = config.appearance.row_position != ROW_POSITION_AUTO;
    if (tv.floating_scrim) {
        int fade = height / 5;
        tv.scrim_rect = (SDL_Rect){0, row.y - fade, width, row.h + 2 * fade};
    }
    else
        tv.scrim_rect = (SDL_Rect){0, height / 2, width, height - height / 2};
}

void size_tv_row(Geometry *geometry)
{
    int width = geometry->screen_width;
    int height = geometry->screen_height;
    int available = width - 2 * (width / 20);
    int gap = SDL_max(2, width / 64);
    int minimum = SDL_max(1, height / 9);
    int capacity = SDL_max(1, (available + gap) / (minimum + gap));
    config.max_buttons = SDL_max(1u, SDL_min(config.max_buttons, (unsigned int)capacity));
    update_tv_geometry(geometry);
}

static SDL_Texture *card_texture(bool focused, int opacity)
{
    char svg[400];
    int border = focused ? SDL_max(1, tv.card_height / 44) : 0;
    int inset = (border + 1) / 2;
    int fill_opacity = focused ? 98 : opacity;
    SDL_snprintf(svg, sizeof(svg),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\">"
        "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" "
        "fill=\"%s\" fill-opacity=\"%d.%02d\" stroke=\"#C1DAFF\" stroke-width=\"%d\"/></svg>",
        tv.card_width, tv.card_height, inset, inset, SDL_max(1, tv.card_width - 2 * inset),
        SDL_max(1, tv.card_height - 2 * inset), SDL_max(1, tv.card_height / 8),
        focused ? "#293C56" : "#111926", fill_opacity / 100, fill_opacity % 100, border);
    return rasterize_svg(svg, -1, -1, NULL);
}

static bool replace_card_textures(void)
{
    int opacity = SDL_clamp(config.appearance.card_opacity, 0, 100);
    bool resize = tv.focused == NULL || tv.skin_width != tv.card_width || tv.skin_height != tv.card_height;
    if (tv.card != NULL && !resize && tv.skin_opacity == opacity)
        return true;

    SDL_Texture *card = card_texture(false, opacity);
    if (card == NULL)
        return false;
    SDL_Texture *focused = resize ? card_texture(true, opacity) : NULL;
    if (resize && focused == NULL) {
        SDL_DestroyTexture(card);
        return false;
    }

    if (tv.card != NULL)
        SDL_DestroyTexture(tv.card);
    tv.card = card;
    tv.skin_opacity = opacity;
    if (resize) {
        if (tv.focused != NULL)
            SDL_DestroyTexture(tv.focused);
        tv.focused = focused;
        tv.skin_width = tv.card_width;
        tv.skin_height = tv.card_height;
    }
    return true;
}

bool apply_tv_appearance(void)
{
    update_tv_geometry(&geo);
    reset_tv_motion();
    // On allocation failure the renderer stretches the previous skins to the
    // new geometry. Replace a resized set only once both textures are ready.
    return replace_card_textures();
}

bool init_tv_ui(void)
{
    if (!replace_card_textures())
        return false;

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
    return tv.scrim != NULL;
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
    if (tv.floating_scrim) {
        // Fade in on both sides of a manually positioned row.
        SDL_Rect above = tv.scrim_rect;
        above.h /= 2;
        SDL_Rect below = tv.scrim_rect;
        below.y += above.h;
        below.h -= above.h;
        SDL_RenderCopy(renderer, tv.scrim, NULL, &above);
        SDL_RenderCopyEx(renderer, tv.scrim, NULL, &below, 0, NULL, SDL_FLIP_VERTICAL);
    }
    else
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
