#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include "image.h"
#include "photo_files.h"
#include "photo_jobs.h"
#include "photo_gallery.h"
#include "tv_ui.h"

extern Config config;
extern Geometry geo;
extern SDL_Window *window;
extern void request_launcher_redraw(void);
extern void refresh_launcher_background(const char *changed_folder);

typedef enum {
    GALLERY_CLOSED,
    GALLERY_GRID,
    GALLERY_PREVIEW,
    GALLERY_CONFIRM
} GalleryView;

typedef enum {
    ITEM_FOLDER,
    ITEM_ADD,
    ITEM_TILE,
    ITEM_PREVIOUS = ITEM_TILE + PHOTO_PAGE_SIZE,
    ITEM_NEXT,
    ITEM_BACK,
    ITEM_USE,
    ITEM_REMOVE,
    ITEM_CANCEL,
    ITEM_DELETE
} GalleryAction;

typedef struct {
    GalleryAction action;
    SDL_Rect rect;
    SDL_Texture *label;
} GalleryItem;

typedef struct {
    const char *string;
    SDL_Texture *texture;
} GalleryLabel;

static GalleryLabel gallery_labels[] = {
    [ITEM_FOLDER] = {"Choose folder"},
    [ITEM_ADD] = {"Add photos"},
    [ITEM_PREVIOUS] = {"Previous page"},
    [ITEM_NEXT] = {"Next page"},
    [ITEM_BACK] = {"Back"},
    [ITEM_USE] = {"Use as background"},
    [ITEM_REMOVE] = {"Remove photo"},
    [ITEM_CANCEL] = {"Cancel"},
    [ITEM_DELETE] = {"Delete photo"}
};

static struct {
    GalleryView view;
    GalleryAction focus;
    ModeBackground *draft_mode;
    char **draft_folder;
    char **draft_image;
    TTF_Font *heading_font;
    TTF_Font *body_font;
    TTF_Font *small_font;
    double scale;
    int origin_x;
    int origin_y;
    int first;
    int detail;
    PhotoList photos;
    GalleryItem items[PHOTO_PAGE_SIZE + 5];
    int item_count;
    SDL_Texture *thumbnails[PHOTO_PAGE_SIZE];
    SDL_Texture *names[PHOTO_PAGE_SIZE];
    SDL_Texture *title_grid;
    SDL_Texture *title_preview;
    SDL_Texture *title_confirm;
    SDL_Texture *section;
    SDL_Texture *folder;
    SDL_Texture *detail_name;
    SDL_Texture *page;
    SDL_Texture *notice;
    SDL_Texture *instructions;
    SDL_Texture *confirmation;
    SDL_Texture *empty;
    SDL_Texture *no_preview;
    SDL_Texture *loading;
    bool need_scan;
    bool need_thumbnails;
    bool mutating;
    bool discard_job;
} gallery;

static const SDL_Color white = {235, 243, 255, 255};
static const SDL_Color muted = {163, 185, 214, 255};
static const SDL_Color amber = {252, 204, 142, 255};
static const SDL_Color border = {193, 218, 255, 255};

static bool has_folder(void)
{
    return gallery.draft_folder != NULL && *gallery.draft_folder != NULL && (*gallery.draft_folder)[0] != '\0';
}

static const char *file_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\')
#else
        if (*p == '/')
#endif
            name = p + 1;
    }
    return name;
}

// Limit the rendered text size and keep full UTF-8 code points.
static void shorten(char *buffer, size_t size, const char *name)
{
    size_t length = strlen(name);
    if (length < size) {
        SDL_strlcpy(buffer, name, size);
        return;
    }
    size_t end = size - 4;
    while (end > 0 && ((unsigned char)name[end] & 0xC0) == 0x80)
        end--;
    memcpy(buffer, name, end);
    memcpy(buffer + end, "...", 4);
}

static int size_for_ui(int value)
{
    return (int)(gallery.scale * value + 0.5);
}

static SDL_Rect rect_for_ui(int x, int y, int w, int h)
{
    return (SDL_Rect){gallery.origin_x + size_for_ui(x), gallery.origin_y + size_for_ui(y),
        size_for_ui(w), size_for_ui(h)};
}

static void replace_text(SDL_Texture **texture, TTF_Font *font, const char *text, SDL_Color color)
{
    SDL_DestroyTexture(*texture);
    *texture = text[0] != '\0' ? load_texture(TTF_RenderUTF8_Blended(font, text, color)) : NULL;
}

static void notice(const char *text, bool error)
{
    char display[160];
    shorten(display, sizeof(display), text);
    replace_text(&gallery.notice, gallery.small_font, display, error ? amber : muted);
}

static void fill(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color color)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

static void outline(SDL_Renderer *renderer, SDL_Rect rect)
{
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    int weight = SDL_max(1, size_for_ui(3));
    for (int i = 0; i < weight && rect.w > 0 && rect.h > 0; i++) {
        SDL_RenderDrawRect(renderer, &rect);
        rect.x++;
        rect.y++;
        rect.w -= 2;
        rect.h -= 2;
    }
}

static void draw_label(SDL_Renderer *renderer, SDL_Texture *texture, SDL_Rect rect, bool centered)
{
    if (texture == NULL)
        return;
    SDL_Rect fit = fit_tv_texture(texture, rect);
    int width, height;
    if (SDL_QueryTexture(texture, NULL, NULL, &width, &height) == 0 && fit.w > width) {
        fit.x = rect.x + (rect.w - width) / 2;
        fit.y = rect.y + (rect.h - height) / 2;
        fit.w = width;
        fit.h = height;
    }
    if (!centered)
        fit.x = rect.x;
    SDL_RenderCopy(renderer, texture, NULL, &fit);
}

static void reset_thumbnails(void)
{
    for (int i = 0; i < PHOTO_PAGE_SIZE; i++) {
        SDL_DestroyTexture(gallery.thumbnails[i]);
        SDL_DestroyTexture(gallery.names[i]);
        gallery.thumbnails[i] = NULL;
        gallery.names[i] = NULL;
    }
}

static void refresh_names(void)
{
    for (int i = 0; i < PHOTO_PAGE_SIZE && gallery.first + i < gallery.photos.count; i++) {
        char name[65];
        shorten(name, sizeof(name), file_name(gallery.photos.paths[gallery.first + i]));
        replace_text(&gallery.names[i], gallery.small_font, name, white);
    }
    char page[48];
    if (gallery.photos.count > 0) {
        int pages = 1 + (gallery.photos.count - 1) / PHOTO_PAGE_SIZE;
        SDL_snprintf(page, sizeof(page), "Page %d of %d", 1 + gallery.first / PHOTO_PAGE_SIZE, pages);
    }
    else
        page[0] = '\0';
    replace_text(&gallery.page, gallery.small_font, page, muted);
}

static void add_item(GalleryAction action, SDL_Rect rect, SDL_Texture *label)
{
    gallery.items[gallery.item_count++] = (GalleryItem){action, rect, label};
}

static void add_action(GalleryAction action, int x, int y, int w, int h)
{
    add_item(action, rect_for_ui(x, y, w, h), gallery_labels[action].texture);
}

static void build_items(void)
{
    gallery.item_count = 0;
    if (gallery.view == GALLERY_GRID) {
        add_action(ITEM_FOLDER, 1077, 133, 275, 72);
        add_action(ITEM_ADD, 1369, 133, 299, 72);
        for (int i = 0; i < PHOTO_PAGE_SIZE && gallery.first + i < gallery.photos.count; i++)
            add_item((GalleryAction)(ITEM_TILE + i),
                rect_for_ui(252 + (i % 3) * 478, 267 + (i / 3) * 254, 460, 236), gallery.names[i]);
        add_action(ITEM_BACK, 252, 800, 200, 70);
        if (gallery.first > 0)
            add_action(ITEM_PREVIOUS, 470, 800, 300, 70);
        if (gallery.photos.count - gallery.first > PHOTO_PAGE_SIZE)
            add_action(ITEM_NEXT, 1380, 800, 288, 70);
    }
    else if (gallery.view == GALLERY_PREVIEW) {
        add_action(ITEM_USE, 1260, 352, 408, 84);
        add_action(ITEM_REMOVE, 1260, 454, 408, 84);
        add_action(ITEM_BACK, 1260, 572, 408, 84);
    }
    else if (gallery.view == GALLERY_CONFIRM) {
        add_action(ITEM_CANCEL, 1260, 413, 408, 84);
        add_action(ITEM_DELETE, 1260, 515, 408, 84);
    }
}

static bool focus_exists(void)
{
    for (int i = 0; i < gallery.item_count; i++) {
        if (gallery.items[i].action == gallery.focus)
            return true;
    }
    return false;
}

static void set_page(int first)
{
    reset_thumbnails();
    gallery.first = first;
    gallery.need_thumbnails = first < gallery.photos.count;
    refresh_names();
    build_items();
    if (!focus_exists())
        gallery.focus = gallery.photos.count > 0 ? ITEM_TILE : ITEM_FOLDER;
}

static void schedule_scan(void)
{
    free_photo_list(&gallery.photos);
    gallery.need_scan = has_folder();
    set_page(0);
}

static void refresh_folder_label(void)
{
    char path[130];
    shorten(path, sizeof(path), has_folder() ? *gallery.draft_folder : "No folder selected");
    replace_text(&gallery.folder, gallery.small_font, path, muted);
}

static void clear_gallery(void)
{
    bool discard_job = gallery.discard_job || photo_job_busy();
    reset_thumbnails();
    free_photo_list(&gallery.photos);
    for (size_t i = 0; i < SDL_arraysize(gallery_labels); i++) {
        SDL_DestroyTexture(gallery_labels[i].texture);
        gallery_labels[i].texture = NULL;
    }
    SDL_DestroyTexture(gallery.title_grid);
    SDL_DestroyTexture(gallery.title_preview);
    SDL_DestroyTexture(gallery.title_confirm);
    SDL_DestroyTexture(gallery.section);
    SDL_DestroyTexture(gallery.folder);
    SDL_DestroyTexture(gallery.detail_name);
    SDL_DestroyTexture(gallery.page);
    SDL_DestroyTexture(gallery.notice);
    SDL_DestroyTexture(gallery.instructions);
    SDL_DestroyTexture(gallery.confirmation);
    SDL_DestroyTexture(gallery.empty);
    SDL_DestroyTexture(gallery.no_preview);
    SDL_DestroyTexture(gallery.loading);
    memset(&gallery, 0, sizeof(gallery));
    gallery.discard_job = discard_job;
}

void open_photo_gallery(ModeBackground *mode, char **folder, char **image,
    TTF_Font *heading, TTF_Font *body, TTF_Font *small)
{
    gallery.view = GALLERY_GRID;
    gallery.draft_mode = mode;
    gallery.draft_folder = folder;
    gallery.draft_image = image;
    gallery.heading_font = heading;
    gallery.body_font = body;
    gallery.small_font = small;
    gallery.scale = SDL_min((double)geo.screen_width / 1920.0, (double)geo.screen_height / 1080.0);
    gallery.origin_x = (geo.screen_width - size_for_ui(1920)) / 2;
    gallery.origin_y = (geo.screen_height - size_for_ui(1080)) / 2;
    for (size_t i = 0; i < SDL_arraysize(gallery_labels); i++) {
        if (gallery_labels[i].string != NULL)
            replace_text(&gallery_labels[i].texture, body, gallery_labels[i].string, white);
    }
    replace_text(&gallery.section, small, "Backgrounds", muted);
    replace_text(&gallery.title_grid, heading, "Manage photos", white);
    replace_text(&gallery.title_preview, heading, "Photo preview", white);
    replace_text(&gallery.title_confirm, heading, "Delete this photo?", amber);
    replace_text(&gallery.confirmation, small, "This removes the photo from the selected folder.", white);
    replace_text(&gallery.instructions, small,
        "Choose a photo to preview it. Save in Backgrounds to apply folder or image choices.", muted);
    replace_text(&gallery.empty, body, "No photos in this folder", muted);
    replace_text(&gallery.no_preview, small, "Preview unavailable", muted);
    replace_text(&gallery.loading, small, "Preparing photos...", muted);
    refresh_folder_label();
    schedule_scan();
    request_launcher_redraw();
}

bool photo_gallery_is_open(void)
{
    return gallery.view != GALLERY_CLOSED;
}

static bool choose_folder(void)
{
    char *chosen = NULL;
    char error[256] = {0};
    PhotoPickResult result = pick_photo_folder(window, &chosen, error, sizeof(error));
    if (result != PHOTO_PICK_OK) {
        if (result == PHOTO_PICK_ERROR)
            notice(error, true);
        return false;
    }

    free(*gallery.draft_folder);
    *gallery.draft_folder = chosen;
    refresh_folder_label();
    schedule_scan();
    notice("Folder selected. Save in Backgrounds to use this folder on Home.", false);
    return true;
}

static void add_photos(void)
{
    if (!has_folder() && !choose_folder())
        return;
    PhotoList files = {0};
    char error[256] = {0};
    PhotoPickResult result = pick_photo_files(window, &files, error, sizeof(error));
    if (result == PHOTO_PICK_ERROR)
        notice(error, true);
    if (result == PHOTO_PICK_OK) {
        if (start_photo_import(*gallery.draft_folder, &files)) {
            gallery.mutating = true;
            notice("Copying photos into the selected folder...", false);
        }
        else
            notice("Could not start importing photos.", true);
    }
    free_photo_list(&files);
}

static void begin_preview(int item)
{
    gallery.detail = item;
    gallery.view = GALLERY_PREVIEW;
    gallery.focus = ITEM_USE;
    char name[116];
    shorten(name, sizeof(name), file_name(gallery.photos.paths[gallery.first + item]));
    replace_text(&gallery.detail_name, gallery.body_font, name, white);
    build_items();
}

static const char *detail_path(void)
{
    return gallery.photos.paths[gallery.first + gallery.detail];
}

static void use_photo(void)
{
    if (gallery.thumbnails[gallery.detail] == NULL) {
        bool loading = photo_job_busy() || gallery.need_thumbnails;
        notice(loading ? "Wait for the preview to finish loading." : "This photo could not be opened.", !loading);
        return;
    }
    char *image = strdup(detail_path());
    if (image == NULL) {
        notice("There is not enough memory to select this photo.", true);
        return;
    }
    free(*gallery.draft_image);
    *gallery.draft_image = image;
    *gallery.draft_mode = BACKGROUND_IMAGE;
    clear_gallery();
}

static void confirm_remove(void)
{
    const char *path = detail_path();
    if (same_photo_path(path, config.background_image) || same_photo_path(path, *gallery.draft_image)) {
        notice("Select and save a different single image before removing this photo.", true);
        return;
    }
    gallery.view = GALLERY_CONFIRM;
    gallery.focus = ITEM_CANCEL;
    notice("", false);
    build_items();
}

static void delete_photo(void)
{
    if (photo_job_busy()) {
        notice("Previews are still loading. Try again in a moment.", false);
        return;
    }
    if (!start_photo_remove(*gallery.draft_folder, detail_path())) {
        notice("Could not start removing the photo.", true);
        return;
    }
    gallery.mutating = true;
    notice("Removing the photo...", false);
}

static void back(void)
{
    if (gallery.view == GALLERY_CONFIRM) {
        gallery.view = GALLERY_PREVIEW;
        gallery.focus = ITEM_REMOVE;
        build_items();
    }
    else if (gallery.view == GALLERY_PREVIEW) {
        gallery.view = GALLERY_GRID;
        gallery.focus = (GalleryAction)(ITEM_TILE + gallery.detail);
        build_items();
    }
    else
        clear_gallery();
}

static void activate(GalleryAction action)
{
    if (action >= ITEM_TILE && action < ITEM_PREVIOUS) {
        begin_preview(action - ITEM_TILE);
        return;
    }
    switch (action) {
        case ITEM_FOLDER:
        case ITEM_ADD:
            if (photo_job_busy())
                notice("Previews are still loading. Try again in a moment.", false);
            else if (action == ITEM_FOLDER)
                choose_folder();
            else
                add_photos();
            break;
        case ITEM_PREVIOUS: set_page(gallery.first - PHOTO_PAGE_SIZE); break;
        case ITEM_NEXT: set_page(gallery.first + PHOTO_PAGE_SIZE); break;
        case ITEM_USE: use_photo(); break;
        case ITEM_REMOVE: confirm_remove(); break;
        case ITEM_DELETE: delete_photo(); break;
        case ITEM_BACK:
        case ITEM_CANCEL: back(); break;
        default: break;
    }
}

static void move_focus(int horizontal, int vertical)
{
    SDL_Rect origin = {0};
    for (int i = 0; i < gallery.item_count; i++) {
        if (gallery.items[i].action == gallery.focus) {
            origin = gallery.items[i].rect;
            break;
        }
    }
    int closest = SDL_MAX_SINT32;
    GalleryAction next = gallery.focus;
    for (int i = 0; i < gallery.item_count; i++) {
        GalleryItem *item = &gallery.items[i];
        int dx = item->rect.x + item->rect.w / 2 - (origin.x + origin.w / 2);
        int dy = item->rect.y + item->rect.h / 2 - (origin.y + origin.h / 2);
        int primary = horizontal != 0 ? dx * horizontal : dy * vertical;
        int secondary = horizontal != 0 ? abs(dy) : abs(dx);
        if (primary <= 0 || (horizontal != 0 && secondary > (origin.h + item->rect.h) / 2))
            continue;
        int score = primary + secondary * 3;
        if (score < closest) {
            closest = score;
            next = item->action;
        }
    }
    gallery.focus = next;
}

void photo_gallery_command(const char *command)
{
    if (!photo_gallery_is_open() || gallery.mutating)
        return;
    if (strcmp(command, SCMD_UP) == 0)
        move_focus(0, -1);
    else if (strcmp(command, SCMD_DOWN) == 0)
        move_focus(0, 1);
    else if (strcmp(command, SCMD_LEFT) == 0)
        move_focus(-1, 0);
    else if (strcmp(command, SCMD_RIGHT) == 0)
        move_focus(1, 0);
    else if (strcmp(command, SCMD_SELECT) == 0)
        activate(gallery.focus);
    else if (strcmp(command, SCMD_BACK) == 0 || strcmp(command, SCMD_HOME) == 0 ||
        strcmp(command, SCMD_QUIT) == 0)
        back();
    else
        return;
    request_launcher_redraw();
}

void photo_gallery_mouse(int x, int y, bool click)
{
    if (!photo_gallery_is_open() || gallery.mutating)
        return;
    SDL_Point point = {x, y};
    for (int i = 0; i < gallery.item_count; i++) {
        if (!SDL_PointInRect(&point, &gallery.items[i].rect))
            continue;
        GalleryAction action = gallery.items[i].action;
        bool changed = gallery.focus != action;
        gallery.focus = action;
        if (click)
            activate(action);
        if (changed || click)
            request_launcher_redraw();
        return;
    }
}

static void draw_thumbnail(SDL_Renderer *renderer, int index, SDL_Rect rect)
{
    SDL_Texture *photo = gallery.thumbnails[index];
    if (photo != NULL) {
        SDL_Rect image = fit_tv_texture(photo, rect);
        SDL_RenderCopy(renderer, photo, NULL, &image);
    }
    else
        draw_label(renderer, photo_job_busy() || gallery.need_thumbnails ? gallery.loading : gallery.no_preview, rect, true);
}

void draw_photo_gallery(SDL_Renderer *renderer)
{
    SDL_Color saved;
    SDL_GetRenderDrawColor(renderer, &saved.r, &saved.g, &saved.b, &saved.a);
    fill(renderer, rect_for_ui(0, 0, 1920, 1080), (SDL_Color){11, 17, 29, 255});
    draw_label(renderer, gallery.section, rect_for_ui(252, 82, 500, 40), false);
    draw_label(renderer, gallery.view == GALLERY_GRID ? gallery.title_grid :
        (gallery.view == GALLERY_CONFIRM ? gallery.title_confirm : gallery.title_preview),
        rect_for_ui(252, 121, 780, 84), false);

    if (gallery.view == GALLERY_GRID) {
        draw_label(renderer, gallery.folder, rect_for_ui(252, 209, 1416, 45), false);
        if (gallery.photos.count == 0)
            draw_label(renderer, photo_job_busy() || gallery.need_scan ? gallery.loading : gallery.empty,
                rect_for_ui(252, 274, 1416, 436), true);
        draw_label(renderer, gallery.page, rect_for_ui(804, 800, 530, 70), true);
        draw_label(renderer, gallery.instructions, rect_for_ui(252, 945, 1416, 45), false);
    }
    else {
        draw_label(renderer, gallery.detail_name, rect_for_ui(252, 211, 1416, 54), false);
        SDL_Rect preview = rect_for_ui(252, 280, 970, 536);
        fill(renderer, preview, (SDL_Color){24, 36, 56, 255});
        draw_thumbnail(renderer, gallery.detail, preview);
        if (gallery.view == GALLERY_CONFIRM)
            draw_label(renderer, gallery.confirmation, rect_for_ui(1260, 317, 408, 61), false);
    }

    for (int i = 0; i < gallery.item_count; i++) {
        GalleryItem *item = &gallery.items[i];
        SDL_Rect rect = item->rect;
        bool focused = gallery.focus == item->action;
        fill(renderer, rect, focused ? (SDL_Color){42, 65, 96, 255} :
            (SDL_Color){24, 36, 56, 255});
        if (item->action >= ITEM_TILE && item->action < ITEM_PREVIOUS) {
            int pad = size_for_ui(9);
            SDL_Rect image = {rect.x + pad, rect.y + pad, rect.w - 2 * pad, rect.h - size_for_ui(53)};
            draw_thumbnail(renderer, item->action - ITEM_TILE, image);
            SDL_Rect name = {rect.x + pad, rect.y + rect.h - size_for_ui(42),
                rect.w - 2 * pad, size_for_ui(40)};
            draw_label(renderer, item->label, name, true);
        }
        else
            draw_label(renderer, item->label, rect, true);
        if (focused)
            outline(renderer, rect);
    }
    draw_label(renderer, gallery.notice, rect_for_ui(252, 882, 1416, 48), false);
    SDL_SetRenderDrawColor(renderer, saved.r, saved.g, saved.b, saved.a);
}

void pump_photo_gallery(void)
{
    PhotoJobResult result;
    if (poll_photo_job(&result)) {
        if (photo_gallery_is_open() && !gallery.discard_job) {
            if (result.kind == PHOTO_JOB_SCAN) {
                if (result.ok) {
                    gallery.photos = result.photos;
                    result.photos = (PhotoList){0};
                    set_page(0);
                }
                else
                    notice(result.message, true);
            }
            else if (result.kind == PHOTO_JOB_THUMBNAILS) {
                if (result.first == gallery.first && !gallery.need_thumbnails) {
                    for (int i = 0; i < result.count; i++) {
                        gallery.thumbnails[i] = load_texture(result.thumbnails[i]);
                        result.thumbnails[i] = NULL;
                    }
                }
            }
            else {
                gallery.mutating = false;
                notice(result.message, !result.ok);
                if (result.ok || (result.kind == PHOTO_JOB_IMPORT && result.count > 0)) {
                    refresh_launcher_background(*gallery.draft_folder);
                    gallery.view = GALLERY_GRID;
                    gallery.focus = ITEM_ADD;
                    schedule_scan();
                }
            }
            request_launcher_redraw();
        }
        gallery.discard_job = false;
        free_photo_list(&result.photos);
        for (int i = 0; i < PHOTO_PAGE_SIZE; i++)
            SDL_FreeSurface(result.thumbnails[i]);
    }

    if (!photo_gallery_is_open() || photo_job_busy())
        return;
    if (gallery.need_scan) {
        gallery.need_scan = false;
        if (!start_photo_scan(*gallery.draft_folder))
            notice("Could not start reading the photo folder.", true);
        request_launcher_redraw();
    }
    else if (gallery.need_thumbnails) {
        gallery.need_thumbnails = false;
        if (!start_photo_thumbnails(&gallery.photos, gallery.first, PHOTO_PAGE_SIZE))
            notice("Could not start preparing previews.", true);
        request_launcher_redraw();
    }
}

void quit_photo_gallery(void)
{
    photo_job_quit();
    clear_gallery();
}
