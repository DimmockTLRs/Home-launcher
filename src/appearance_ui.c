#include <stdbool.h>
#include <string.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include "appearance_ui.h"
#include "image.h"

extern Geometry geo;
extern void request_launcher_redraw(void);
extern bool apply_appearance_settings(const AppearanceSettings *settings, char *error, size_t size);
extern void draw_appearance_preview(SDL_Renderer *renderer, SDL_Rect viewport, const AppearanceSettings *settings);

typedef enum {
    ROW_POSITION, ROW_SIZE, ROW_OPACITY, ROW_BACK, ROW_RESET, ROW_SAVE, ROW_COUNT
} AppearanceRow;

typedef struct {
    SDL_Texture *texture;
    int width;
    int height;
} AppearanceText;

static const char *const row_labels[ROW_COUNT] = {
    "Row position", "Card size", "Card opacity", "Back", "Reset to defaults", "Save and return"
};
static const SDL_Color light = {239, 245, 255, 255};
static const SDL_Color muted = {162, 180, 205, 255};
static const SDL_Color accent = {193, 218, 255, 255};
static const SDL_Color warning = {255, 208, 143, 255};

static struct {
    bool open;
    bool saving;
    bool tv_layout;
    double scale;
    int origin_x;
    int origin_y;
    AppearanceRow focus;
    AppearanceSettings original;
    AppearanceSettings draft;
    TTF_Font *body_font;
    TTF_Font *small_font;
    SDL_Rect rows[ROW_COUNT];
    AppearanceText labels[ROW_COUNT];
    AppearanceText values[ROW_BACK];
    AppearanceText section;
    AppearanceText title;
    AppearanceText preview;
    AppearanceText previous;
    AppearanceText next;
    AppearanceText help;
    AppearanceText notice;
} ui;

static int scaled(int value)
{
    return (int)(value * ui.scale + 0.5);
}

static SDL_Rect bounds(int x, int y, int w, int h)
{
    return (SDL_Rect){ui.origin_x + scaled(x), ui.origin_y + scaled(y), scaled(w), scaled(h)};
}

static void set_text(AppearanceText *text, TTF_Font *font, const char *value, SDL_Color color)
{
    SDL_DestroyTexture(text->texture);
    *text = (AppearanceText){0};
    if (*value == '\0')
        return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, value, color);
    if (surface != NULL) {
        text->width = surface->w;
        text->height = surface->h;
        text->texture = load_texture(surface);
    }
}

static void draw_text(SDL_Renderer *renderer, const AppearanceText *text, SDL_Rect area, bool centered)
{
    if (text->texture == NULL || area.w <= 0 || area.h <= 0)
        return;
    double scale = SDL_min(1.0, SDL_min((double)area.w / text->width, (double)area.h / text->height));
    int w = SDL_max(1, (int)(text->width * scale));
    int h = SDL_max(1, (int)(text->height * scale));
    SDL_Rect target = {area.x + (centered ? (area.w - w) / 2 : 0), area.y + (area.h - h) / 2, w, h};
    SDL_RenderCopy(renderer, text->texture, NULL, &target);
}

static void fill(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color color)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

static void draw_row(SDL_Renderer *renderer, SDL_Rect rect, bool focused)
{
    fill(renderer, rect, focused ? (SDL_Color){42, 65, 96, 255} : (SDL_Color){24, 36, 56, 255});
    if (!focused)
        return;
    SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, accent.a);
    for (int i = 0; i < SDL_max(1, scaled(3)) && rect.w > 0 && rect.h > 0; i++) {
        SDL_RenderDrawRect(renderer, &rect);
        rect.x++;
        rect.y++;
        rect.w -= 2;
        rect.h -= 2;
    }
}

static AppearanceRow last_control(void)
{
    return ui.tv_layout ? ROW_OPACITY : ROW_POSITION;
}

static int *row_value(AppearanceRow row)
{
    if (row == ROW_SIZE)
        return &ui.draft.card_scale;
    if (row == ROW_OPACITY)
        return &ui.draft.card_opacity;
    return &ui.draft.row_position;
}

static void update_value(AppearanceRow row)
{
    int value = *row_value(row);
    char text[24];
    if (row == ROW_POSITION && value == ROW_POSITION_AUTO)
        SDL_strlcpy(text, "Auto", sizeof(text));
    else
        SDL_snprintf(text, sizeof(text), "%d%%", value);
    set_text(&ui.values[row], ui.body_font, text, light);
}

static bool edited(void)
{
    return ui.draft.row_position != ui.original.row_position ||
        ui.draft.card_scale != ui.original.card_scale ||
        ui.draft.card_opacity != ui.original.card_opacity;
}

static void set_notice(const char *message, SDL_Color color)
{
    set_text(&ui.notice, ui.small_font, message, color);
}

static void update_notice(void)
{
    set_notice(edited() ? "Save to apply the preview on Home." : "", muted);
}

bool appearance_ui_open(const AppearanceSettings *settings, bool tv_layout,
    TTF_Font *heading, TTF_Font *body, TTF_Font *small)
{
    if (ui.open)
        return true;
    if (settings == NULL || heading == NULL || body == NULL || small == NULL ||
        geo.screen_width <= 0 || geo.screen_height <= 0)
        return false;

    ui.open = true;
    ui.tv_layout = tv_layout;
    ui.original = ui.draft = *settings;
    ui.body_font = body;
    ui.small_font = small;
    ui.scale = SDL_min((double)geo.screen_width / 1920, (double)geo.screen_height / 1080);
    ui.origin_x = (geo.screen_width - scaled(1920)) / 2;
    ui.origin_y = (geo.screen_height - scaled(1080)) / 2;
    for (AppearanceRow i = ROW_POSITION; i <= last_control(); i++) {
        ui.rows[i] = bounds(252, 270 + 112 * i, 730, 94);
        update_value(i);
    }
    ui.rows[ROW_BACK] = bounds(252, 826, 240, 76);
    ui.rows[ROW_RESET] = bounds(520, 826, 370, 76);
    ui.rows[ROW_SAVE] = bounds(1320, 826, 348, 76);
    for (int i = 0; i < ROW_COUNT; i++) {
        if (ui.rows[i].w > 0)
            set_text(&ui.labels[i], body, row_labels[i], light);
    }
    set_text(&ui.section, small, "Settings / Applications and icons", muted);
    set_text(&ui.title, heading, "Icon row", light);
    set_text(&ui.preview, small, "Home layout preview", muted);
    set_text(&ui.previous, body, "\xc2\xab", accent);
    set_text(&ui.next, body, "\xc2\xbb", accent);
    set_text(&ui.help, small, "Up / Down  Navigate     Left / Right  Adjust     Enter / A  Select     Esc / B  Back", muted);
    request_launcher_redraw();
    return true;
}

bool appearance_ui_is_open(void)
{
    return ui.open;
}

static void close_page(void)
{
    appearance_ui_quit();
    request_launcher_redraw();
}

static void adjust(AppearanceRow row, int direction)
{
    int *value = row_value(row);
    int next = *value;
    if (row == ROW_POSITION) {
        int minimum = ui.tv_layout ? 0 : CLASSIC_ROW_MIN;
        int maximum = ui.tv_layout ? 100 : CLASSIC_ROW_MAX;
        if (ui.tv_layout && *value == ROW_POSITION_AUTO)
            next = direction > 0 ? 90 : 80;
        else
            next = SDL_clamp(*value + direction * 5, minimum, maximum);
    }
    else if (row == ROW_SIZE)
        next = SDL_clamp(*value + direction * 5, TV_CARD_SCALE_MIN, TV_CARD_SCALE_MAX);
    else if (row == ROW_OPACITY)
        next = SDL_clamp(*value + direction * 5, 0, 100);

    if (next != *value) {
        *value = next;
        update_value(row);
        update_notice();
    }
}

static void reset(void)
{
    ui.draft.row_position = ui.tv_layout ? ROW_POSITION_AUTO : CLASSIC_ROW_DEFAULT;
    if (ui.tv_layout) {
        ui.draft.card_scale = TV_CARD_SCALE_DEFAULT;
        ui.draft.card_opacity = TV_CARD_OPACITY_DEFAULT;
    }
    for (AppearanceRow row = ROW_POSITION; row <= last_control(); row++)
        update_value(row);
    update_notice();
}

static void save(void)
{
    if (!edited()) {
        close_page();
        return;
    }
    char error[256] = {0};
    if (apply_appearance_settings(&ui.draft, error, sizeof(error))) {
        ui.saving = true;
        set_notice("Saving changes...", muted);
    }
    else
        set_notice(error[0] != '\0' ? error : "Could not save the icon row.", warning);
}

static void activate(AppearanceRow row)
{
    if (row < ROW_BACK)
        adjust(row, 1);
    else if (row == ROW_BACK)
        close_page();
    else if (row == ROW_RESET)
        reset();
    else if (row == ROW_SAVE)
        save();
}

void appearance_ui_command(const char *command)
{
    if (!ui.open || ui.saving || command == NULL)
        return;
    bool left = strcmp(command, SCMD_LEFT) == 0;
    if (left || strcmp(command, SCMD_RIGHT) == 0) {
        int direction = left ? -1 : 1;
        if (ui.focus < ROW_BACK)
            adjust(ui.focus, direction);
        else
            ui.focus = SDL_clamp((int)ui.focus + direction, ROW_BACK, ROW_SAVE);
    }
    else if (strcmp(command, SCMD_UP) == 0) {
        if (ui.focus >= ROW_BACK)
            ui.focus = last_control();
        else if (ui.focus == ROW_POSITION)
            ui.focus = ROW_SAVE;
        else
            ui.focus--;
    }
    else if (strcmp(command, SCMD_DOWN) == 0) {
        if (ui.focus >= ROW_BACK)
            ui.focus = ROW_POSITION;
        else if (ui.focus == last_control())
            ui.focus = ROW_BACK;
        else
            ui.focus++;
    }
    else if (strcmp(command, SCMD_SELECT) == 0)
        activate(ui.focus);
    else if (strcmp(command, SCMD_BACK) == 0 || strcmp(command, SCMD_HOME) == 0 ||
        strcmp(command, SCMD_QUIT) == 0)
        close_page();
    else
        return;
    if (ui.open)
        request_launcher_redraw();
}

void appearance_ui_mouse(int x, int y, bool click)
{
    if (!ui.open || ui.saving)
        return;
    SDL_Point point = {x, y};
    for (int row = 0; row < ROW_COUNT; row++) {
        const SDL_Rect *rect = &ui.rows[row];
        if (!SDL_PointInRect(&point, rect))
            continue;
        bool changed = ui.focus != (AppearanceRow)row;
        ui.focus = row;
        if (click) {
            if (row >= ROW_BACK)
                activate(row);
            else if (x >= rect->x + scaled(422) && x < rect->x + scaled(490))
                adjust(row, -1);
            else if (x >= rect->x + scaled(628))
                adjust(row, 1);
        }
        if (ui.open && (changed || click))
            request_launcher_redraw();
        return;
    }
}

void appearance_ui_draw(SDL_Renderer *renderer)
{
    if (!ui.open)
        return;
    SDL_Color old;
    SDL_GetRenderDrawColor(renderer, &old.r, &old.g, &old.b, &old.a);
    fill(renderer, bounds(0, 0, 1920, 1080), (SDL_Color){11, 17, 29, 255});
    draw_text(renderer, &ui.section, bounds(252, 71, 1416, 36), false);
    draw_text(renderer, &ui.title, bounds(252, 106, 1416, 74), false);
    for (int i = 0; i < ROW_COUNT; i++) {
        SDL_Rect row = ui.rows[i];
        if (row.w <= 0 || row.h <= 0)
            continue;
        draw_row(renderer, row, ui.focus == (AppearanceRow)i && !ui.saving);
        if (i >= ROW_BACK) {
            draw_text(renderer, &ui.labels[i], row, true);
            continue;
        }
        int y = 270 + 112 * i;
        draw_text(renderer, &ui.labels[i], bounds(280, y, 380, 94), false);
        draw_text(renderer, &ui.previous, bounds(674, y, 68, 94), true);
        draw_text(renderer, &ui.values[i], bounds(742, y, 138, 94), true);
        draw_text(renderer, &ui.next, bounds(880, y, 84, 94), true);
    }
    draw_row(renderer, bounds(1020, 270, 648, 428), false);
    draw_text(renderer, &ui.preview, bounds(1046, 286, 596, 47), false);
    draw_appearance_preview(renderer, bounds(1046, 344, 596, 335), &ui.draft);
    draw_text(renderer, &ui.notice, bounds(252, 920, 1416, 37), false);
    draw_text(renderer, &ui.help, bounds(252, 972, 1416, 37), false);
    SDL_SetRenderDrawColor(renderer, old.r, old.g, old.b, old.a);
}

void appearance_ui_save_completed(bool success, const char *error)
{
    if (!ui.open || !ui.saving)
        return;
    ui.saving = false;
    if (success)
        close_page();
    else {
        set_notice(error != NULL && *error != '\0' ? error : "Could not save the icon row.", warning);
        request_launcher_redraw();
    }
}

void appearance_ui_quit(void)
{
    for (int i = 0; i < ROW_COUNT; i++)
        SDL_DestroyTexture(ui.labels[i].texture);
    for (int i = 0; i < ROW_BACK; i++)
        SDL_DestroyTexture(ui.values[i].texture);
    SDL_DestroyTexture(ui.section.texture);
    SDL_DestroyTexture(ui.title.texture);
    SDL_DestroyTexture(ui.preview.texture);
    SDL_DestroyTexture(ui.previous.texture);
    SDL_DestroyTexture(ui.next.texture);
    SDL_DestroyTexture(ui.help.texture);
    SDL_DestroyTexture(ui.notice.texture);
    memset(&ui, 0, sizeof(ui));
}
