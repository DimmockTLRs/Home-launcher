#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include "app_editor.h"
#include "image.h"
#include "photo_files.h"
#include "platform/platform.h"
#include "tv_ui.h"
#include "util.h"
#include "website.h"

#define EDITOR_PAGE_SIZE 6
#define PREVIEW_EDGE 480
#define TEXT_LIMIT 4096
#define WEBSITE_PREFIX SCMD_WEBSITE " "
#define DASHBOARD_URL "Dashboard URL"

extern SDL_Window *window;
extern Geometry geo;
extern void request_launcher_redraw(void);
extern bool apply_menu_settings(const Entry *entries, char *error, size_t size);

typedef enum {
    VIEW_CLOSED, VIEW_LIST, VIEW_EDIT, VIEW_LIBRARY, VIEW_TEXT, VIEW_CONFIRM
} EditorView;

typedef enum {
    ITEM_ADD, ITEM_ADD_ASSISTANT, ITEM_EDIT, ITEM_UP, ITEM_DOWN, ITEM_REMOVE, ITEM_SAVE, ITEM_BACK,
    ITEM_NAME, ITEM_COMMAND, ITEM_PROGRAM, ITEM_WEBSITE, ITEM_IMPORT, ITEM_LIBRARY,
    ITEM_DONE, ITEM_CANCEL, ITEM_CONFIRM, ITEM_PREVIOUS, ITEM_NEXT,
    ITEM_ROW, ITEM_TOTAL = ITEM_ROW + EDITOR_PAGE_SIZE
} EditorAction;

typedef enum {
    JOB_NONE, JOB_PREVIEW, JOB_IMPORT, JOB_SCAN
} EditorJobType;

typedef struct {
    EditorAction action;
    SDL_Rect rect;
    SDL_Texture *text;
    bool left_align;
} EditorItem;

static const char *const item_names[] = {
    "Add application", "Add Home Assistant", "Edit", "Move up", "Move down", "Remove", "Save and return", "Back",
    "Name", "Launch command", "Choose program", "Website URL", "Import icon", "Icon library",
    "Done", "Cancel", "Remove card", "Previous page", "Next page"
};

static const SDL_Color white = {235, 243, 255, 255};
static const SDL_Color muted = {163, 185, 214, 255};
static const SDL_Color amber = {252, 204, 142, 255};
static const SDL_Color border = {193, 218, 255, 255};

static struct {
    SDL_Thread *thread;
    SDL_atomic_t ready;
    SDL_atomic_t stop_requested;
    EditorJobType type;
    unsigned session;
    unsigned preview_version;
    char *path;
    SDL_Surface *surface;
    PhotoList icons;
    char *imported;
    char error[256];
    bool success;
} job;
static unsigned session_number;

static struct {
    EditorView view;
    Entry *entries;
    Entry *selected;
    Entry *editing;
    char *name;
    char *command;
    char *icon;
    bool dirty;
    bool saving;
    int list_first;
    PhotoList icons;
    int library_first;
    int library_index;
    bool scan_pending;
    char *import_pending;
    bool importing;
    bool preview_pending;
    unsigned preview_version;
    SDL_Texture *preview;
    TTF_Font *heading_font;
    TTF_Font *body_font;
    TTF_Font *small_font;
    double scale;
    int origin_x, origin_y;
    EditorItem items[ITEM_TOTAL];
    int item_count;
    EditorAction focus;
    SDL_Texture *labels[ITEM_ROW];
    SDL_Texture *titles[VIEW_CONFIRM + 1];
    SDL_Texture *dashboard;
    SDL_Texture *rows[EDITOR_PAGE_SIZE];
    SDL_Texture *section;
    SDL_Texture *instructions;
    SDL_Texture *notice;
    SDL_Texture *preview_name;
    SDL_Texture *preview_command;
    SDL_Texture *icon_binding;
    SDL_Texture *no_preview;
    SDL_Texture *loading;
    SDL_Texture *empty_list;
    SDL_Texture *empty_library;
    SDL_Texture *confirm_message;
    SDL_Texture *text;
    SDL_Texture *text_title;
    SDL_Texture *page_label;
    EditorAction text_field;
    char *input;
    size_t capacity;
    size_t caret;
    int caret_px;
    bool select_all;
} editor;

static int scaled(int value)
{
    return (int)(editor.scale * value + 0.5);
}

static SDL_Rect rect_for(int x, int y, int width, int height)
{
    return (SDL_Rect){editor.origin_x + scaled(x), editor.origin_y + scaled(y),
        scaled(width), scaled(height)};
}

static const char *file_name(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\')
#else
        if (*p == '/')
#endif
            last = p + 1;
    }
    return last;
}

static void shorten(char *out, size_t size, const char *text)
{
    if (strlen(text) < size) {
        SDL_strlcpy(out, text, size);
        return;
    }
    size_t end = size - 4;
    while (end > 0 && ((unsigned char)text[end] & 0xC0) == 0x80)
        end--;
    memcpy(out, text, end);
    memcpy(out + end, "...", 4);
}

static void label(SDL_Texture **texture, TTF_Font *font, const char *text, SDL_Color colour)
{
    SDL_DestroyTexture(*texture);
    *texture = *text ? load_texture(TTF_RenderUTF8_Blended(font, text, colour)) : NULL;
}

static void set_notice(const char *message, bool warning)
{
    char safe[156];
    shorten(safe, sizeof(safe), message);
    label(&editor.notice, editor.small_font, safe, warning ? amber : muted);
    request_launcher_redraw();
}

static void paint(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color colour)
{
    SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_RenderFillRect(renderer, &rect);
}

static void draw_text(SDL_Renderer *renderer, SDL_Texture *text, SDL_Rect space, bool centered)
{
    int width, height;
    if (text == NULL || SDL_QueryTexture(text, NULL, NULL, &width, &height) != 0 ||
        width <= 0 || height <= 0 || space.w <= 0 || space.h <= 0)
        return;
    SDL_Rect hit = space;
    double ratio = SDL_min(1.0, SDL_min((double)space.w / width, (double)space.h / height));
    hit.w = SDL_max(1, (int)(width * ratio));
    hit.h = SDL_max(1, (int)(height * ratio));
    hit.y += (space.h - hit.h) / 2;
    if (centered)
        hit.x += (space.w - hit.w) / 2;
    SDL_RenderCopy(renderer, text, NULL, &hit);
}

static void draw_box(SDL_Renderer *renderer, SDL_Rect box, bool selected)
{
    paint(renderer, box, selected ? (SDL_Color){42, 65, 96, 255} : (SDL_Color){24, 36, 56, 255});
    if (!selected)
        return;
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    int stroke = SDL_max(1, scaled(3));
    for (int i = 0; i < stroke && box.w > 0 && box.h > 0; i++) {
        SDL_RenderDrawRect(renderer, &box);
        box.x++;
        box.y++;
        box.w -= 2;
        box.h -= 2;
    }
}

static Entry *entry_at(int index)
{
    Entry *entry = editor.entries;
    while (entry != NULL && index-- > 0)
        entry = entry->next;
    return entry;
}

static int entry_index(const Entry *entry)
{
    int index = 0;
    for (Entry *current = editor.entries; current != NULL; current = current->next, index++) {
        if (entry == current)
            return index;
    }
    return -1;
}

static int entry_count(void)
{
    int count = 0;
    for (Entry *entry = editor.entries; entry != NULL; entry = entry->next)
        count++;
    return count;
}

static int page_start(void)
{
    return editor.view == VIEW_LIBRARY ? editor.library_first : editor.list_first;
}

static int content_count(void)
{
    return editor.view == VIEW_LIBRARY ? editor.icons.count : entry_count();
}

static void add_item(EditorAction action, int x, int y, int w, int h)
{
    bool row = action >= ITEM_ROW;
    SDL_Texture *text = row ? editor.rows[action - ITEM_ROW] : editor.labels[action];
    if (action == ITEM_WEBSITE && home_assistant_address(editor.command) != NULL)
        text = editor.dashboard;
    editor.items[editor.item_count++] = (EditorItem){action, rect_for(x, y, w, h),
        text, row};
}

static bool current_focus_visible(void)
{
    for (int i = 0; i < editor.item_count; i++) {
        if (editor.items[i].action == editor.focus)
            return true;
    }
    return false;
}

static void update_items(void)
{
    editor.item_count = 0;
    if (editor.view == VIEW_LIST || editor.view == VIEW_LIBRARY) {
        int first = page_start();
        int count = content_count();
        for (int i = 0; i < EDITOR_PAGE_SIZE; i++) {
            char name[62] = {0};
            int index = first + i;
            if (index < count) {
                const char *value = editor.view == VIEW_LIBRARY ?
                    file_name(editor.icons.paths[index]) : entry_at(index)->title;
                shorten(name, sizeof(name), value);
            }
            label(&editor.rows[i], editor.body_font, name, white);
            if (index < count)
                add_item((EditorAction)(ITEM_ROW + i), 252, 274 + i * 84, 765, 69);
        }
        char meta[52] = {0};
        if (count > 0) {
            SDL_snprintf(meta, sizeof(meta), "Page %d of %d", first / EDITOR_PAGE_SIZE + 1,
                (count - 1) / EDITOR_PAGE_SIZE + 1);
        }
        label(&editor.page_label, editor.small_font, meta, muted);
        if (editor.view == VIEW_LIST) {
            add_item(ITEM_ADD, 252, 190, 350, 68);
            add_item(ITEM_ADD_ASSISTANT, 620, 190, 397, 68);
            if (editor.selected != NULL) {
                add_item(ITEM_EDIT, 1074, 596, 265, 70);
                add_item(ITEM_REMOVE, 1350, 596, 318, 70);
                if (editor.selected->previous != NULL)
                    add_item(ITEM_UP, 1074, 684, 265, 70);
                if (editor.selected->next != NULL)
                    add_item(ITEM_DOWN, 1350, 684, 318, 70);
            }
            add_item(ITEM_SAVE, 1319, 834, 349, 72);
        }
        if (first > 0)
            add_item(ITEM_PREVIOUS, 465, 834, 260, 72);
        if (count - first > EDITOR_PAGE_SIZE)
            add_item(ITEM_NEXT, 744, 834, 273, 72);
        add_item(ITEM_BACK, 252, 834, 194, 72);
    }
    else if (editor.view == VIEW_EDIT) {
        bool dashboard = home_assistant_address(editor.command) != NULL;
        int row = 0;
        for (int action = ITEM_NAME; action <= ITEM_LIBRARY; action++) {
            if (dashboard && (action == ITEM_COMMAND || action == ITEM_PROGRAM))
                continue;
            add_item((EditorAction)action, 252, 249 + row * 87, 765, 73);
            row++;
        }
        add_item(ITEM_BACK, 252, 834, 194, 72);
        add_item(ITEM_DONE, 1319, 834, 349, 72);
    }
    else if (editor.view == VIEW_TEXT) {
        add_item(ITEM_CANCEL, 252, 632, 250, 77);
        add_item(ITEM_DONE, 1319, 632, 349, 77);
    }
    else if (editor.view == VIEW_CONFIRM) {
        add_item(ITEM_CANCEL, 252, 587, 365, 80);
        add_item(ITEM_CONFIRM, 1220, 587, 448, 80);
    }
    if (!current_focus_visible() && editor.item_count > 0)
        editor.focus = editor.items[0].action;
}

static const char *preview_path(void)
{
    if (editor.view == VIEW_LIBRARY)
        return editor.library_index >= 0 ? editor.icons.paths[editor.library_index] : NULL;
    if (editor.view == VIEW_EDIT)
        return editor.icon;
    if (editor.view == VIEW_LIST)
        return editor.selected != NULL ? editor.selected->icon_path : NULL;
    return NULL;
}

static const char *command_caption(const char *command)
{
    const char *address = home_assistant_address(command);
    if (address == NULL)
        return command;
    return valid_home_assistant_url(address) ? address : "Dashboard address required";
}

static void reset_preview(void)
{
    SDL_DestroyTexture(editor.preview);
    editor.preview = NULL;
    editor.preview_version++;
    const char *path = preview_path();
    bool builtin = path != NULL && strcmp(path, APP_ICON_DEFAULT) == 0;
    bool assistant = path != NULL && strcmp(path, APP_ICON_HOME_ASSISTANT) == 0;
    editor.preview_pending = path != NULL && *path && !builtin && !assistant;
    if (builtin)
        editor.preview = default_app_icon();
    else if (assistant)
        editor.preview = home_assistant_icon();

    char name[52] = {0}, command[70] = {0};
    if (editor.view == VIEW_LIBRARY && path != NULL)
        shorten(name, sizeof(name), file_name(path));
    else if (editor.view == VIEW_EDIT) {
        shorten(name, sizeof(name), *editor.name ? editor.name : "New application");
        if (*editor.command)
            shorten(command, sizeof(command), command_caption(editor.command));
    }
    else if (editor.view == VIEW_LIST && editor.selected != NULL) {
        shorten(name, sizeof(name), editor.selected->title);
        shorten(command, sizeof(command), command_caption(editor.selected->cmd));
    }
    label(&editor.preview_name, editor.body_font, name, white);
    label(&editor.preview_command, editor.small_font, command, muted);
    if (editor.view == VIEW_EDIT) {
        char name_only[70];
        const char *icon_name = assistant ? "Home Assistant icon" :
            (path == NULL || builtin ? "Default icon" : file_name(path));
        shorten(name_only, sizeof(name_only), icon_name);
        label(&editor.icon_binding, editor.small_font, name_only, muted);
    }
    request_launcher_redraw();
}

static SDL_Surface *prepare_preview(const char *path)
{
    SDL_Surface *source = IMG_Load(path);
    if (source == NULL)
        return NULL;
    if (source->w <= 0 || source->h <= 0) {
        SDL_FreeSurface(source);
        return NULL;
    }
    if (source->format->palette != NULL || SDL_HasColorKey(source)) {
        SDL_Surface *rgba = SDL_ConvertSurfaceFormat(source, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(source);
        if (rgba == NULL)
            return NULL;
        source = rgba;
    }
    double scale = SDL_min(1.0, SDL_min((double)PREVIEW_EDGE / source->w, (double)PREVIEW_EDGE / source->h));
    SDL_Surface *image = SDL_CreateRGBSurfaceWithFormat(0, SDL_max(1, (int)(source->w * scale)),
        SDL_max(1, (int)(source->h * scale)), 32, SDL_PIXELFORMAT_RGBA32);
    if (image != NULL) {
        SDL_FillRect(image, NULL, 0);
        SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
        if (SDL_BlitScaled(source, NULL, image, NULL) != 0) {
            SDL_FreeSurface(image);
            image = NULL;
        }
    }
    SDL_FreeSurface(source);
    return image;
}

static int compare_paths(const void *left, const void *right)
{
    const char *a = *(const char *const *)left;
    const char *b = *(const char *const *)right;
    return SDL_strcasecmp(a, b);
}

static int run_job(void *unused)
{
    (void)unused;
    if (job.type == JOB_PREVIEW)
        job.surface = prepare_preview(job.path);
    else if (job.type == JOB_IMPORT)
        job.success = import_app_icon(job.path, &job.imported, job.error, sizeof(job.error));
    else {
        char *folder = app_icon_folder(job.error, sizeof(job.error));
        if (folder != NULL) {
            job.success = scan_slideshow_directory(&job.icons, folder, &job.stop_requested);
            if (!job.success)
                SDL_strlcpy(job.error, "Could not read the icon library.", sizeof(job.error));
            if (job.success && job.icons.count > 1)
                qsort(job.icons.paths, (size_t)job.icons.count, sizeof(*job.icons.paths), compare_paths);
        }
        free(folder);
    }
    SDL_AtomicSet(&job.ready, 1);
    return 0;
}

static bool start_job(EditorJobType type, const char *path)
{
    job.type = type;
    job.session = session_number;
    job.preview_version = editor.preview_version;
    job.path = path != NULL ? strdup(path) : NULL;
    if (path != NULL && job.path == NULL)
        return false;
    job.success = false;
    job.error[0] = '\0';
    SDL_AtomicSet(&job.ready, 0);
    SDL_AtomicSet(&job.stop_requested, 0);
    job.thread = SDL_CreateThread(run_job, "Application icon job", NULL);
    if (job.thread == NULL) {
        free(job.path);
        job.path = NULL;
        return false;
    }
    return true;
}

static void clear_job(void)
{
    SDL_FreeSurface(job.surface);
    free_photo_list(&job.icons);
    free(job.path);
    free(job.imported);
    memset(&job, 0, sizeof(job));
}

void app_editor_pump(void)
{
    if (job.thread != NULL && SDL_AtomicGet(&job.ready)) {
        SDL_WaitThread(job.thread, NULL);
        if (job.session == session_number && app_editor_is_open()) {
            if (job.type == JOB_PREVIEW && job.preview_version == editor.preview_version) {
                editor.preview = load_texture(job.surface);
                job.surface = NULL;
            }
            else if (job.type == JOB_IMPORT) {
                editor.importing = false;
                if (job.success) {
                    free(editor.icon);
                    editor.icon = job.imported;
                    job.imported = NULL;
                    reset_preview();
                    set_notice("Icon imported. Choose Done to keep it on this application.", false);
                }
                else
                    set_notice(job.error, true);
            }
            else if (job.type == JOB_SCAN && editor.view == VIEW_LIBRARY) {
                if (job.success) {
                    free_photo_list(&editor.icons);
                    editor.icons = job.icons;
                    job.icons = (PhotoList){0};
                    editor.library_index = editor.icons.count > 0 ? 0 : -1;
                    editor.focus = editor.icons.count > 0 ? ITEM_ROW : ITEM_BACK;
                    update_items();
                    reset_preview();
                }
                else
                    set_notice(job.error, true);
            }
            request_launcher_redraw();
        }
        clear_job();
    }
    if (!app_editor_is_open() || job.thread != NULL)
        return;
    if (editor.import_pending != NULL) {
        if (!start_job(JOB_IMPORT, editor.import_pending)) {
            editor.importing = false;
            set_notice("Could not start importing the icon.", true);
        }
        free(editor.import_pending);
        editor.import_pending = NULL;
    }
    else if (editor.scan_pending) {
        editor.scan_pending = false;
        if (!start_job(JOB_SCAN, NULL))
            set_notice("Could not open the icon library.", true);
    }
    else if (editor.preview_pending) {
        editor.preview_pending = false;
        if (!start_job(JOB_PREVIEW, preview_path()))
            request_launcher_redraw();
    }
}

static void end_input(void)
{
    if (editor.view == VIEW_TEXT)
        SDL_StopTextInput();
    free(editor.input);
    editor.input = NULL;
    editor.capacity = 0;
}

static void discard_form(void)
{
    free(editor.name);
    free(editor.command);
    free(editor.icon);
    editor.name = editor.command = editor.icon = NULL;
}

static void close_editor(void)
{
    if (!app_editor_is_open())
        return;
    session_number++;
    SDL_AtomicSet(&job.stop_requested, 1);
    end_input();
    discard_form();
    free_menu_entries(editor.entries);
    free_photo_list(&editor.icons);
    free(editor.import_pending);
    for (int i = 0; i < ITEM_ROW; i++)
        SDL_DestroyTexture(editor.labels[i]);
    for (int i = 0; i <= VIEW_CONFIRM; i++)
        SDL_DestroyTexture(editor.titles[i]);
    SDL_DestroyTexture(editor.dashboard);
    for (int i = 0; i < EDITOR_PAGE_SIZE; i++)
        SDL_DestroyTexture(editor.rows[i]);
    SDL_DestroyTexture(editor.section);
    SDL_DestroyTexture(editor.notice);
    SDL_DestroyTexture(editor.instructions);
    SDL_DestroyTexture(editor.preview);
    SDL_DestroyTexture(editor.preview_name);
    SDL_DestroyTexture(editor.preview_command);
    SDL_DestroyTexture(editor.icon_binding);
    SDL_DestroyTexture(editor.no_preview);
    SDL_DestroyTexture(editor.loading);
    SDL_DestroyTexture(editor.empty_list);
    SDL_DestroyTexture(editor.empty_library);
    SDL_DestroyTexture(editor.confirm_message);
    SDL_DestroyTexture(editor.text);
    SDL_DestroyTexture(editor.text_title);
    SDL_DestroyTexture(editor.page_label);
    memset(&editor, 0, sizeof(editor));
    request_launcher_redraw();
}

bool app_editor_open(const Entry *entries, const Entry *stop,
    TTF_Font *heading, TTF_Font *body, TTF_Font *small)
{
    if (app_editor_is_open())
        return true;
    if (!clone_menu_entries(entries, stop, &editor.entries))
        return false;

    session_number++;
    editor.view = VIEW_LIST;
    editor.focus = ITEM_ADD;
    editor.selected = editor.entries;
    editor.heading_font = heading;
    editor.body_font = body;
    editor.small_font = small;
    editor.scale = SDL_min((double)geo.screen_width / 1920.0, (double)geo.screen_height / 1080.0);
    editor.origin_x = (geo.screen_width - scaled(1920)) / 2;
    editor.origin_y = (geo.screen_height - scaled(1080)) / 2;
    for (int i = 0; i < ITEM_ROW; i++)
        label(&editor.labels[i], body, item_names[i], white);
    label(&editor.dashboard, body, DASHBOARD_URL, white);
    label(&editor.section, small, "Settings", muted);
    label(&editor.titles[VIEW_LIST], heading, "Applications and Icons", white);
    label(&editor.titles[VIEW_EDIT], heading, "Application details", white);
    label(&editor.titles[VIEW_LIBRARY], heading, "Icon library", white);
    label(&editor.titles[VIEW_CONFIRM], heading, "Remove application?", amber);
    label(&editor.instructions, small, "Arrows  Navigate     Enter / A  Select     Esc / B  Back", muted);
    label(&editor.no_preview, small, "No icon preview", muted);
    label(&editor.loading, small, "Preparing preview...", muted);
    label(&editor.empty_list, body, "No applications on Home", muted);
    label(&editor.empty_library, body, "Import an icon to start your library", muted);
    label(&editor.confirm_message, small, "The card is removed when you save. Your application and icon files stay on disk.", muted);
    update_items();
    reset_preview();
    return true;
}

bool app_editor_is_open(void)
{
    return editor.view != VIEW_CLOSED;
}

bool app_editor_is_typing(void)
{
    return editor.view == VIEW_TEXT;
}

static bool start_edit(Entry *entry, bool assistant_template)
{
    const char *name = assistant_template ? "Home Assistant" : "";
    const char *command = assistant_template ? SCMD_HOME_ASSISTANT : "";
    const char *icon = assistant_template ? APP_ICON_HOME_ASSISTANT : APP_ICON_DEFAULT;
    editor.name = strdup(entry != NULL ? entry->title : name);
    editor.command = strdup(entry != NULL ? entry->cmd : command);
    editor.icon = strdup(entry != NULL ? entry->icon_path : icon);
    if (editor.name == NULL || editor.command == NULL || editor.icon == NULL) {
        discard_form();
        set_notice("Could not open the application editor.", true);
        return false;
    }
    editor.editing = entry;
    editor.view = VIEW_EDIT;
    editor.focus = ITEM_NAME;
    set_notice("", false);
    update_items();
    reset_preview();
    return true;
}

static void return_to_list(void)
{
    discard_form();
    editor.view = VIEW_LIST;
    int index = entry_index(editor.selected);
    editor.list_first = index >= 0 ? index / EDITOR_PAGE_SIZE * EDITOR_PAGE_SIZE : 0;
    editor.focus = index >= 0 ? (EditorAction)(ITEM_ROW + index % EDITOR_PAGE_SIZE) : ITEM_ADD;
    update_items();
    reset_preview();
}

static bool contains_line_break(const char *text)
{
    return strchr(text, '\r') != NULL || strchr(text, '\n') != NULL;
}

static bool nonempty(const char *text)
{
    while (*text && SDL_isspace((unsigned char)*text))
        text++;
    return *text != '\0';
}

static void finish_edit(void)
{
    if (!nonempty(editor.name)) {
        set_notice("Enter a name for the application.", true);
        editor.focus = ITEM_NAME;
        return;
    }
    if (!nonempty(editor.command)) {
        set_notice("Choose a program, enter a launch command, or add a website URL.", true);
        editor.focus = ITEM_PROGRAM;
        return;
    }
    if (contains_line_break(editor.name) || strchr(editor.name, ';') != NULL ||
        contains_line_break(editor.icon) || strchr(editor.icon, ';') != NULL ||
        contains_line_break(editor.command) || strcmp(editor.command, SCMD_SELECT) == 0) {
        set_notice("This entry contains characters or a command the launcher cannot save.", true);
        return;
    }
    const char *dashboard = home_assistant_address(editor.command);
    if (dashboard != NULL && *dashboard != '\0' && !valid_home_assistant_url(dashboard)) {
        set_notice("Enter a dashboard address without sign-in details, a query or a fragment.", true);
        return;
    }
    size_t website_prefix = sizeof(WEBSITE_PREFIX) - 1;
    if (strncmp(editor.command, WEBSITE_PREFIX, website_prefix) == 0 &&
        !valid_website_url(editor.command + website_prefix)) {
        set_notice("Enter a valid HTTP or HTTPS website URL.", true);
        return;
    }

    Entry *entry = editor.editing;
    if (entry != NULL && strcmp(entry->title, editor.name) == 0 &&
        strcmp(entry->cmd, editor.command) == 0 && strcmp(entry->icon_path, editor.icon) == 0) {
        return_to_list();
        return;
    }
    if (entry == NULL) {
        entry = calloc(1, sizeof(*entry));
        if (entry == NULL) {
            set_notice("Could not add the application.", true);
            return;
        }
        Entry *tail = editor.entries;
        if (tail == NULL)
            editor.entries = entry;
        else {
            while (tail->next != NULL)
                tail = tail->next;
            tail->next = entry;
            entry->previous = tail;
        }
    }
    if (entry->icon_path == NULL || strcmp(entry->icon_path, editor.icon) != 0) {
        free(entry->icon_selected_path);
        entry->icon_selected_path = NULL;
    }
    free(entry->title);
    free(entry->cmd);
    free(entry->icon_path);
    entry->title = editor.name;
    entry->cmd = editor.command;
    entry->icon_path = editor.icon;
    editor.name = editor.command = editor.icon = NULL;
    editor.selected = entry;
    editor.dirty = true;
    return_to_list();
    set_notice("Save to apply your changes on Home.", false);
}

static void swap_adjacent(Entry *left)
{
    Entry *right = left->next;
    Entry *before = left->previous;
    Entry *after = right->next;
    if (before != NULL)
        before->next = right;
    else
        editor.entries = right;
    right->previous = before;
    right->next = left;
    left->previous = right;
    left->next = after;
    if (after != NULL)
        after->previous = left;
}

static void move_entry(bool up)
{
    Entry *entry = editor.selected;
    if (entry == NULL || (up && entry->previous == NULL) || (!up && entry->next == NULL))
        return;
    swap_adjacent(up ? entry->previous : entry);
    editor.list_first = entry_index(entry) / EDITOR_PAGE_SIZE * EDITOR_PAGE_SIZE;
    editor.dirty = true;
    update_items();
    set_notice("Card moved. Save to apply its new position on Home.", false);
}

static void remove_entry(void)
{
    Entry *entry = editor.selected;
    editor.selected = entry->next != NULL ? entry->next : entry->previous;
    if (entry->previous != NULL)
        entry->previous->next = entry->next;
    else
        editor.entries = entry->next;
    if (entry->next != NULL)
        entry->next->previous = entry->previous;
    entry->next = entry->previous = NULL;
    free_menu_entries(entry);
    editor.dirty = true;
    return_to_list();
    set_notice("Card removed. Save to apply the change on Home.", false);
}

static void choose_program(void)
{
    char *path = NULL;
    char error[256] = {0};
    PhotoPickResult result = pick_app_executable(window, &path, error, sizeof(error));
    if (result == PHOTO_PICK_ERROR)
        set_notice(error, true);
    if (result != PHOTO_PICK_OK)
        return;

    size_t length = strlen(path);
    char *command = malloc(length + 3);
    if (command == NULL) {
        free(path);
        set_notice("Could not store the program path.", true);
        return;
    }
    SDL_snprintf(command, length + 3, "\"%s\"", path);
    free(editor.command);
    editor.command = command;
    if (!nonempty(editor.name)) {
        char *name = strdup(file_name(path));
        if (name != NULL) {
            char *ext = strrchr(name, '.');
            if (ext != NULL && ext != name)
                *ext = '\0';
            // Keep a default derived from the name compatible with the INI field delimiter.
            for (char *c = name; *c; c++) {
                if (*c == ';')
                    *c = '-';
            }
            free(editor.name);
            editor.name = name;
        }
    }
    free(path);
    set_notice("Program selected. Choose Done to keep it on this application.", false);
    reset_preview();
}

static void pick_icon(void)
{
    char *source = NULL;
    char error[256] = {0};
    PhotoPickResult result = pick_app_icon(window, &source, error, sizeof(error));
    if (result == PHOTO_PICK_ERROR)
        set_notice(error, true);
    if (result == PHOTO_PICK_OK) {
        editor.import_pending = source;
        editor.importing = true;
        set_notice("Importing the icon...", false);
    }
}

static void open_library(void)
{
    editor.view = VIEW_LIBRARY;
    editor.focus = ITEM_BACK;
    editor.library_index = -1;
    editor.library_first = 0;
    editor.scan_pending = true;
    set_notice("", false);
    update_items();
    reset_preview();
}

static void return_from_library(void)
{
    free_photo_list(&editor.icons);
    editor.scan_pending = false;
    if (job.thread != NULL && job.type == JOB_SCAN && job.session == session_number) {
        SDL_AtomicSet(&job.stop_requested, 1);
        job.session = 0;
    }
    editor.view = VIEW_EDIT;
    editor.focus = ITEM_LIBRARY;
    update_items();
    reset_preview();
}

static void choose_library_icon(int index)
{
    char *path = strdup(editor.icons.paths[index]);
    if (path == NULL) {
        set_notice("Could not select the icon.", true);
        return;
    }
    free(editor.icon);
    editor.icon = path;
    return_from_library();
    set_notice("Icon selected. Choose Done to keep it on this application.", false);
}

static size_t previous_codepoint(const char *text, size_t index)
{
    if (index > 0) {
        index--;
        while (index > 0 && ((unsigned char)text[index] & 0xC0) == 0x80)
            index--;
    }
    return index;
}

static size_t next_codepoint(const char *text, size_t index)
{
    if (text[index] != '\0') {
        index++;
        while (((unsigned char)text[index] & 0xC0) == 0x80)
            index++;
    }
    return index;
}

static void update_input(void)
{
    size_t start = editor.caret;
    for (int i = 0; i < 56 && start > 0; i++)
        start = previous_codepoint(editor.input, start);
    size_t end = editor.caret;
    for (int i = 0; i < 56 && editor.input[end] != '\0'; i++)
        end = next_codepoint(editor.input, end);
    char saved = editor.input[editor.caret];
    editor.input[editor.caret] = '\0';
    editor.caret_px = 0;
    TTF_SizeUTF8(editor.body_font, editor.input + start, &editor.caret_px, NULL);
    editor.input[editor.caret] = saved;
    saved = editor.input[end];
    editor.input[end] = '\0';
    label(&editor.text, editor.body_font, editor.input + start, white);
    editor.input[end] = saved;
    request_launcher_redraw();
}

static void start_input(EditorAction field)
{
    size_t website_prefix = sizeof(WEBSITE_PREFIX) - 1;
    const char *dashboard = field == ITEM_WEBSITE ? home_assistant_address(editor.command) : NULL;
    const char *value = field == ITEM_NAME ? editor.name : editor.command;
    if (dashboard != NULL)
        value = valid_home_assistant_url(dashboard) ? dashboard : "";
    else if (field == ITEM_WEBSITE)
        value = strncmp(value, WEBSITE_PREFIX, website_prefix) == 0 ? value + website_prefix : "";
    size_t length = strlen(value);
    editor.capacity = SDL_max((size_t)TEXT_LIMIT + 1, length + 1);
    editor.input = malloc(editor.capacity);
    if (editor.input == NULL) {
        set_notice("Could not start editing the field.", true);
        return;
    }
    memcpy(editor.input, value, length + 1);
    editor.caret = length;
    editor.select_all = length != 0;
    editor.text_field = field;
    editor.view = VIEW_TEXT;
    editor.focus = ITEM_DONE;
    set_notice(dashboard != NULL ? "Enter your Home Assistant dashboard address or paste with Ctrl+V. Press Enter to apply." :
        "Type or paste with Ctrl+V. Enter applies the value. Esc cancels.", false);
    label(&editor.text_title, editor.heading_font, dashboard != NULL ? DASHBOARD_URL : item_names[field], white);
    update_items();
    update_input();
    SDL_Rect position = rect_for(252, 397, 1416, 105);
    SDL_SetTextInputRect(&position);
    SDL_StartTextInput();
}

void app_editor_configure_home_assistant(int index)
{
    Entry *entry = entry_at(index);
    if (editor.view == VIEW_LIST && entry != NULL && home_assistant_address(entry->cmd) != NULL &&
        start_edit(entry, false))
        start_input(ITEM_WEBSITE);
}

static void close_input(void)
{
    end_input();
    editor.view = VIEW_EDIT;
    editor.focus = editor.text_field;
    update_items();
    set_notice("", false);
    reset_preview();
}

static void commit_input(void)
{
    bool website = editor.text_field == ITEM_WEBSITE;
    bool dashboard = website && home_assistant_address(editor.command) != NULL;
    if (dashboard && *editor.input != '\0' && !valid_home_assistant_url(editor.input)) {
        set_notice("Use an HTTP or HTTPS dashboard address without sign-in details, a query or a fragment.", true);
        return;
    }
    if (website && !dashboard && !valid_website_url(editor.input)) {
        set_notice("Enter an address starting with http:// or https://.", true);
        return;
    }
    if (editor.text_field == ITEM_NAME && strchr(editor.input, ';') != NULL) {
        set_notice("Names cannot contain a semicolon.", true);
        return;
    }
    const char *prefix = dashboard ? (*editor.input ? SCMD_HOME_ASSISTANT " " : SCMD_HOME_ASSISTANT) :
        (website ? WEBSITE_PREFIX : "");
    size_t size = strlen(prefix) + strlen(editor.input) + 1;
    char *value = malloc(size);
    if (value == NULL) {
        set_notice("Could not store the new value.", true);
        return;
    }
    SDL_snprintf(value, size, "%s%s", prefix, editor.input);
    char **target = editor.text_field == ITEM_NAME ? &editor.name : &editor.command;
    free(*target);
    *target = value;
    close_input();
}

static void erase_input(bool backward)
{
    if (editor.select_all) {
        editor.input[0] = '\0';
        editor.caret = 0;
        editor.select_all = false;
    }
    else {
        size_t first = backward ? previous_codepoint(editor.input, editor.caret) : editor.caret;
        size_t last = backward ? editor.caret : next_codepoint(editor.input, editor.caret);
        memmove(editor.input + first, editor.input + last, strlen(editor.input + last) + 1);
        editor.caret = first;
    }
    update_input();
}

void app_editor_text(const char *text)
{
    if (!app_editor_is_typing() || text == NULL || *text == '\0')
        return;
    size_t added = strlen(text);
    size_t length = editor.select_all ? 0 : strlen(editor.input);
    if (contains_line_break(text) || added >= editor.capacity - length) {
        set_notice("Enter a shorter, single-line value.", true);
        return;
    }
    if (editor.select_all) {
        editor.input[0] = '\0';
        editor.caret = 0;
        editor.select_all = false;
    }
    memmove(editor.input + editor.caret + added, editor.input + editor.caret, length - editor.caret + 1);
    memcpy(editor.input + editor.caret, text, added);
    editor.caret += added;
    update_input();
}

void app_editor_key(SDL_Keycode key)
{
    if (!app_editor_is_typing())
        return;
    SDL_Keymod mods = SDL_GetModState();
    if (mods & (KMOD_CTRL | KMOD_GUI)) {
        if (key == SDLK_a) {
            editor.select_all = true;
            editor.caret = strlen(editor.input);
            update_input();
        }
        else if (key == SDLK_v && SDL_HasClipboardText()) {
            char *clipboard = SDL_GetClipboardText();
            app_editor_text(clipboard);
            SDL_free(clipboard);
        }
        else if (key == SDLK_c && editor.select_all)
            SDL_SetClipboardText(editor.input);
        return;
    }

    switch (key) {
        case SDLK_ESCAPE: close_input(); return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: commit_input(); return;
        case SDLK_BACKSPACE: erase_input(true); return;
        case SDLK_DELETE: erase_input(false); return;
        case SDLK_LEFT:
            editor.caret = editor.select_all ? 0 : previous_codepoint(editor.input, editor.caret);
            break;
        case SDLK_RIGHT:
            editor.caret = editor.select_all ? strlen(editor.input) : next_codepoint(editor.input, editor.caret);
            break;
        case SDLK_HOME: editor.caret = 0; break;
        case SDLK_END: editor.caret = strlen(editor.input); break;
        default: return;
    }
    editor.select_all = false;
    update_input();
}

static void choose_row(EditorAction action)
{
    int index = page_start() + action - ITEM_ROW;
    if (editor.view == VIEW_LIBRARY)
        editor.library_index = index;
    else
        editor.selected = entry_at(index);
    editor.focus = action;
    reset_preview();
}

static void change_page(int direction)
{
    int target = page_start() + direction * EDITOR_PAGE_SIZE;
    if (editor.view == VIEW_LIBRARY)
        editor.library_first = target;
    else
        editor.list_first = target;
    choose_row(ITEM_ROW);
    update_items();
}

static void back(void)
{
    if (editor.view == VIEW_TEXT)
        close_input();
    else if (editor.view == VIEW_LIBRARY)
        return_from_library();
    else if (editor.view == VIEW_EDIT || editor.view == VIEW_CONFIRM) {
        return_to_list();
        set_notice("", false);
    }
    else
        close_editor();
}

static void apply(void)
{
    if (!editor.dirty) {
        close_editor();
        return;
    }

    char error[256] = {0};
    if (apply_menu_settings(editor.entries, error, sizeof(error))) {
        editor.saving = true;
        set_notice("Saving changes...", false);
    }
    else
        set_notice(error[0] ? error : "Could not save the applications.", true);
}

void app_editor_save_completed(bool success, const char *error)
{
    if (!app_editor_is_open() || !editor.saving)
        return;
    editor.saving = false;
    if (success)
        close_editor();
    else
        set_notice(error != NULL && *error ? error : "Could not save the applications.", true);
}

static void activate(EditorAction action)
{
    if (action >= ITEM_ROW) {
        int index = page_start() + action - ITEM_ROW;
        if (editor.view == VIEW_LIBRARY)
            choose_library_icon(index);
        else {
            choose_row(action);
            start_edit(editor.selected, false);
        }
        return;
    }
    switch (action) {
        case ITEM_ADD: start_edit(NULL, false); break;
        case ITEM_ADD_ASSISTANT:
            if (start_edit(NULL, true))
                start_input(ITEM_WEBSITE);
            break;
        case ITEM_EDIT: start_edit(editor.selected, false); break;
        case ITEM_UP: move_entry(true); break;
        case ITEM_DOWN: move_entry(false); break;
        case ITEM_REMOVE:
            editor.view = VIEW_CONFIRM;
            editor.focus = ITEM_CANCEL;
            update_items();
            set_notice("", false);
            break;
        case ITEM_CONFIRM: remove_entry(); break;
        case ITEM_SAVE: apply(); break;
        case ITEM_CANCEL:
        case ITEM_BACK: back(); break;
        case ITEM_NAME:
        case ITEM_COMMAND:
        case ITEM_WEBSITE: start_input(action); break;
        case ITEM_PROGRAM: choose_program(); break;
        case ITEM_IMPORT: pick_icon(); break;
        case ITEM_LIBRARY: open_library(); break;
        case ITEM_DONE:
            if (app_editor_is_typing())
                commit_input();
            else
                finish_edit();
            break;
        case ITEM_PREVIOUS: change_page(-1); break;
        case ITEM_NEXT: change_page(1); break;
        default: break;
    }
}

static void focus_action(EditorAction action)
{
    if (action != editor.focus) {
        if (action >= ITEM_ROW) {
            choose_row(action);
            // Refresh move actions when a different application row is selected.
            update_items();
        }
        else
            editor.focus = action;
        request_launcher_redraw();
    }
}

static void move_focus(int horizontal, int vertical)
{
    if (editor.focus == ITEM_ADD && horizontal > 0) {
        focus_action(ITEM_ADD_ASSISTANT);
        return;
    }
    if (editor.focus == ITEM_ADD_ASSISTANT && horizontal < 0) {
        focus_action(ITEM_ADD);
        return;
    }

    SDL_Rect origin = {0};
    for (int i = 0; i < editor.item_count; i++) {
        if (editor.items[i].action == editor.focus) {
            origin = editor.items[i].rect;
            break;
        }
    }
    int closest = SDL_MAX_SINT32;
    EditorAction next = editor.focus;
    for (int i = 0; i < editor.item_count; i++) {
        EditorItem *item = &editor.items[i];
        // Rows use Right to reach their actions. The template is in the Add row above.
        if (horizontal != 0 && editor.focus >= ITEM_ROW && item->action == ITEM_ADD_ASSISTANT)
            continue;
        int dx = item->rect.x + item->rect.w / 2 - (origin.x + origin.w / 2);
        int dy = item->rect.y + item->rect.h / 2 - (origin.y + origin.h / 2);
        int primary = horizontal != 0 ? dx * horizontal : dy * vertical;
        int secondary = horizontal != 0 ? abs(dy) : abs(dx);
        if (primary <= 0)
            continue;
        int score = primary + secondary * 2;
        if (score < closest) {
            closest = score;
            next = item->action;
        }
    }
    focus_action(next);
}

void app_editor_command(const char *command)
{
    if (!app_editor_is_open() || editor.saving || editor.importing || command == NULL)
        return;
    if (strcmp(command, SCMD_BACK) == 0 || strcmp(command, SCMD_HOME) == 0 ||
        strcmp(command, SCMD_QUIT) == 0)
        back();
    else if (app_editor_is_typing())
        return;
    else if (strcmp(command, SCMD_UP) == 0)
        move_focus(0, -1);
    else if (strcmp(command, SCMD_DOWN) == 0)
        move_focus(0, 1);
    else if (strcmp(command, SCMD_LEFT) == 0)
        move_focus(-1, 0);
    else if (strcmp(command, SCMD_RIGHT) == 0)
        move_focus(1, 0);
    else if (strcmp(command, SCMD_SELECT) == 0)
        activate(editor.focus);
    request_launcher_redraw();
}

void app_editor_mouse(int x, int y, bool click)
{
    if (!app_editor_is_open() || editor.saving || editor.importing)
        return;
    SDL_Point point = {x, y};
    for (int i = 0; i < editor.item_count; i++) {
        if (SDL_PointInRect(&point, &editor.items[i].rect)) {
            EditorAction action = editor.items[i].action;
            focus_action(action);
            if (click) {
                activate(action);
                request_launcher_redraw();
            }
            return;
        }
    }
}

static void draw_icon_preview(SDL_Renderer *renderer)
{
    SDL_Rect card = rect_for(1074, 251, 594, 263);
    draw_box(renderer, card, false);
    SDL_Rect icon_rect = rect_for(1174, 267, 394, 184);
    if (editor.preview != NULL) {
        SDL_Rect image = fit_tv_texture(editor.preview, icon_rect);
        SDL_RenderCopy(renderer, editor.preview, NULL, &image);
    }
    else
        draw_text(renderer, editor.preview_pending || job.type == JOB_PREVIEW ? editor.loading : editor.no_preview,
            icon_rect, true);
    draw_text(renderer, editor.preview_name, rect_for(1104, 454, 534, 49), true);
    draw_text(renderer, editor.preview_command, rect_for(1081, 522, 580, 48), true);
    if (editor.view == VIEW_EDIT)
        draw_text(renderer, editor.icon_binding, rect_for(1081, 580, 580, 48), true);
}

static void draw_input(SDL_Renderer *renderer)
{
    SDL_Rect field = rect_for(252, 385, 1416, 120);
    draw_box(renderer, field, true);
    SDL_Rect clip = rect_for(275, 396, 1370, 98);
    SDL_RenderSetClipRect(renderer, &clip);
    int horizontal = SDL_max(0, editor.caret_px - clip.w + scaled(8));
    if (editor.text != NULL) {
        SDL_Rect rect = {clip.x - horizontal, clip.y, 0, 0};
        SDL_QueryTexture(editor.text, NULL, NULL, &rect.w, &rect.h);
        rect.y += (clip.h - rect.h) / 2;
        if (editor.select_all)
            paint(renderer, rect, (SDL_Color){65, 95, 132, 255});
        SDL_RenderCopy(renderer, editor.text, NULL, &rect);
    }
    if (!editor.select_all) {
        SDL_Rect caret = {clip.x + editor.caret_px - horizontal, clip.y + scaled(20),
            SDL_max(1, scaled(2)), clip.h - scaled(40)};
        paint(renderer, caret, border);
    }
    SDL_RenderSetClipRect(renderer, NULL);
}

void app_editor_draw(SDL_Renderer *renderer)
{
    if (!app_editor_is_open())
        return;

    SDL_Color previous;
    SDL_GetRenderDrawColor(renderer, &previous.r, &previous.g, &previous.b, &previous.a);
    paint(renderer, rect_for(0, 0, 1920, 1080), (SDL_Color){11, 17, 29, 255});
    draw_text(renderer, editor.section, rect_for(252, 71, 600, 36), false);
    draw_text(renderer, editor.view == VIEW_TEXT ? editor.text_title : editor.titles[editor.view],
        rect_for(252, 106, 1416, 74), false);

    if (editor.view == VIEW_TEXT)
        draw_input(renderer);
    else if (editor.view == VIEW_CONFIRM) {
        draw_text(renderer, editor.preview_name, rect_for(252, 318, 1416, 95), false);
        draw_text(renderer, editor.confirm_message, rect_for(252, 438, 1416, 60), false);
    }
    else {
        draw_icon_preview(renderer);
        if (editor.view == VIEW_LIST || editor.view == VIEW_LIBRARY)
            draw_text(renderer, editor.page_label, rect_for(252, 763, 765, 54), false);
        if (editor.view == VIEW_LIST && editor.entries == NULL)
            draw_text(renderer, editor.empty_list, rect_for(252, 380, 765, 68), false);
        else if (editor.view == VIEW_LIBRARY && editor.icons.count == 0 && !editor.scan_pending &&
            job.type != JOB_SCAN)
            draw_text(renderer, editor.empty_library, rect_for(252, 380, 765, 68), false);
    }

    for (int i = 0; i < editor.item_count; i++) {
        EditorItem *item = &editor.items[i];
        SDL_Rect rect = item->rect;
        bool list_focus = editor.focus == item->action;
        if (item->action >= ITEM_ROW && editor.view == VIEW_LIST &&
            entry_at(editor.list_first + item->action - ITEM_ROW) == editor.selected && !list_focus)
            paint(renderer, rect, (SDL_Color){28, 46, 69, 255});
        else
            draw_box(renderer, rect, list_focus);
        if (item->left_align) {
            rect.x += scaled(24);
            rect.w -= scaled(48);
        }
        draw_text(renderer, item->text, rect, !item->left_align);
    }
    draw_text(renderer, editor.notice, rect_for(252, 916, 1416, 49), false);
    if (editor.view != VIEW_TEXT)
        draw_text(renderer, editor.instructions, rect_for(252, 971, 1416, 46), false);
    SDL_SetRenderDrawColor(renderer, previous.r, previous.g, previous.b, previous.a);
}

void app_editor_quit(void)
{
    close_editor();
    if (job.thread != NULL) {
        SDL_AtomicSet(&job.stop_requested, 1);
        SDL_WaitThread(job.thread, NULL);
    }
    clear_job();
}
