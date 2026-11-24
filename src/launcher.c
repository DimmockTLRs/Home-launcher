#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include <launcher_config.h>
#include "image.h"
#include "background.h"
#include "config_save.h"
#include "settings_ui.h"
#include "tv_ui.h"
#include "weather.h"
#include "util.h"
#include "debug.h"
#include "clock.h"
#include "platform/platform.h"

static void init_sdl(void);
static void init_sdl_image(void);
static void create_window(void);
static void init_sdl_ttf(void);
static int load_menu(Menu *menu, bool set_back_menu, bool reset_position);
static int load_menu_by_name(const char *menu_name, bool set_back_menu, bool reset_position);
static void update_screensaver(void);
static bool update_clock(void);
static void init_screensaver(void);
static void calculate_button_geometry(Entry *entry, int buttons);
static void render_buttons(Menu *menu);
static void move_left(void);
static void move_right(void);
static void load_submenu(const char *submenu);
static void load_back_menu(Menu *menu);
static void draw_screen(void);
static void handle_keypress(SDL_Keysym *key);
static void execute_command(const char *command);
static void poll_gamepad(void);
static void init_gamepad(Gamepad **gamepad, int device_index);
static void connect_gamepad(int device_index, bool open, bool raise_error);
static void disconnect_gamepad(int id, bool disconnect, bool remove);
static void open_controller(Gamepad *gamepad, bool raise_error);
static void cleanup(void);

// Initialize default settings
Config config = {
    .default_menu                     = NULL,
    .background_image                 = NULL,
    .slideshow_directory              = NULL,
    .title_font_path                  = NULL,
    .vsync                            = true,
    .fps_limit                        = -1,
    .application_timeout              = DEFAULT_APPLICATION_TIMEOUT * 1000,
    .titles_enabled                   = DEFAULT_TITLES_ENABLED,
    .title_font_size                  = DEFAULT_FONT_SIZE,
    .title_font_color.r               = DEFAULT_TITLE_FONT_COLOR_R,
    .title_font_color.g               = DEFAULT_TITLE_FONT_COLOR_G,
    .title_font_color.b               = DEFAULT_TITLE_FONT_COLOR_B,
    .title_font_color.a               = DEFAULT_TITLE_FONT_COLOR_A,
    .title_shadows                    = DEFAULT_TITLE_SHADOWS,
    .title_shadow_color.r             = DEFAULT_TITLE_SHADOW_COLOR_R,
    .title_shadow_color.g             = DEFAULT_TITLE_SHADOW_COLOR_G,
    .title_shadow_color.b             = DEFAULT_TITLE_SHADOW_COLOR_B,
    .title_shadow_color.a             = DEFAULT_TITLE_SHADOW_COLOR_A,
    .background_mode                  = BACKGROUND_COLOR,
    .photo_fit                        = PHOTO_CONTAIN,
    .weather.latitude                 = 999,
    .weather.longitude                = 999,
    .collage_layout                   = COLLAGE_AUTO,
    .photo_crop_x                     = 50,
    .photo_crop_y                     = 50,
    .background_color.r               = DEFAULT_BACKGROUND_COLOR_R,
    .background_color.g               = DEFAULT_BACKGROUND_COLOR_G,
    .background_color.b               = DEFAULT_BACKGROUND_COLOR_B,
    .chroma_key_color.r               = DEFAULT_CHROMA_KEY_COLOR_R,
    .chroma_key_color.g               = DEFAULT_CHROMA_KEY_COLOR_G,
    .chroma_key_color.b               = DEFAULT_CHROMA_KEY_COLOR_B,
    .chroma_key_color.a               = DEFAULT_CHROMA_KEY_COLOR_A,
    .background_color.a               = 0xFF,
    .background_overlay               = DEFAULT_BACKGROUND_OVERLAY,
    .background_overlay_color.r       = DEFAULT_BACKGROUND_OVERLAY_COLOR_R,
    .background_overlay_color.g       = DEFAULT_BACKGROUND_OVERLAY_COLOR_G,
    .background_overlay_color.b       = DEFAULT_BACKGROUND_OVERLAY_COLOR_B,
    .background_overlay_color.a       = DEFAULT_BACKGROUND_OVERLAY_COLOR_A,
    .background_overlay_opacity[0]    = '\0',
    .highlight                        = true,
    .icon_size                        = DEFAULT_ICON_SIZE,
    .highlight_fill_color.r           = DEFAULT_HIGHLIGHT_FILL_COLOR_R,
    .highlight_fill_color.g           = DEFAULT_HIGHLIGHT_FILL_COLOR_G,
    .highlight_fill_color.b           = DEFAULT_HIGHLIGHT_FILL_COLOR_B,
    .highlight_fill_color.a           = DEFAULT_HIGHLIGHT_FILL_COLOR_A,
    .highlight_outline_color.r        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_R,
    .highlight_outline_color.g        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_G,
    .highlight_outline_color.b        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_B,
    .highlight_outline_color.a        = DEFAULT_HIGHLIGHT_OUTLINE_COLOR_A,
    .highlight_outline_size           = DEFAULT_HIGHLIGHT_OUTLINE_SIZE,
    .highlight_rx                     = DEFAULT_HIGHLIGHT_CORNER_RADIUS,
    .title_padding                    = -1,
    .max_buttons                      = DEFAULT_MAX_BUTTONS,
    .icon_spacing                     = -1,
    .highlight_vpadding               = -1,
    .highlight_hpadding               = -1,
    .title_opacity[0]                 = '\0',
    .highlight_fill_opacity[0]        = '\0',
    .highlight_outline_opacity[0]     = '\0',
    .vcenter[0]             = '\0',
    .icon_spacing_str[0]              = '\0',
    .scroll_indicators                = DEFAULT_SCROLL_INDICATORS,
    .scroll_indicator_fill_color.r    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_R,
    .scroll_indicator_fill_color.g    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_G,
    .scroll_indicator_fill_color.b    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_B,
    .scroll_indicator_fill_color.a    = DEFAULT_SCROLL_INDICATOR_FILL_COLOR_A,
    .scroll_indicator_outline_size    = DEFAULT_SCROLL_INDICATOR_OUTLINE_SIZE,
    .scroll_indicator_outline_color.r = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_R,
    .scroll_indicator_outline_color.g = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_G,
    .scroll_indicator_outline_color.b = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_B,
    .scroll_indicator_outline_color.a = DEFAULT_SCROLL_INDICATOR_OUTLINE_COLOR_A,
    .scroll_indicator_opacity[0]      = '\0',
    .title_oversize_mode              = OVERSIZE_TRUNCATE,
    .wrap_entries                     = DEFAULT_WRAP_ENTRIES,
    .reset_on_back                    = DEFAULT_RESET_ON_BACK,
    .mouse_select                     = DEFAULT_MOUSE_SELECT,
    .inhibit_os_screensaver           = DEFAULT_INHIBIT_OS_SCREENSAVER,
    .startup_cmd                      = NULL,
    .quit_cmd                         = NULL,
    .screensaver_enabled              = false,
    .screensaver_idle_time            = DEFAULT_SCREENSAVER_IDLE_TIME*1000,
    .screensaver_intensity_str[0]     = '\0',
    .screensaver_pause_slideshow      = DEFAULT_SCREENSAVER_PAUSE_SLIDESHOW,
    .gamepad_enabled                  = DEFAULT_GAMEPAD_ENABLED,
    .gamepad_device                   = DEFAULT_GAMEPAD_DEVICE,
    .gamepad_mappings_file            = NULL,
    .on_launch                        = ON_LAUNCH_BLANK,
    .debug                            = false,
    .exe_path                         = NULL,
    .first_menu                       = NULL,
    .num_menus                        = 0,
    .clock_enabled                    = DEFAULT_CLOCK_ENABLED,
    .clock_show_date                  = DEFAULT_CLOCK_SHOW_DATE,
    .clock_alignment                  = DEFAULT_CLOCK_ALIGNMENT,
    .clock_font_path                  = NULL,
    .clock_margin_str[0]              = '\0',
    .clock_margin                     = -1,
    .clock_font_color.r               = DEFAULT_CLOCK_FONT_COLOR_R,
    .clock_font_color.g               = DEFAULT_CLOCK_FONT_COLOR_G,
    .clock_font_color.b               = DEFAULT_CLOCK_FONT_COLOR_B,
    .clock_font_color.a               = DEFAULT_CLOCK_FONT_COLOR_A,
    .clock_shadows                    = DEFAULT_CLOCK_SHADOWS,
    .clock_shadow_color.r             = DEFAULT_CLOCK_SHADOW_COLOR_R,
    .clock_shadow_color.g             = DEFAULT_CLOCK_SHADOW_COLOR_G,
    .clock_shadow_color.b             = DEFAULT_CLOCK_SHADOW_COLOR_B,
    .clock_shadow_color.a             = DEFAULT_CLOCK_SHADOW_COLOR_A,
    .clock_opacity[0]                 = '\0',
    .clock_font_size                  = DEFAULT_CLOCK_FONT_SIZE,
    .clock_time_format                = DEFAULT_CLOCK_TIME_FORMAT,
    .clock_date_format                = DEFAULT_CLOCK_DATE_FORMAT,
    .clock_include_weekday            = DEFAULT_CLOCK_INCLUDE_WEEKDAY,
    .slideshow_image_duration         = DEFAULT_SLIDESHOW_IMAGE_DURATION,
    .slideshow_transition_time        = DEFAULT_SLIDESHOW_TRANSITION_TIME
};

// Initialize default states
State state = { false };

// Global variables
SDL_Window *window                    = NULL;
SDL_Renderer *renderer                = NULL;
SDL_Texture *background_overlay       = NULL;
Menu *default_menu                    = NULL;
Menu *current_menu                    = NULL;
Entry *current_entry                  = NULL;
Highlight *highlight                  = NULL;
Scroll *scroll                        = NULL;
Screensaver *screensaver              = NULL;
FILE *log_file                        = NULL;
Gamepad *gamepads                     = NULL;
GamepadControl *gamepad_controls      = NULL;
Hotkey *hotkeys                       = NULL;
Clock *clk                            = NULL;
SDL_Event event;
SDL_SysWMinfo wm_info;
SDL_DisplayMode display_mode;
TextInfo title_info;
Ticks ticks;
Geometry geo;
Uint32 refresh_period;
static bool redraw_requested = true;


static char *config_file_path;
static Entry *settings_entry;
static SDL_Rect settings_card_bounds;
static bool settings_key_latched[SDL_NUM_SCANCODES];
static struct {
    SDL_Thread *thread;
    SDL_atomic_t ready;
    BackgroundSettings before;
    BackgroundSettings after;
    bool saved;
    char error[256];
} settings_save;

void request_launcher_redraw(void)
{
    redraw_requested = true;
}

static int save_settings_worker(void *unused)
{
    (void)unused;
    settings_save.saved = save_background_config(config_file_path, &settings_save.after,
        &settings_save.before, settings_save.error, sizeof(settings_save.error));
    SDL_AtomicSet(&settings_save.ready, 1);
    return 0;
}

bool apply_background_settings(const BackgroundSettings *settings, char *error, size_t error_size)
{
    if (settings_save.thread != NULL) {
        SDL_strlcpy(error, "A save is already in progress.", error_size);
        return false;
    }
    settings_save.before = (BackgroundSettings){
        .mode = config.background_mode,
        .layout = config.collage_layout,
        .image_duration = config.slideshow_image_duration,
        .transition_time = config.slideshow_transition_time,
    };
    settings_save.after = *settings;
    settings_save.error[0] = '\0';
    SDL_AtomicSet(&settings_save.ready, 0);
    settings_save.thread = SDL_CreateThread(save_settings_worker, "Save launcher settings", NULL);
    if (settings_save.thread == NULL) {
        SDL_strlcpy(error, "Could not start the settings save.", error_size);
        return false;
    }
    return true;
}

static void finish_settings_save(void)
{
    if (settings_save.thread == NULL || !SDL_AtomicGet(&settings_save.ready))
        return;
    SDL_WaitThread(settings_save.thread, NULL);
    settings_save.thread = NULL;
    if (settings_save.saved) {
        const BackgroundSettings *after = &settings_save.after;
        bool mode_changed = config.background_mode != after->mode;
        bool layout_changed = config.collage_layout != after->layout;
#ifdef _WIN32
        if (mode_changed && config.background_mode == BACKGROUND_TRANSPARENT)
            restore_opaque_window();
#endif
        config.background_mode = after->mode;
        config.collage_layout = after->layout;
        config.slideshow_image_duration = after->image_duration;
        config.slideshow_transition_time = after->transition_time;
        if (mode_changed || (layout_changed && after->mode == BACKGROUND_COLLAGE)) {
            set_draw_color();
            reload_background(renderer);
        }
        else
            restart_background_interval(SDL_GetTicks());
        log_debug("Background settings saved to %s", config_file_path);
    }
    settings_save_completed(settings_save.saved, settings_save.error);
    request_launcher_redraw();
}

static void add_settings_card(Menu *menu)
{
    Entry *last = NULL;
    for (Entry *entry = menu->first_entry; entry != NULL; entry = entry->next)
        last = entry;

    settings_entry = calloc(1, sizeof(*settings_entry));
    if (settings_entry == NULL)
        log_fatal("Could not allocate Settings card");
    settings_entry->title = strdup("Settings");
    settings_entry->icon_path = strdup("(built-in gear)");
    settings_entry->cmd = strdup(SCMD_SETTINGS);
    if (last == NULL)
        menu->first_entry = settings_entry;
    else
        last->next = settings_entry;
    settings_entry->previous = last;
    menu->num_entries++;
    if (settings_entry->title == NULL || settings_entry->icon_path == NULL || settings_entry->cmd == NULL)
        log_fatal("Could not allocate Settings card text");
}

static SDL_Texture *settings_gear_icon(void)
{
    char gear[] =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 96 96'>"
        "<path fill='#E4EEFF' fill-rule='evenodd' d='"
        "M40 7H56L59 19L66 22L77 15L88 26L81 37L84 44L96 47V57L84 60L81 67L88 78L77 89"
        "L66 82L59 85L56 96H40L37 85L30 82L19 89L8 78L15 67L12 60L0 57V47L12 44L15 37L8 26"
        "L19 15L30 22L37 19Z M65 52A17 17 0 1 0 31 52A17 17 0 1 0 65 52Z'/></svg>";
    return rasterize_svg(gear, 192, 192, NULL);
}

#define NAVIGATION_POP_MS 200u
#define NAVIGATION_AUDIO_RATE 48000
#define NAVIGATION_AUDIO_SAMPLES (NAVIGATION_AUDIO_RATE * 65 / 1000)

static Entry *navigation_entry;
static Uint32 navigation_started;
static SDL_AudioDeviceID navigation_device;
static float navigation_tone[NAVIGATION_AUDIO_SAMPLES];

static void init_navigation_audio(void)
{
    if (!config.navigation_sound)
        return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        log_error("Navigation sound unavailable: %s", SDL_GetError());
        return;
    }

    SDL_AudioSpec spec = {
        .freq = NAVIGATION_AUDIO_RATE,
        .format = AUDIO_F32SYS,
        .channels = 1,
        .samples = 256,
    };
    navigation_device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (navigation_device == 0) {
        log_error("Navigation sound unavailable: %s", SDL_GetError());
        return;
    }

    // A short, softly decaying two-tone chime. Fade both ends to avoid clicks.
    for (int i = 0; i < NAVIGATION_AUDIO_SAMPLES; i++) {
        float t = (float)i / NAVIGATION_AUDIO_RATE;
        float attack = SDL_min(1.0f, t / 0.006f);
        float decay = 1.0f - (float)i / (NAVIGATION_AUDIO_SAMPLES - 1);
        float wave = sinf(6.2831853f * 660.0f * t) + 0.25f * sinf(6.2831853f * 990.0f * t);
        navigation_tone[i] = 0.13f * attack * decay * decay * wave;
    }
    SDL_PauseAudioDevice(navigation_device, 0);
}

static void navigation_moved(const Entry *previous, SDL_Rect previous_card, int page_direction)
{
    if (previous == current_entry)
        return;
    redraw_requested = true;
    if (config.tv_layout)
        move_tv_focus(previous_card, current_entry->card_rect.x, page_direction);
    if (config.navigation_pop) {
        navigation_entry = current_entry;
        navigation_started = SDL_GetTicks();
    }
    if (navigation_device != 0 && SDL_GetQueuedAudioSize(navigation_device) == 0 &&
        SDL_QueueAudio(navigation_device, navigation_tone, sizeof(navigation_tone)) < 0)
        log_error("Could not play navigation sound: %s", SDL_GetError());
}

static SDL_Rect navigation_icon_rect(const Entry *entry)
{
    SDL_Rect rect = entry->icon_rect;
    if (entry == navigation_entry) {
        Uint32 elapsed = SDL_GetTicks() - navigation_started;
        if (elapsed >= NAVIGATION_POP_MS) {
            navigation_entry = NULL;
            return rect;
        }

        // Grow quickly and settle more slowly. Bound the expansion so the
        // logo keeps its aspect ratio and clears its neighbours and title.
        float phase = elapsed / (float)NAVIGATION_POP_MS;
        float progress = phase < 0.25f ? 4.0f * phase : (1.0f - phase) / 0.75f;
        float eased = progress * progress * (3.0f - 2.0f * progress);
        int spacing = config.tv_layout ? rect.y - entry->card_rect.y : config.icon_spacing / 2;
        int space = SDL_min(spacing, SDL_min(rect.x, geo.screen_width - rect.x - rect.w));
        space = SDL_min(space, SDL_min(rect.y, geo.screen_height - rect.y - rect.h));
        if (config.titles_enabled)
            space = SDL_min(space, SDL_max(0, entry->text_rect.y - rect.y - rect.h));
        int inset = SDL_max(0, SDL_min(space, (int)(rect.w * 0.045f * eased + 0.5f)));
        rect.x -= inset;
        rect.y -= inset;
        rect.w += 2 * inset;
        rect.h += 2 * inset;
    }
    return rect;
}

// A function to initialize SDL
static void init_sdl()
{    
    // Set flags, hints
    Uint32 sdl_flags = SDL_INIT_VIDEO;
#ifdef __unix__
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, config.inhibit_os_screensaver ? "0" : "1");
    if (config.gamepad_enabled)
        sdl_flags |= SDL_INIT_GAMECONTROLLER;

    // Initialize SDL
    if (SDL_Init(sdl_flags) < 0)
        log_fatal("Could not initialize SDL\n%s", SDL_GetError());

    if (SDL_GetDesktopDisplayMode(0, &display_mode) < 0)
        log_fatal("Could not read desktop display mode: %s", SDL_GetError());
    if (display_mode.refresh_rate <= 0) {
        log_debug("Desktop refresh rate unavailable, using 60 Hz for timing");
        display_mode.refresh_rate = 60;
    }
    geo.screen_width = display_mode.w;
    geo.screen_height = display_mode.h;
    refresh_period = SDL_max(1u, 1000 / (Uint32) display_mode.refresh_rate);
    geo.screen_margin = (int) (SCREEN_MARGIN * (float) geo.screen_height);
}

// A function to create the window and renderer
static void create_window()
{
    window = SDL_CreateWindow(PROJECT_NAME,
                 SDL_WINDOWPOS_UNDEFINED,
                 SDL_WINDOWPOS_UNDEFINED,
                 0,
                 0,
                 SDL_WINDOW_FULLSCREEN_DESKTOP
             );
    if (window == NULL)
        log_fatal("Could not create SDL Window\n%s", SDL_GetError());
    SDL_ShowCursor(SDL_DISABLE);

    // Create HW accelerated renderer, get screen resolution for geometry calculations
    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
    if (!config.vsync) {
        if (config.fps_limit > MIN_FPS_LIMIT && config.fps_limit <= display_mode.refresh_rate)
            refresh_period = SDL_max(1u, 1000 / (Uint32) config.fps_limit);
        else
            config.vsync = true;
    }
    if (config.vsync) {
        refresh_period = SDL_max(1u, 1000 / (Uint32) display_mode.refresh_rate);
        renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
    }
    renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if (renderer == NULL)
        log_fatal("Could not initialize renderer\n%s", SDL_GetError());
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Set background color
    set_draw_color();

#ifdef _WIN32
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info))
        log_fatal("Could not get the Windows launcher window: %s", SDL_GetError());
    if (config.background_mode == BACKGROUND_TRANSPARENT)
        make_window_transparent();
#endif
}

// A function to initialize the SDL_image library
static void init_sdl_image()
{
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
    if (!(IMG_Init(img_flags) & img_flags))
        log_fatal("Could not initialize SDL_image\n%s", IMG_GetError());
}

// A function to set the color of the renderer
void set_draw_color()
{
    SDL_Color *color = NULL;
    if (config.background_mode == BACKGROUND_TRANSPARENT)
        color = &config.chroma_key_color;

    else
        color = &config.background_color;

    SDL_SetRenderDrawColor(renderer, color->r, color->g, color->b, color->a);
}

// A function to initialize SDL's TTF subsystem
static void init_sdl_ttf()
{
    if (TTF_Init() == -1)
        log_fatal("Could not initialize SDL_ttf\n%s", TTF_GetError());
    
    title_info = (TextInfo) { 
        .font_size = (int) config.title_font_size,
        .shadow = config.title_shadows,
        .font_path = &config.title_font_path,
        .max_width = config.icon_size,
        .oversize_mode = config.title_oversize_mode,
        .color = &config.title_font_color
    };
    if (config.title_shadows) {
        title_info.shadow_color = &config.title_shadow_color;
        calculate_shadow_alpha(title_info);
    }
    else
        title_info.shadow_color = NULL;

    int error = load_font(&title_info, FILENAME_DEFAULT_FONT);
    if (error)
        log_fatal("Could not load title font");
    geo.font_height = config.titles_enabled ? TTF_FontHeight(title_info.font) : 0;
}

// A function to close subsystems and free memory before quitting
static void cleanup()
{
    if (settings_save.thread != NULL)
        SDL_WaitThread(settings_save.thread, NULL);
    settings_quit();
    quit_background();
    quit_tv_ui();
    quit_weather();
    if (gamepads != NULL)
        disconnect_gamepad(-1, true, true);
    if (title_info.font != NULL)
        TTF_CloseFont(title_info.font);
    if (clk != NULL && clk->text_info.font != NULL)
        TTF_CloseFont(clk->text_info.font);
    
    // Destroy renderer and window
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
        window = NULL;
    }

    if (navigation_device != 0)
        SDL_CloseAudioDevice(navigation_device);

    // Quit subsystems
    quit_svg();
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();

    // Close log file if open
    if (log_file != NULL)
        fclose(log_file);

    // Free dynamically allocated memory
    free(config_file_path);
    free(config.default_menu);
    free(config.background_image);
    free(config.title_font_path);
    SDL_free(config.exe_path);
    free(config.slideshow_directory);
    free(config.clock_font_path);
    free(config.gamepad_mappings_file);
    free(config.startup_cmd);
    free(config.quit_cmd);
    free(highlight);
    free(scroll);
    free(screensaver);
    free(clk);

    // Free menu and entry linked lists
    Entry *entry = NULL;
    Entry *tmp_entry = NULL;
    Menu *menu = config.first_menu;
    Menu *tmp_menu = NULL;
    for (size_t i = 0; i < config.num_menus; i++) {
        free(menu->name);
        entry = menu->first_entry;
        for(size_t j = 0; j < menu->num_entries; j++) {
            free(entry->title);
            free(entry->icon_path);
            free(entry->icon_selected_path);
            free(entry->cmd);
            tmp_entry = entry;
            entry = entry->next;
            free(tmp_entry);
        }
        tmp_menu = menu;
        menu = menu->next;
        free(tmp_menu);
    }

    // Free hotkey linked list
    Hotkey *tmp_hotkey = NULL;
    for(Hotkey *i = hotkeys; i != NULL; i = i->next) {
        free(tmp_hotkey);
        free(i->cmd);
        tmp_hotkey = i;
    }
    free(tmp_hotkey);

    // Free gamepad control linked list
    GamepadControl *tmp_gamepad = NULL;
    for (GamepadControl *i = gamepad_controls; i != NULL; i = i->next) {
        free(tmp_gamepad);
        free(i->cmd);
        tmp_gamepad = i;
    }
    free(tmp_gamepad);
}

// A function to handle key presses from keyboard
static void handle_keypress(SDL_Keysym *key)
{
    if (config.debug)
        log_debug("Key %s (#%X) detected", SDL_GetKeyName(key->sym), key->sym);

    if (settings_is_open()) {
        switch (key->sym) {
            case SDLK_UP: settings_command(SCMD_UP); break;
            case SDLK_DOWN: settings_command(SCMD_DOWN); break;
            case SDLK_LEFT: settings_command(SCMD_LEFT); break;
            case SDLK_RIGHT: settings_command(SCMD_RIGHT); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE: settings_command(SCMD_SELECT); break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE: settings_command(SCMD_BACK); break;
            default: break;
        }
        return;
    }

    // Check default keys
    if (key->sym == SDLK_LEFT)
        move_left();
    else if (key->sym == SDLK_RIGHT)
        move_right();
    else if (key->sym == SDLK_RETURN) {
        log_debug("Selected Entry:\n"
            "Title: %s\n"
            "Icon Path: %s\n"
            "Command: %s", 
            current_entry->title, 
            current_entry->icon_path, 
            current_entry->cmd
        );
        
        execute_command(current_entry->cmd);
    }
    else if (key->sym == SDLK_BACKSPACE)
        load_back_menu(current_menu);

    //Check hotkeys
    else {
        for (Hotkey *i = hotkeys; i != NULL; i = i->next) {
            if (key->sym == i->keycode) {
                execute_command(i->cmd);
                return;
            }
        }
        if (key->sym == SDLK_F1 && current_menu == default_menu)
            execute_command(SCMD_SETTINGS);
    }
}

// A function to initialize the screensaver feature
static void init_screensaver()
{
    // Allocate memory for structure
    screensaver = malloc(sizeof(Screensaver));
    
    // Convert intensity string to float
    char intensity[PERCENT_MAX_CHARS];
    if (config.screensaver_intensity_str[0] != '\0')
        copy_string(intensity, config.screensaver_intensity_str, sizeof(intensity));
    else
        copy_string(intensity, DEFAULT_SCREENSAVER_INTENSITY, sizeof(intensity));
    size_t length = strlen(intensity);
    intensity[length - 1] = '\0';
    float percent = (float) atof(intensity);

    // Calculate alpha end value
    screensaver->alpha_end_value = 255.0f * percent / 100.0f;
    if (screensaver->alpha_end_value < 1.0f) {
        log_error("Invalid screensaver intensity value, disabling feature");
        config.screensaver_enabled = false;
        free(screensaver);
        screensaver = NULL;
        return;
    }
    else if (screensaver->alpha_end_value >= 255.0f)
        screensaver->alpha_end_value = 255.0f;

    screensaver->transition_change_rate = screensaver->alpha_end_value / ((float) SCREENSAVER_TRANSITION_TIME / (float) refresh_period);
    
    // Render texture
    SDL_Surface *surface = NULL;
    surface = SDL_CreateRGBSurfaceWithFormat(0, 
                  geo.screen_width, 
                  geo.screen_height, 
                  32,
                  SDL_PIXELFORMAT_ARGB8888
              );
    Uint32 color = SDL_MapRGBA(surface->format, 0, 0, 0, 0xFF);
    SDL_FillRect(surface, NULL, color);
    screensaver->texture = load_texture(surface);
    screensaver->alpha = 0.0f;
    SDL_SetTextureAlphaMod(screensaver->texture, 0.0f);
}

// A function to load a menu
static int load_menu(Menu *menu, bool set_back_menu, bool reset_position)
{
    if (menu == NULL)
        return 1;

    unsigned int buttons;
    Menu *previous_menu = current_menu;

    current_menu = menu;
    log_debug("Loading menu '%s'", current_menu->name);

    // Return error if the menu doesn't contain entires
    if (current_menu->num_entries == 0) {
        log_error("No valid entries found for Menu '%s'", current_menu->name);
        current_menu = previous_menu;
        return 1;
    }

    // Render the menu if not already rendered
    if (current_menu->rendered == false)
        render_buttons(current_menu);

    // Set menu properties
    if (set_back_menu)
        current_menu->back = previous_menu;

    navigation_entry = NULL;
    reset_tv_motion();
    redraw_requested = true;
    if (reset_position) {
        current_entry = current_menu->first_entry;
        current_menu->root_entry = current_entry;
        current_menu->highlight_position = 0;
        current_menu->page = 0;
    }
    else
        current_entry = current_menu->last_selected_entry;

    buttons = current_menu->num_entries - (current_menu->page)*config.max_buttons;
    if (buttons > config.max_buttons)
        buttons = config.max_buttons;
    
    // Recalculate the screen geometry
    calculate_button_geometry(current_menu->root_entry, (int) buttons);
    if (config.highlight && !config.tv_layout) {
        highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        highlight->rect.y = current_entry->icon_rect.y - config.highlight_vpadding;
    }
    return 0;
}

// A function to load a menu by its name
static int load_menu_by_name(const char *menu_name, bool set_back_menu, bool reset_position)
{
    Menu *menu = get_menu(menu_name);
    return load_menu(menu, set_back_menu, reset_position);
}

// A function to calculate the layout of the buttons
static void calculate_button_geometry(Entry *entry, int buttons)
{
    if (config.tv_layout) {
        layout_tv_entries(entry, buttons);
        return;
    }

    // Calculate proper spacing
    geo.x_margin = (geo.screen_width - config.icon_size*buttons -
                   buttons*config.icon_spacing + config.icon_spacing) / 2;
    geo.x_advance = config.icon_size + config.icon_spacing;
    geo.num_buttons = buttons;

    // Assign values to entries
    for (int i = 0; i < geo.num_buttons; i++) {
            entry->icon_rect.x = geo.x_margin + i*geo.x_advance;
            entry->icon_rect.y = geo.y_margin;
            entry->icon_rect.w = config.icon_size;
            entry->icon_rect.h = config.icon_size;
            entry->text_rect.x = entry->icon_rect.x +
                                 (entry->icon_rect.w - entry->text_rect.w) / 2;
            entry->text_rect.y = entry->icon_rect.y + config.icon_size + entry->title_offset + 
                                 config.title_padding;
            entry = entry->next;
    }
}

// A function to render all buttons (icon and text) for a menu
static void render_buttons(Menu *menu)
{
    Entry *entry;
    int h;
    for (entry = menu->first_entry; entry != NULL; entry = entry->next) {
        entry->icon = entry == settings_entry ? settings_gear_icon() : load_texture_from_file(entry->icon_path);
        entry->icon_selected = (entry->icon_selected_path != NULL) ? load_texture_from_file(entry->icon_selected_path) : NULL;
        if (config.titles_enabled) {
            entry->title_texture = render_text_texture(entry->title, &title_info, &entry->text_rect, &h);
            if (config.title_oversize_mode == OVERSIZE_SHRINK && h != geo.font_height)
                entry->title_offset = (geo.font_height - h) / 2;
        }
    }
    menu->rendered = true;
}

// A function to move the selection left when clicked by user
static void move_left()
{
    Entry *previous = current_entry;
    SDL_Rect previous_card = previous->card_rect;
    unsigned int previous_page = current_menu->page;
    // If we are not in leftmost position, move highlight left
    if (current_menu->highlight_position > 0) {
        if (config.highlight && !config.tv_layout)
            highlight->rect.x -= geo.x_advance;
        current_menu->highlight_position--;
        current_entry = current_entry->previous;
    }

    // If we are in leftmost position...
    else if (current_menu->highlight_position == 0 && (current_menu->page > 0 || config.wrap_entries)) {
        unsigned int buttons;
        current_entry = current_entry->previous;

        // Load the previous page if there is a valid previous entry
        if (current_entry) {
            buttons = config.max_buttons;
            current_menu->root_entry = advance_entries(current_menu->root_entry, (int) buttons, DIRECTION_LEFT);
            current_menu->page--;
        }

        // If the user has the wrap entries setting, select the last entry in the menu
        else {
            current_entry = advance_entries(current_menu->first_entry, (int) current_menu->num_entries - 1, DIRECTION_RIGHT);
            unsigned int num_pages = DIV_ROUND_UP(current_menu->num_entries, config.max_buttons);
            current_menu->root_entry = advance_entries(current_menu->root_entry,
                (int) ((num_pages - 1 - current_menu->page) * config.max_buttons),
                DIRECTION_RIGHT
            );
            current_menu->page = num_pages - 1;
            buttons = current_menu->num_entries - current_menu->page * config.max_buttons;
        }

        calculate_button_geometry(current_menu->root_entry, (int) buttons);
        if (config.highlight && !config.tv_layout)
            highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        current_menu->highlight_position = buttons - 1;
    }
    navigation_moved(previous, previous_card, current_menu->page == previous_page ? 0 : -1);
}

// A function to move the selection right when clicked by the user
static void move_right()
{
    Entry *previous = current_entry;
    SDL_Rect previous_card = previous->card_rect;
    unsigned int previous_page = current_menu->page;
    // If we are not in the rightmost position, move highlight right
    if ((int) current_menu->highlight_position < (geo.num_buttons - 1)) {
        if (config.highlight && !config.tv_layout)
            highlight->rect.x += geo.x_advance;
        current_menu->highlight_position++;
        current_entry = current_entry->next;
    }

    // If we are in the rightmost postion, but there are more entries in the menu, load next page
    else if (current_menu->highlight_position + current_menu->page*config.max_buttons <
    (current_menu->num_entries - 1)) {
        unsigned int buttons = current_menu->num_entries - (current_menu->page + 1)*config.max_buttons;
        if (buttons > config.max_buttons)
            buttons = config.max_buttons;
        current_entry = current_entry->next;
        current_menu->root_entry = current_entry;
        calculate_button_geometry(current_menu->root_entry, (int) buttons);
        if (config.highlight && !config.tv_layout)
            highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        current_menu->page++;
        current_menu->highlight_position = 0;
    }

    // If user has the wrap entries setting, reset menu to first entry
    else if (config.wrap_entries) {
        current_entry = current_menu->first_entry;
        current_menu->root_entry = current_entry;
        current_menu->highlight_position = 0;
        current_menu->page = 0;
        if (config.highlight && !config.tv_layout)
            highlight->rect.x = current_entry->icon_rect.x - config.highlight_hpadding;
        calculate_button_geometry(current_menu->root_entry, (int) MIN(current_menu->num_entries, config.max_buttons));
    }
    navigation_moved(previous, previous_card, current_menu->page == previous_page ? 0 : 1);
}

// A function to load a submenu
static void load_submenu(const char *submenu)
{
    current_menu->last_selected_entry = current_entry;
    load_menu_by_name(submenu, true, true);
}

// A function to load the previous menu
static void load_back_menu(Menu *menu)
{
    load_menu(menu->back, false, config.reset_on_back);
}

// A function to update the screen with all visible textures
static void draw_screen()
{
    settings_card_bounds = (SDL_Rect){0};
    SDL_RenderClear(renderer);
    if (settings_is_open()) {
        settings_draw(renderer);
        if (state.screensaver_active)
            SDL_RenderCopy(renderer, screensaver->texture, NULL, NULL);
    }
    else if (!(state.application_launching && config.on_launch == ON_LAUNCH_BLANK)) {
        draw_background(renderer);

        // Draw background overlay
        if (config.background_overlay)
            SDL_RenderCopy(renderer, background_overlay, NULL, NULL);

        // Draw scroll indicators
        if (config.scroll_indicators &&
        (current_menu->page*config.max_buttons + (unsigned int) geo.num_buttons) <= (current_menu->num_entries - 1))
            SDL_RenderCopy(renderer, scroll->texture, NULL, &scroll->rect_right);

        if (config.scroll_indicators && current_menu->page > 0)
            SDL_RenderCopyEx(renderer, scroll->texture, NULL, &scroll->rect_left, 0, NULL, SDL_FLIP_HORIZONTAL);

        // Draw clock
        if (config.clock_enabled) {
            SDL_RenderCopy(renderer, clk->time_texture, NULL, &clk->time_rect);
            if (config.clock_show_date)
                SDL_RenderCopy(renderer, clk->date_texture, NULL, &clk->date_rect);
        }

        // Draw weather on the default menu, opposite the clock.
        if (current_menu == default_menu)
            draw_weather(renderer, geo.screen_width, geo.screen_height,
                config.clock_enabled && config.clock_alignment == ALIGNMENT_LEFT);

        // Draw highlight
        if (config.highlight && !config.tv_layout)
            SDL_RenderCopy(renderer,
                highlight->texture,
                NULL,
                &highlight->rect
            );

        // Draw buttons
        int slide = config.tv_layout ?
            draw_tv_row(renderer, current_menu->root_entry, current_entry, geo.num_buttons) : 0;
        Entry *entry = current_menu->root_entry;
        SDL_Texture *icon;
        for (int i = 0; i < geo.num_buttons; i++) {
            icon = (entry->icon_selected != NULL && i == (int) current_menu->highlight_position) ? entry->icon_selected : entry->icon;
            SDL_Rect icon_rect = navigation_icon_rect(entry);
            icon_rect.x += slide;
            if (config.tv_layout)
                icon_rect = fit_tv_texture(icon, icon_rect);
            if (entry == settings_entry) {
                if (config.tv_layout) {
                    settings_card_bounds = entry->card_rect;
                    settings_card_bounds.x += slide;
                    SDL_Rect clip;
                    SDL_RenderGetClipRect(renderer, &clip);
                    SDL_IntersectRect(&settings_card_bounds, &clip, &settings_card_bounds);
                }
                else if (config.titles_enabled)
                    SDL_UnionRect(&icon_rect, &entry->text_rect, &settings_card_bounds);
                else
                    settings_card_bounds = icon_rect;
            }
            if (icon != NULL)
                SDL_RenderCopy(renderer, icon, NULL, &icon_rect);
            if (config.titles_enabled && entry->title_texture != NULL) {
                SDL_Rect title = entry->text_rect;
                title.x += slide;
                SDL_RenderCopy(renderer, entry->title_texture, NULL, &title);
            }
            entry = entry-> next;
        }

        if (config.tv_layout)
            SDL_RenderSetClipRect(renderer, NULL);

        // Draw screensaver
        if (state.screensaver_active)
            SDL_RenderCopy(renderer, screensaver->texture, NULL, NULL);
    }
    else
        SDL_RenderFillRect(renderer, NULL);

    // Output to screen
    SDL_RenderPresent(renderer);
    if (!config.vsync) {
        Uint32 elapsed = SDL_GetTicks() - ticks.main;
        if (elapsed < refresh_period)
            SDL_Delay(refresh_period - elapsed);
    }
}

// A function to execute the user's command
static void execute_command(const char *command)
{
    // Copy command into separate buffer
    char *cmd = strdup(command);

    // Parse special commands
    if (cmd == NULL) {
        log_error("Could not copy the launch command");
        return;
    }
    if (cmd[0] == ':') {
        char *delimiter = " ";
        char *special_command = strtok(cmd, delimiter);
        if (!strcmp(special_command, SCMD_SUBMENU)) {
            char *submenu = strtok(NULL, "");
            if (submenu != NULL)
                load_submenu(submenu);
        }
        else if (!strcmp(special_command, SCMD_FORK)) {
            char *fork_command = strtok(NULL, "");
            if (fork_command != NULL)
                start_process(fork_command, false);
        }
        else if (!strcmp(special_command, SCMD_LEFT))
            move_left();
        else if (!strcmp(special_command, SCMD_RIGHT))
            move_right();
        else if (!strcmp(special_command, SCMD_SELECT))
            execute_command(current_entry->cmd);
        else if (!strcmp(special_command, SCMD_SETTINGS)) {
            if (current_menu == default_menu && !state.application_launching && !state.application_running &&
                settings_open()) {
                navigation_entry = NULL;
                reset_tv_motion();
                request_launcher_redraw();
                log_debug("Opened launcher settings");
            }
        }
        else if (!strcmp(special_command, SCMD_HOME))
            load_menu(default_menu, false, true);
        else if (!strcmp(special_command, SCMD_BACK))
            load_back_menu(current_menu);
        else if (!strcmp(special_command, SCMD_QUIT))
            quit(EXIT_SUCCESS);
        else if (!strcmp(special_command, SCMD_SHUTDOWN))
            scmd_shutdown();
        else if (!strcmp(special_command, SCMD_RESTART))
            scmd_restart();
        else if (!strcmp(special_command, SCMD_SLEEP))
            scmd_sleep();
    }

    // Launch external application
    else {
        if (start_process(cmd, true)) {
            state.application_launching = true;
            redraw_requested = true;
            ticks.application_launched = SDL_GetTicks();
            log_debug("Waiting for focus handoff (timeout %u ms)",
                (unsigned)config.application_timeout);
            if (config.on_launch == ON_LAUNCH_BLANK)
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
            else if (config.on_launch == ON_LAUNCH_QUIT)
                quit(EXIT_SUCCESS);
        }
    }
    free(cmd);
}

// A function to initialize the gamepad struct
static void init_gamepad(Gamepad **gamepad, int device_index)
{
    *gamepad = malloc(sizeof(Gamepad));
    **gamepad = (Gamepad) {
        .device_index = device_index,
        .id = (int) SDL_JoystickGetDeviceInstanceID(device_index),
        .controller = NULL,
        .next = NULL,
        .previous = NULL,
    };
}

// A function to open the SDL controller
static void open_controller(Gamepad *gamepad, bool raise_error)
{
    if (gamepad->controller != NULL)
        return;
    gamepad->controller = SDL_GameControllerOpen(gamepad->device_index);
    if (gamepad->controller == NULL) {
        if (raise_error)
            log_error("Could not open gamepad at device index %i: %s", gamepad->device_index, SDL_GetError());
        else
            log_debug("Could not reopen gamepad at device index %i: %s", gamepad->device_index, SDL_GetError());
        return;
    }
    log_debug("Opened gamepad at device index %i (instance %i)", gamepad->device_index, gamepad->id);
    if (config.debug && raise_error) {
        char *mapping = SDL_GameControllerMapping(gamepad->controller);
        log_debug("Gamepad Mapping:\n%s", mapping);
        SDL_free(mapping);
    }
}

// A function to connect gamepad(s)
static void connect_gamepad(int device_index, bool open, bool raise_error)
{
    if (device_index >= 0) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
        if (id < 0)
            return;
        Gamepad *gamepad = NULL;
        for (gamepad = gamepads; gamepad != NULL; gamepad = gamepad->next) {
            if (gamepad->id == id)
                break;
        }
        if (gamepad == NULL) {
            init_gamepad(&gamepad, device_index);
            if (gamepads == NULL)
                gamepads = gamepad;

            // Add to end of the linked list
            else {
                Gamepad *i;
                for (i = gamepads; i->next != NULL; i = i->next);
                i->next = gamepad;
                gamepad->previous = i;
            }
        }
        if (open)
            open_controller(gamepad, raise_error);
    }
    else if (open) {
        int count = SDL_NumJoysticks();
        for (int index = 0; index < count; index++) {
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
            for (Gamepad *i = gamepads; i != NULL; i = i->next) {
                if (i->id == id) {
                    i->device_index = index;
                    open_controller(i, raise_error);
                    break;
                }
            }
        }
    }
}

// A function to disconnect gamepad(s)
static void disconnect_gamepad(int id, bool disconnect, bool remove)
{
    for (Gamepad *i = gamepads; i != NULL;) {
        if (id < 0 || i->id == id) {
            if (disconnect && i->controller != NULL)
                SDL_GameControllerClose(i->controller);
            if (remove) {
                if (i->next != NULL)
                    i->next->previous = i->previous;
                if (i->previous != NULL)
                    i->previous->next = i->next;
                if (i == gamepads)
                    gamepads = i->next;
                Gamepad *tmp = i->next;
                free(i);
                i = tmp;
            }
            else {
                i->controller = NULL;
                i = i->next;
            }
        }
        else
            i = i->next;
    }
}

// A function to poll the connected gamepad for commands
static void poll_gamepad()
{
    int value_multiplier; // Handles positive or negative axis
    bool pressed;
    for (GamepadControl *i = gamepad_controls; i != NULL; i = i->next) {
        pressed = false;
        for (Gamepad *gamepad = gamepads; gamepad != NULL; gamepad = gamepad->next) {

            if (gamepad->controller == NULL)
                continue;

            // Check if axis value exceeds dead zone
            if (i->type == TYPE_AXIS_POS || i->type == TYPE_AXIS_NEG) {
                if (i->type == TYPE_AXIS_POS)
                    value_multiplier = 1;
                else if (i->type == TYPE_AXIS_NEG)
                    value_multiplier = -1;
                if (value_multiplier*SDL_GameControllerGetAxis(gamepad->controller, i->index) > GAMEPAD_DEADZONE) {
                    pressed = true;
                    break;
                }
            }

            // Check buttons
            else if (i->type == TYPE_BUTTON) {
                if (SDL_GameControllerGetButton(gamepad->controller, i->index)) {
                    pressed = true;
                    break;
                }
            }
        }
        if (!pressed) {
            i->held = false;
            i->settings_latched = false;
            continue;
        }

        if (i->settings_latched)
            continue;
        if (!i->held || SDL_TICKS_PASSED(ticks.main, i->repeat_at)) {
            if (!i->held)
                log_debug("Gamepad %s detected", i->label);
            bool editing = settings_is_open();
            i->repeat_at = ticks.main + (i->held ?
                (editing ? 110 : GAMEPAD_REPEAT_INTERVAL) : GAMEPAD_REPEAT_DELAY);
            i->held = true;
            ticks.last_input = ticks.main;
            if (editing) {
                if (!strcmp(i->cmd, SCMD_SELECT) || !strcmp(i->cmd, SCMD_BACK) ||
                    !strcmp(i->cmd, SCMD_HOME) || !strcmp(i->cmd, SCMD_QUIT))
                    i->settings_latched = true;
                settings_command(i->cmd);
            }
            else {
                execute_command(i->cmd);
                if (settings_is_open())
                    i->settings_latched = true;
            }
        }
    }
}

static void settings_controller_button(Uint8 button)
{
    if (!settings_is_open() ||
        (button != SDL_CONTROLLER_BUTTON_DPAD_UP && button != SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        return;

    // The default mappings leave vertical D-pad directions unassigned.
    for (const GamepadControl *control = gamepad_controls; control != NULL; control = control->next) {
        if (control->type == TYPE_BUTTON && control->index == button)
            return;
    }
    ticks.last_input = ticks.main;
    settings_command(button == SDL_CONTROLLER_BUTTON_DPAD_UP ? SCMD_UP : SCMD_DOWN);
}

// A function to update the screensaver
static void update_screensaver()
{
    // Activate the screensaver if the launcher has been idle for the required time
    if (!state.screensaver_active && ticks.main - ticks.last_input > config.screensaver_idle_time) {
        state.screensaver_active = true;
        state.screensaver_transition = true;
        if (background_rotates() && config.screensaver_pause_slideshow)
            state.slideshow_paused = true;
    }
    else {

        // Transition the screen to dark
        if (state.screensaver_transition) {
            redraw_requested = true;
            screensaver->alpha += screensaver->transition_change_rate;
            if (screensaver->alpha >= screensaver->alpha_end_value) {
                SDL_SetTextureAlphaMod(screensaver->texture, (Uint8) screensaver->alpha_end_value);
                state.screensaver_transition = false;
            }
            else
                SDL_SetTextureAlphaMod(screensaver->texture, (Uint8) screensaver->alpha);
        }

        // User has pressed input, deactivate the screensaver
        if (state.screensaver_active && ticks.last_input == ticks.main) {
            SDL_SetTextureAlphaMod(screensaver->texture, 0);
            screensaver->alpha = 0.0f;
            state.screensaver_active = false;
            state.screensaver_transition = false;
            redraw_requested = true;
            state.slideshow_paused = false;

            // Give the visible background a full interval after waking.
            if (background_rotates())
                restart_background_interval(ticks.main);
        }
    }
}

// A function to update the clock display
static bool update_clock(void)
{
    if (ticks.main - ticks.clock_update <= CLOCK_UPDATE_PERIOD)
        return false;
    ticks.clock_update = ticks.main;
    get_time(clk);
    if (!clk->render_time)
        return false;

    render_clock(clk);
    SDL_Texture *time = load_texture(clk->time_surface);
    clk->time_surface = NULL;
    if (time != NULL) {
        SDL_DestroyTexture(clk->time_texture);
        clk->time_texture = time;
    }
    if (clk->render_date) {
        SDL_Texture *date = load_texture(clk->date_surface);
        clk->date_surface = NULL;
        if (date != NULL) {
            SDL_DestroyTexture(clk->date_texture);
            clk->date_texture = date;
        }
    }
    clk->render_time = false;
    clk->render_date = false;
    return true;
}

static inline void pre_launch()
{
    if (gamepads != NULL) {
        log_debug("Releasing gamepads during application handoff");
        disconnect_gamepad(-1, true, false);
        for (GamepadControl *control = gamepad_controls; control != NULL; control = control->next)
            control->held = false;
    }

// Initialize exit hotkey for Windows
#ifdef _WIN32
    if (has_exit_hotkey())
        SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
}

static inline void post_launch()
{
    // Rebaseline the timing after the program is done
    ticks.main = SDL_GetTicks();
    ticks.last_input = ticks.main;

    // Post-application updates
    redraw_requested = true;
    if (config.gamepad_enabled)
        connect_gamepad(-1, true, false);
    if (config.clock_enabled)
        update_clock();
    if (background_rotates())
        restart_background_interval(ticks.main);
    if (config.on_launch == ON_LAUNCH_BLANK)
        set_draw_color();

#ifdef _WIN32
    SDL_EventState(SDL_SYSWMEVENT, SDL_DISABLE);
    if (config.background_mode == BACKGROUND_TRANSPARENT)
        hide_cursor(current_entry);
#endif
}

// A function to quit the launcher
void quit(int status)
{
    log_debug("Quitting program");
    if (status != EXIT_SUCCESS)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, 
            PROJECT_NAME, 
            "A critical error occurred. Check the log file for details.", 
            NULL
        );
    if (config.quit_cmd != NULL) {
        char *command = config.quit_cmd;
        config.quit_cmd = NULL;
        execute_command(command);
        free(command);
    }
    cleanup();
    exit(status);
}

// A function to print the version and other info to command line
void print_version(FILE *stream)
{
    SDL_version sdl_version;
    SDL_GetVersion(&sdl_version);
    const SDL_version *img_version = IMG_Linked_Version();
    const SDL_version *ttf_version = TTF_Linked_Version();
    fprintf(stream, PROJECT_NAME " version " PROJECT_VERSION ", using:" endline);
    fprintf(stream, "  SDL       %u.%u.%u" endline, sdl_version.major, sdl_version.minor, sdl_version.patch);
    fprintf(stream, "  SDL_image %u.%u.%u" endline, img_version->major, img_version->minor, img_version->patch);
    fprintf(stream, "  SDL_ttf   %u.%u.%u" endline, ttf_version->major, ttf_version->minor, ttf_version->patch);
}

int main(int argc, char *argv[]) 
{
    int error;
    config.exe_path = SDL_GetBasePath();

    // Handle command line arguments, find config file
    handle_arguments(argc, argv, &config_file_path);

    // Parse config file for settings and menu entries
    parse_config_file(config_file_path);

    // Get default menu
    if (config.default_menu == NULL)
        log_fatal("No default menu defined in config file");
    default_menu = get_menu(config.default_menu);
    if (default_menu == NULL)
        log_fatal("Default menu %s not found in config file", config.default_menu);
    add_settings_card(default_menu);

    // Initialize SDL, verify all settings are in their allowable range
    init_sdl();
    init_navigation_audio();
    init_sdl_image();
    init_sdl_ttf();
    validate_settings(&geo);
    if (config.tv_layout)
        size_tv_row(&geo);
    
    // Initialize Nanosvg, create window and renderer
    init_svg();
    create_window();
    if (config.tv_layout && !init_tv_ui())
        log_fatal("Could not create TV layout textures: %s", SDL_GetError());
    init_weather(title_info.font);

    // Initialize timing
    ticks.main = SDL_GetTicks();
    ticks.last_input = ticks.main;
    ticks.program_start = ticks.main;

    // Load gamepad overrides
    if (config.gamepad_enabled && config.gamepad_mappings_file != NULL) {
        error = SDL_GameControllerAddMappingsFromFile(config.gamepad_mappings_file);
        if (error < 0) {
            log_error("Could not load gamepad mappings from %s\n%s", 
                config.gamepad_mappings_file,
                SDL_GetError()
            );
        }
    }

    init_background(renderer);

    // Initialize screensaver
    if (config.screensaver_enabled)
        init_screensaver();

    // Initialize clock
    if (config.clock_enabled) {
        clk = calloc(1, sizeof(Clock));
        if (clk == NULL)
            log_fatal("Could not allocate clock state");
        init_clock(clk);
        ticks.clock_update = ticks.main;
    }
    
    // Render highlight
    if (config.highlight && !config.tv_layout) {
        int button_height = config.icon_size + config.title_padding + geo.font_height;
        highlight = malloc(sizeof(Highlight));
        highlight->texture = render_highlight(config.icon_size + 2*config.highlight_hpadding,
                                button_height + 2*config.highlight_vpadding,
                                &highlight->rect
                            );
    }

    // Render scroll indicators
    if (config.scroll_indicators) {
        scroll = malloc(sizeof(Scroll));
        scroll->texture = NULL;
        int scroll_indicator_height = (int) ((float) geo.screen_height * SCROLL_INDICATOR_HEIGHT);
        render_scroll_indicators(scroll, scroll_indicator_height, &geo);
    }

    // Render background overlay
    if (config.background_overlay) {
        SDL_Surface *overlay_surface = NULL;
        overlay_surface = SDL_CreateRGBSurfaceWithFormat(0, 
                              geo.screen_width, 
                              geo.screen_height, 
                              32,
                              SDL_PIXELFORMAT_ARGB8888
                          );
        Uint32 overlay_color = SDL_MapRGBA(overlay_surface->format, 
                                   config.background_overlay_color.r, 
                                   config.background_overlay_color.g, 
                                   config.background_overlay_color.b, 
                                   config.background_overlay_color.a
                               );
        SDL_FillRect(overlay_surface, NULL, overlay_color);
        background_overlay = load_texture(overlay_surface);
    }

    // Register exit hotkey with Windows
#ifdef _WIN32
    if (has_exit_hotkey())
        register_exit_hotkey();
#endif

    // Print debug info to log
    if (config.debug) {
        debug_video(renderer, &display_mode);
        debug_settings();
        debug_gamepad(gamepad_controls);
        debug_hotkeys(hotkeys);    
        debug_menu_entries(config.first_menu, config.num_menus);
    }

    // Load the default menu and display it
    error = load_menu(default_menu, false, true);
    if (error)
        log_fatal("Could not load default menu %s", config.default_menu);

    // Execute startup command
    if (config.startup_cmd != NULL)
        execute_command(config.startup_cmd);
    
    // Main program loop
    log_debug("Begin program loop");
    while (1) {
        ticks.main = SDL_GetTicks();
        while (SDL_PollEvent(&event)) {
            switch(event.type) {
                case SDL_QUIT:
                    quit(EXIT_SUCCESS);
                    break;

                case SDL_KEYDOWN: {
                    SDL_Scancode scancode = event.key.keysym.scancode;
                    if (event.key.repeat && settings_key_latched[scancode])
                        break;
                    ticks.last_input = ticks.main;
                    bool editing = settings_is_open();
                    SDL_Keycode key = event.key.keysym.sym;
                    handle_keypress(&event.key.keysym);
                    settings_key_latched[scancode] = editing != settings_is_open() ||
                        (editing && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
                            key == SDLK_ESCAPE || key == SDLK_BACKSPACE));
                    break;
                }

                case SDL_KEYUP:
                    settings_key_latched[event.key.keysym.scancode] = false;
                    break;
                
                case SDL_MOUSEMOTION:
                    if (settings_is_open()) {
                        ticks.last_input = ticks.main;
                        settings_mouse(event.motion.x, event.motion.y, false);
                    }
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        ticks.last_input = ticks.main;
                        SDL_Point point = {event.button.x, event.button.y};
                        if (settings_is_open())
                            settings_mouse(point.x, point.y, true);
                        else if (SDL_PointInRect(&point, &settings_card_bounds))
                            execute_command(SCMD_SETTINGS);
                        else if (config.mouse_select)
                            execute_command(current_entry->cmd);
                    }
                    break;

                case SDL_CONTROLLERBUTTONDOWN:
                    settings_controller_button(event.cbutton.button);
                    break;

                case SDL_JOYDEVICEADDED:
                    if (SDL_IsGameController(event.jdevice.which) == SDL_TRUE) {
                        log_debug("Gamepad connected with device index %i", event.jdevice.which);
                        if (config.gamepad_device < 0 || config.gamepad_device == event.jdevice.which)
                            connect_gamepad(event.jdevice.which, !state.application_running, true);
                    }
                    break;

                case SDL_JOYDEVICEREMOVED:
                    log_debug("Joystick disconnected (instance %i)", event.jdevice.which);
                    disconnect_gamepad(event.jdevice.which, true, true);
                    break;

                case SDL_RENDER_TARGETS_RESET:
                case SDL_RENDER_DEVICE_RESET:
                    redraw_requested = true;
                    log_debug("SDL renderer reset (event %u)", (unsigned)event.type);
                    break;

                case SDL_WINDOWEVENT:
                    redraw_requested = true;
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        log_debug("Lost keyboard focus (launching=%i, running=%i)",
                            state.application_launching, state.application_running);
#ifdef _WIN32
                        debug_foreground_window();
#endif
                        state.has_focus = false;
                        if (state.application_launching) {
                            log_debug("Focus handoff observed %u ms after launch",
                                (unsigned)(SDL_GetTicks() - ticks.application_launched));
                            state.application_launching = false;
                            state.application_running = true;
                            pre_launch();
                        }
#ifdef _WIN32
                        // Sometimes the launcher will lose focus on Windows when autostarting
                        // So if we lose the window focus within 10 seconds of the launcher starting
                        // we will grab back th Window focus. This is a bit of a hack
                        else if (ticks.main - ticks.program_start < 10000)
                            set_foreground_window();
#endif
                    }
                    else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        log_debug("Gained keyboard focus (launching=%i, running=%i)",
                            state.application_launching, state.application_running);
#ifdef _WIN32
                        debug_foreground_window();
#endif
                        state.has_focus = true;
                    }
                    else if (event.window.event == SDL_WINDOWEVENT_LEAVE)
                        log_debug("Lost mouse focus");
                    break;
#ifdef _WIN32
                case SDL_SYSWMEVENT:
                    check_exit_hotkey(event.syswm.msg);
                    break;
#endif
            }
        }

        // Update application state
        if (state.application_running && state.has_focus) {
            state.application_running = false;
            post_launch();
            log_debug("Launcher restored after regaining focus");
        }

        finish_settings_save();

        // Post-event loop updates
        if (!(state.application_running || state.application_launching)) {
            if (gamepads != NULL)
                poll_gamepad();
            bool background_changed = update_background(renderer, ticks.main, state.slideshow_paused);
            redraw_requested |= background_changed && !settings_is_open();
            if (config.screensaver_enabled)
                update_screensaver();
            if (config.clock_enabled)
                redraw_requested |= update_clock();
            if (current_menu == default_menu)
                redraw_requested |= update_weather(ticks.main);
        }
        if (state.application_launching &&
        SDL_GetTicks() - ticks.application_launched >= config.application_timeout) {
            log_debug("Launch wait expired without a focus handoff");
#ifdef _WIN32
            debug_foreground_window();
#endif
            state.application_launching = false;
            redraw_requested = true;
            if (config.on_launch == ON_LAUNCH_BLANK)
                set_draw_color();
        }
        if (state.application_running)
            SDL_Delay(APPLICATION_WAIT_PERIOD);
        else if (!config.tv_layout || redraw_requested || tv_motion_active() || navigation_entry != NULL) {
            redraw_requested = false;
            draw_screen();
        }
        else
            SDL_Delay(8);
    }
    quit(EXIT_SUCCESS);
}
