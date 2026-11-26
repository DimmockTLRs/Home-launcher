#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <launcher_config.h>
#include "launcher.h"
#include "app_editor.h"
#include "config_save.h"
#include "image.h"
#include "photo_gallery.h"
#include "settings_ui.h"
#include "util.h"

extern Config config;
extern Geometry geo;
extern TextInfo title_info;
extern bool apply_background_settings(const BackgroundSettings *settings, char *error, size_t error_size);
extern bool open_launcher_app_editor(TTF_Font *heading, TTF_Font *body, TTF_Font *small);
extern void request_launcher_redraw(void);

typedef enum {
    ROW_MODE,
    ROW_COLLAGE,
    ROW_DURATION,
    ROW_TRANSITION,
    ROW_SAVE,
    ROW_BACK,
    ROW_MANAGE,
    ROW_APPS,
    ROW_COUNT
} SettingsRow;

typedef struct {
    SDL_Texture *texture;
    int width;
    int height;
} SettingsText;

static const char *const labels[] = {
    "Background mode", "Collage layout", "Photo duration", "Transition duration"
};
static const char *const modes[] = { "Solid colour", "Single image", "Slideshow", "Collage" };
static const ModeBackground mode_values[] = { BACKGROUND_COLOR, BACKGROUND_IMAGE, BACKGROUND_SLIDESHOW, BACKGROUND_COLLAGE };
static const char *const collage_layouts[] = { "Auto", "Split", "Feature", "Grid" };

static const SDL_Color light = { 239, 245, 255, 255 };
static const SDL_Color muted = { 162, 180, 205, 255 };
static const SDL_Color accent = { 193, 218, 255, 255 };
static const SDL_Color caution = { 255, 208, 143, 255 };

static struct {
    bool open;
    bool saving;
    int cursor_before;
    int selected;
    double scale;
    int origin_x;
    int origin_y;
    SDL_Rect rows[ROW_COUNT];
    TTF_Font *body_font;
    TTF_Font *small_font;
    TTF_Font *heading_font;
    BackgroundSettings original;
    BackgroundSettings draft;
    char *image_path;
    char *folder_path;
    SettingsText section;
    SettingsText title;
    SettingsText labels[ROW_SAVE];
    SettingsText values[ROW_SAVE];
    SettingsText left;
    SettingsText right;
    SettingsText save;
    SettingsText back;
    SettingsText manage;
    SettingsText apps;
    SettingsText help;
    SettingsText notice;
    SDL_Texture *row;
    SDL_Texture *focus_row;
} ui;

static int scaled(int value)
{
    return (int)(ui.scale * (double)value + 0.5);
}

static SDL_Rect bounds(int x, int y, int width, int height)
{
    return (SDL_Rect){ ui.origin_x + scaled(x), ui.origin_y + scaled(y), scaled(width), scaled(height) };
}

static void set_text(SettingsText *target, TTF_Font *font, const char *value, SDL_Color color)
{
    SDL_DestroyTexture(target->texture);
    *target = (SettingsText){0};
    if (value[0] == '\0')
        return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, value, color);
    if (surface == NULL)
        return;
    target->width = surface->w;
    target->height = surface->h;
    target->texture = load_texture(surface);
}

static void draw_text(SDL_Renderer *renderer, const SettingsText *text, SDL_Rect region, bool centered)
{
    if (text->texture == NULL || region.w <= 0 || region.h <= 0)
        return;
    double scale = SDL_min(1.0, SDL_min((double)region.w / text->width, (double)region.h / text->height));
    int width = SDL_max(1, (int)(text->width * scale));
    int height = SDL_max(1, (int)(text->height * scale));
    SDL_Rect rect = {
        region.x + (centered ? (region.w - width) / 2 : 0),
        region.y + (region.h - height) / 2,
        width, height
    };
    SDL_RenderCopy(renderer, text->texture, NULL, &rect);
}

static SDL_Texture *row_skin(int width, int height, bool focused)
{
    char svg[390];
    int border = focused ? SDL_max(2, scaled(3)) : 0;
    int margin = (border + 1) / 2;
    SDL_snprintf(svg, sizeof(svg),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\">"
        "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" "
        "fill=\"%s\" stroke=\"#C1DAFF\" stroke-width=\"%d\"/></svg>",
        width, height, margin, margin, SDL_max(1, width - margin * 2),
        SDL_max(1, height - margin * 2), SDL_max(1, scaled(16)),
        focused ? "#2A4160" : "#182438", border);
    return rasterize_svg(svg, -1, -1, NULL);
}

static TTF_Font *settings_font(const char *path, int size)
{
    TTF_Font *font = path != NULL ? TTF_OpenFont(path, SDL_max(12, scaled(size))) : NULL;
    return font != NULL ? font : title_info.font;
}

static int mode_index(void)
{
    for (size_t i = 0; i < SDL_arraysize(mode_values); i++) {
        if (mode_values[i] == ui.draft.mode)
            return (int)i;
    }
    return -1;
}

static bool edited(void)
{
    return ui.draft.mode != ui.original.mode ||
        ui.draft.layout != ui.original.layout ||
        ui.draft.image_duration != ui.original.image_duration ||
        ui.draft.transition_time != ui.original.transition_time ||
        strcmp(ui.image_path != NULL ? ui.image_path : "", ui.original.image_path) != 0 ||
        strcmp(ui.folder_path != NULL ? ui.folder_path : "", ui.original.folder_path) != 0;
}

static void set_notice(const char *message, SDL_Color color)
{
    set_text(&ui.notice, ui.small_font, message, color);
}

static void refresh_notice(void)
{
    if (ui.draft.mode == BACKGROUND_IMAGE &&
        (ui.image_path == NULL || ui.image_path[0] == '\0'))
        set_notice("Choose a single image in Manage photos.", caution);
    else if ((ui.draft.mode == BACKGROUND_SLIDESHOW || ui.draft.mode == BACKGROUND_COLLAGE) &&
        (ui.folder_path == NULL || ui.folder_path[0] == '\0'))
        set_notice("Choose a photo folder in Manage photos.", caution);
    else
        set_notice(edited() ? "Save to apply your changes." : "", muted);
}

static void refresh_value(int row)
{
    char value[40];
    switch (row) {
        case ROW_MODE: {
            int index = mode_index();
            SDL_strlcpy(value, index < 0 ? "Transparent" : modes[index], sizeof(value));
            break;
        }
        case ROW_COLLAGE:
            SDL_strlcpy(value, collage_layouts[ui.draft.layout], sizeof(value));
            break;
        case ROW_DURATION:
            SDL_snprintf(value, sizeof(value), "%u s", (unsigned int)(ui.draft.image_duration / 1000));
            break;
        case ROW_TRANSITION:
            if (ui.draft.transition_time == 0)
                SDL_strlcpy(value, "Off", sizeof(value));
            else
                SDL_snprintf(value, sizeof(value), "%.2f s", (double)ui.draft.transition_time / 1000.0);
            break;
        default:
            return;
    }
    set_text(&ui.values[row], ui.body_font, value, accent);
}

bool settings_open(void)
{
    if (ui.open)
        return true;
    if (geo.screen_width <= 0 || geo.screen_height <= 0 || title_info.font == NULL)
        return false;
    const char *image = config.background_image != NULL ? config.background_image : "";
    const char *folder = config.slideshow_directory != NULL ? config.slideshow_directory : "";
    char *image_copy = strdup(image);
    char *folder_copy = strdup(folder);
    if (image_copy == NULL || folder_copy == NULL) {
        free(image_copy);
        free(folder_copy);
        return false;
    }
    ui.image_path = image_copy;
    ui.folder_path = folder_copy;

    ui.scale = SDL_min((double)geo.screen_width / 1920.0, (double)geo.screen_height / 1080.0);
    ui.origin_x = (geo.screen_width - scaled(1920)) / 2;
    ui.origin_y = (geo.screen_height - scaled(1080)) / 2;
    for (int i = 0; i < ROW_SAVE; i++)
        ui.rows[i] = bounds(252, 262 + 112 * i, 1416, 96);
    ui.rows[ROW_BACK] = bounds(252, 826, 240, 76);
    ui.rows[ROW_MANAGE] = bounds(520, 826, 348, 76);
    ui.rows[ROW_SAVE] = bounds(1320, 826, 348, 76);
    ui.rows[ROW_APPS] = bounds(1170, 133, 498, 76);

    char font_dir[MAX_PATH_CHARS + 1];
    const char *prefixes[] = {
        join_paths(font_dir, sizeof(font_dir), 3, config.exe_path, PATH_ASSETS_EXE, PATH_FONTS_EXE),
#ifdef __unix__
        PATH_FONTS_SYSTEM
#else
        PATH_FONTS_RELATIVE
#endif
    };
    char *font_path = find_file("Inter-Regular.ttf", (int)SDL_arraysize(prefixes), prefixes);
    ui.body_font = settings_font(font_path, 35);
    ui.small_font = settings_font(font_path, 25);
    ui.heading_font = settings_font(font_path, 64);
    free(font_path);

    ui.original = (BackgroundSettings){
        .mode = config.background_mode,
        .layout = config.collage_layout,
        .image_duration = config.slideshow_image_duration,
        .transition_time = config.slideshow_transition_time,
        .image_path = image,
        .folder_path = folder
    };
    ui.draft = ui.original;
    set_text(&ui.section, ui.small_font, "Settings", muted);
    set_text(&ui.title, ui.heading_font, "Backgrounds", light);
    set_text(&ui.left, ui.body_font, "\xc2\xab", accent);
    set_text(&ui.right, ui.body_font, "\xc2\xbb", accent);
    set_text(&ui.save, ui.body_font, "Save and return", light);
    set_text(&ui.back, ui.body_font, "Back", light);
    set_text(&ui.manage, ui.body_font, "Manage photos", light);
    set_text(&ui.apps, ui.body_font, "Applications and icons", light);
    set_text(&ui.help, ui.small_font, "Arrows  Navigate or adjust     Enter / A  Select     Esc / B  Back", muted);
    for (int i = 0; i < ROW_SAVE; i++) {
        set_text(&ui.labels[i], ui.body_font, labels[i], light);
        refresh_value(i);
    }
    if (ui.rows[0].w >= 4 && ui.rows[0].h >= 4) {
        ui.row = row_skin(ui.rows[0].w, ui.rows[0].h, false);
        ui.focus_row = row_skin(ui.rows[0].w, ui.rows[0].h, true);
    }
    refresh_notice();
    ui.open = true;
    ui.cursor_before = SDL_ShowCursor(SDL_QUERY);
    SDL_ShowCursor(SDL_ENABLE);
    request_launcher_redraw();
    return true;
}

bool settings_is_open(void)
{
    return ui.open;
}

static void adjust(int row, int direction)
{
    switch (row) {
        case ROW_MODE: {
            int index = mode_index();
            int count = (int)SDL_arraysize(mode_values);
            index = index < 0 ? (direction > 0 ? 0 : count - 1) : (index + direction + count) % count;
            ui.draft.mode = mode_values[index];
            break;
        }
        case ROW_COLLAGE: {
            int count = (int)SDL_arraysize(collage_layouts);
            ui.draft.layout = (CollageLayout)(((int)ui.draft.layout + direction + count) % count);
            break;
        }
        case ROW_DURATION:
            ui.draft.image_duration = (Uint32)SDL_clamp((int)ui.draft.image_duration + 5000 * direction,
                MIN_SLIDESHOW_IMAGE_DURATION, MAX_SLIDESHOW_IMAGE_DURATION);
            break;
        case ROW_TRANSITION:
            ui.draft.transition_time = (Uint32)SDL_clamp((int)ui.draft.transition_time + 250 * direction,
                0, MAX_SLIDESHOW_TRANSITION_TIME);
            break;
        default:
            return;
    }
    refresh_value(row);
    refresh_notice();
}

static void close_settings(void)
{
    settings_quit();
    request_launcher_redraw();
}

static bool open_application_editor(void)
{
    if (open_launcher_app_editor(ui.heading_font, ui.body_font, ui.small_font))
        return true;
    set_notice("Could not open the application editor.", caution);
    return false;
}

bool settings_open_home_assistant(int index)
{
    if (!settings_open())
        return false;
    ui.selected = ROW_APPS;
    if (open_application_editor())
        app_editor_configure_home_assistant(index);
    return true;
}

static void activate(int direction)
{
    if (ui.selected < ROW_SAVE) {
        adjust(ui.selected, direction);
        return;
    }
    if (ui.selected == ROW_MANAGE) {
        open_photo_gallery(&ui.draft.mode, &ui.folder_path, &ui.image_path,
            ui.heading_font, ui.body_font, ui.small_font);
        return;
    }
    if (ui.selected == ROW_APPS) {
        open_application_editor();
        return;
    }
    if (ui.selected == ROW_BACK || !edited()) {
        close_settings();
        return;
    }

    char error[512] = {0};
    ui.draft.image_path = ui.image_path;
    ui.draft.folder_path = ui.folder_path;
    if (apply_background_settings(&ui.draft, error, sizeof(error))) {
        ui.saving = true;
        set_notice("Saving changes...", muted);
    }
    else {
        set_notice(error[0] != '\0' ? error : "Could not save the changes.", caution);
    }
}

void settings_save_completed(bool success, const char *error)
{
    if (!ui.open || !ui.saving)
        return;
    ui.saving = false;
    if (success)
        close_settings();
    else {
        set_notice(error != NULL && error[0] != '\0' ? error : "Could not save the changes.", caution);
        request_launcher_redraw();
    }
}

void settings_app_save_completed(bool success, const char *error)
{
    app_editor_save_completed(success, error);
    if (!success)
        return;
    if (edited()) {
        set_notice("Application cards saved. Save Backgrounds to apply its other changes.", muted);
        request_launcher_redraw();
    }
    else
        close_settings();
}

static void return_from_gallery(void)
{
    if (!photo_gallery_is_open()) {
        refresh_value(ROW_MODE);
        refresh_notice();
    }
}

void settings_command(const char *command)
{
    if (!ui.open || ui.saving || command == NULL)
        return;
    if (app_editor_is_open()) {
        app_editor_command(command);
        return;
    }
    if (photo_gallery_is_open()) {
        photo_gallery_command(command);
        return_from_gallery();
        return;
    }

    if (strcmp(command, SCMD_UP) == 0)
        ui.selected = (ui.selected + ROW_COUNT - 1) % ROW_COUNT;
    else if (strcmp(command, SCMD_DOWN) == 0)
        ui.selected = (ui.selected + 1) % ROW_COUNT;
    else if (strcmp(command, SCMD_LEFT) == 0 || strcmp(command, SCMD_RIGHT) == 0) {
        int direction = strcmp(command, SCMD_LEFT) == 0 ? -1 : 1;
        if (ui.selected < ROW_SAVE)
            adjust(ui.selected, direction);
        else if (ui.selected == ROW_MANAGE)
            ui.selected = direction > 0 ? ROW_SAVE : ROW_BACK;
        else if ((ui.selected == ROW_BACK && direction > 0) || (ui.selected == ROW_SAVE && direction < 0))
            ui.selected = ROW_MANAGE;
    }
    else if (strcmp(command, SCMD_SELECT) == 0)
        activate(1);
    else if (strcmp(command, SCMD_BACK) == 0 || strcmp(command, SCMD_HOME) == 0 ||
        strcmp(command, SCMD_QUIT) == 0)
        close_settings();
    else
        return;
    request_launcher_redraw();
}

void settings_mouse(int x, int y, bool click)
{
    if (!ui.open || ui.saving)
        return;
    if (app_editor_is_open()) {
        app_editor_mouse(x, y, click);
        return;
    }
    if (photo_gallery_is_open()) {
        photo_gallery_mouse(x, y, click);
        return_from_gallery();
        return;
    }
    SDL_Point mouse = { x, y };
    for (int i = 0; i < ROW_COUNT; i++) {
        if (SDL_PointInRect(&mouse, &ui.rows[i])) {
            bool changed = ui.selected != i;
            ui.selected = i;
            if (click) {
                SDL_Rect decrement = bounds(1140, 262 + 112 * i, 72, 96);
                activate(i < ROW_SAVE && SDL_PointInRect(&mouse, &decrement) ? -1 : 1);
            }
            if (changed || click)
                request_launcher_redraw();
            return;
        }
    }
}

bool settings_is_typing(void)
{
    return app_editor_is_typing();
}

void settings_key(SDL_Keycode key)
{
    app_editor_key(key);
}

void settings_text(const char *text)
{
    if (app_editor_is_typing())
        app_editor_text(text);
}

void settings_pump(void)
{
    pump_photo_gallery();
    app_editor_pump();
}

static void fill(SDL_Renderer *renderer, const SDL_Rect *rect, SDL_Color color)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, rect);
}

static void draw_action(SDL_Renderer *renderer, int index, const SettingsText *text)
{
    SDL_Rect rect = ui.rows[index];
    fill(renderer, &rect, index == ROW_SAVE ? (SDL_Color){ 43, 70, 105, 255 } :
        (SDL_Color){ 25, 38, 57, 255 });
    if (ui.selected == index) {
        SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, accent.a);
        for (int i = 0; i < SDL_max(1, scaled(3)); i++) {
            SDL_Rect border = { rect.x + i, rect.y + i, rect.w - i * 2, rect.h - i * 2 };
            if (border.w > 0 && border.h > 0)
                SDL_RenderDrawRect(renderer, &border);
        }
    }
    draw_text(renderer, text, rect, true);
}

void settings_draw(SDL_Renderer *renderer)
{
    if (!ui.open)
        return;
    if (app_editor_is_open()) {
        app_editor_draw(renderer);
        return;
    }
    if (photo_gallery_is_open()) {
        draw_photo_gallery(renderer);
        return;
    }

    SDL_Color previous;
    SDL_GetRenderDrawColor(renderer, &previous.r, &previous.g, &previous.b, &previous.a);
    fill(renderer, NULL, (SDL_Color){ 11, 17, 29, 255 });

    SDL_Rect panel = bounds(220, 233, 1480, 698);
    fill(renderer, &panel, (SDL_Color){ 14, 23, 38, 255 });
    draw_text(renderer, &ui.section, bounds(252, 88, 950, 42), false);
    draw_text(renderer, &ui.title, bounds(252, 122, 1100, 92), false);
    draw_action(renderer, ROW_APPS, &ui.apps);

    for (int i = 0; i < ROW_SAVE; i++) {
        SDL_Rect rect = ui.rows[i];
        SDL_Texture *skin = ui.selected == i ? ui.focus_row : ui.row;
        if (skin != NULL)
            SDL_RenderCopy(renderer, skin, NULL, &rect);
        else
            fill(renderer, &rect, ui.selected == i ? (SDL_Color){ 42, 65, 96, 255 } :
                (SDL_Color){ 24, 36, 56, 255 });
        int y = 262 + 112 * i;
        draw_text(renderer, &ui.labels[i], bounds(292, y, 815, 96), false);
        draw_text(renderer, &ui.values[i], bounds(1212, y, 364, 96), true);
        draw_text(renderer, &ui.left, bounds(1140, y, 72, 96), true);
        draw_text(renderer, &ui.right, bounds(1576, y, 72, 96), true);
    }
    draw_text(renderer, &ui.notice, bounds(252, 721, 1416, 94), false);
    draw_action(renderer, ROW_BACK, &ui.back);
    draw_action(renderer, ROW_MANAGE, &ui.manage);
    draw_action(renderer, ROW_SAVE, &ui.save);
    draw_text(renderer, &ui.help, bounds(252, 954, 1416, 60), false);
    SDL_SetRenderDrawColor(renderer, previous.r, previous.g, previous.b, previous.a);
}

void settings_quit(void)
{
    if (!ui.open)
        return;

    app_editor_quit();
    free(ui.image_path);
    free(ui.folder_path);
    if (ui.cursor_before >= 0)
        SDL_ShowCursor(ui.cursor_before);
    for (int i = 0; i < ROW_SAVE; i++) {
        SDL_DestroyTexture(ui.labels[i].texture);
        SDL_DestroyTexture(ui.values[i].texture);
    }
    SDL_DestroyTexture(ui.section.texture);
    SDL_DestroyTexture(ui.title.texture);
    SDL_DestroyTexture(ui.left.texture);
    SDL_DestroyTexture(ui.right.texture);
    SDL_DestroyTexture(ui.save.texture);
    SDL_DestroyTexture(ui.back.texture);
    SDL_DestroyTexture(ui.manage.texture);
    SDL_DestroyTexture(ui.apps.texture);
    SDL_DestroyTexture(ui.help.texture);
    SDL_DestroyTexture(ui.notice.texture);
    SDL_DestroyTexture(ui.row);
    SDL_DestroyTexture(ui.focus_row);

    if (ui.body_font != title_info.font)
        TTF_CloseFont(ui.body_font);
    if (ui.small_font != title_info.font)
        TTF_CloseFont(ui.small_font);
    if (ui.heading_font != title_info.font)
        TTF_CloseFont(ui.heading_font);
    memset(&ui, 0, sizeof(ui));
}
