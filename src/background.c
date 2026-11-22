#include <stdbool.h>
#include <stdlib.h>
#include <SDL.h>
#include <SDL_image.h>
#include "launcher.h"
#include "background.h"
#include "debug.h"
#include "platform/platform.h"
#include "util.h"

#define MAX_PHOTO_LONG_EDGE 3840
#define MAX_PHOTO_SHORT_EDGE 2160
#define MAX_COLLAGE_PANELS 4

extern Config config;

static struct {
    PhotoList files;
    int *order;
    int current_index;
    int remaining_photos;
    int collage_cursor;
    int last_collage[MAX_COLLAGE_PANELS];
    int last_collage_size;
    int auto_panels;
    int width;
    int height;
    SDL_Thread *worker;
    SDL_atomic_t stop_requested;
    SDL_atomic_t ready;
    SDL_Surface *prepared;
    SDL_Texture *current;
    SDL_Texture *incoming;
    Uint32 shown_at;
    Uint32 fade_started_at;
    bool finished;
} background = { .current_index = -1, .auto_panels = 3 };

bool background_rotates(void)
{
    return config.background_mode == BACKGROUND_SLIDESHOW ||
        config.background_mode == BACKGROUND_COLLAGE;
}

static const char *background_path(void)
{
    return background_rotates() ? config.slideshow_directory : config.background_image;
}

// Calculate the source crop or destination inset using the actual display aspect.
static void photo_rects(SDL_Rect *source, SDL_Rect *destination, PhotoFit fit)
{
    Sint64 source_ratio = (Sint64)source->w * destination->h;
    Sint64 destination_ratio = (Sint64)destination->w * source->h;

    if (fit == PHOTO_CONTAIN) {
        if (source_ratio > destination_ratio) {
            int height = SDL_max(1, (int)(destination_ratio / source->w));
            destination->y += (destination->h - height) / 2;
            destination->h = height;
        }
        else {
            int width = SDL_max(1, (int)(source_ratio / source->h));
            destination->x += (destination->w - width) / 2;
            destination->w = width;
        }
    }
    else if (fit == PHOTO_COVER) {
        if (source_ratio > destination_ratio) {
            int width = SDL_max(1, (int)(destination_ratio / destination->h));
            source->x = (int)((Sint64)(source->w - width) * config.photo_crop_x / 100);
            source->w = width;
        }
        else {
            int height = SDL_max(1, (int)(source_ratio / destination->w));
            source->y = (int)((Sint64)(source->h - height) * config.photo_crop_y / 100);
            source->h = height;
        }
    }
}

// One opaque frame includes the photos, any matte, and the gutters, so all
// parts of the outgoing and incoming compositions crossfade together.
static SDL_Surface *new_photo_frame(void)
{
    SDL_Surface *frame = SDL_CreateRGBSurfaceWithFormat(0, background.width,
        background.height, 32, SDL_PIXELFORMAT_RGB888);
    if (frame != NULL) {
        SDL_Color fill = config.background_color;
        if (SDL_FillRect(frame, NULL, SDL_MapRGB(frame->format, fill.r, fill.g, fill.b)) < 0) {
            SDL_FreeSurface(frame);
            return NULL;
        }
    }
    return frame;
}

static bool blit_photo(SDL_Surface *photo, SDL_Surface *frame, SDL_Rect panel, PhotoFit fit)
{
    SDL_Rect source = { 0, 0, photo->w, photo->h };
    photo_rects(&source, &panel, fit);
    return SDL_BlitScaled(photo, &source, frame, &panel) == 0;
}

static SDL_Surface *prepare_photo(const char *path)
{
    SDL_Surface *photo = IMG_Load(path);
    if (photo == NULL)
        return NULL;

    SDL_Surface *frame = new_photo_frame();
    if (frame != NULL) {
        SDL_Rect panel = { 0, 0, frame->w, frame->h };
        if (!blit_photo(photo, frame, panel, config.photo_fit)) {
            SDL_FreeSurface(frame);
            frame = NULL;
        }
    }
    SDL_FreeSurface(photo);
    return frame;
}

static bool init_photo_catalog(void)
{
    if (background.order != NULL)
        return true;
    if (!scan_slideshow_directory(&background.files, config.slideshow_directory,
            &background.stop_requested) || background.files.count == 0)
        return false;

    background.order = malloc((size_t)background.files.count * sizeof(*background.order));
    if (background.order == NULL)
        return false;
    background.remaining_photos = background.files.count;
    random_array(background.order, background.files.count);
    return true;
}

static void skip_photo(int index)
{
    background.order[index] = -1;
    background.remaining_photos--;
}

static SDL_Surface *next_slideshow_photo(void)
{
    int index = background.current_index;
    for (int attempt = 0; attempt < background.files.count; attempt++) {
        if (SDL_AtomicGet(&background.stop_requested))
            break;
        index = (index + 1) % background.files.count;
        if (index == background.current_index)
            break;
        int file = background.order[index];
        if (file < 0)
            continue;
        SDL_Surface *frame = prepare_photo(background.files.paths[file]);
        if (frame != NULL) {
            background.current_index = index;
            return frame;
        }
        // Skip this file for the rest of this run.
        skip_photo(index);
    }
    return NULL;
}

static int fixed_panel_count(void)
{
    switch (config.collage_layout) {
        case COLLAGE_SPLIT:   return 2;
        case COLLAGE_FEATURE: return 3;
        case COLLAGE_GRID:    return 4;
        default:              return 0;
    }
}

static bool have_another_background(void)
{
    if (config.background_mode == BACKGROUND_SLIDESHOW)
        return background.remaining_photos > 1;
    if (config.background_mode == BACKGROUND_COLLAGE) {
        int count = fixed_panel_count();
        return background.remaining_photos > (count == 0 ? 2 : count);
    }
    return false;
}

static void collage_panels(SDL_Rect panels[], int count)
{
    int width = background.width;
    int height = background.height;
    int gap = SDL_max(1, SDL_min(width, height) / 180);
    int left = (width - gap) / 2;
    int top = (height - gap) / 2;
    int right_x = left + gap;
    int bottom_y = top + gap;

    if (count == 1) {
        panels[0] = (SDL_Rect){ 0, 0, width, height };
    }
    else if (count == 2 && width >= height) {
        panels[0] = (SDL_Rect){ 0, 0, left, height };
        panels[1] = (SDL_Rect){ right_x, 0, width - right_x, height };
    }
    else if (count == 2) {
        panels[0] = (SDL_Rect){ 0, 0, width, top };
        panels[1] = (SDL_Rect){ 0, bottom_y, width, height - bottom_y };
    }
    else if (count == 3 && width >= height) {
        left = (width - gap) * 3 / 5;
        right_x = left + gap;
        panels[0] = (SDL_Rect){ 0, 0, left, height };
        panels[1] = (SDL_Rect){ right_x, 0, width - right_x, top };
        panels[2] = (SDL_Rect){ right_x, bottom_y, width - right_x, height - bottom_y };
    }
    else if (count == 3) {
        top = (height - gap) * 3 / 5;
        bottom_y = top + gap;
        panels[0] = (SDL_Rect){ 0, 0, width, top };
        panels[1] = (SDL_Rect){ 0, bottom_y, left, height - bottom_y };
        panels[2] = (SDL_Rect){ right_x, bottom_y, width - right_x, height - bottom_y };
    }
    else {
        panels[0] = (SDL_Rect){ 0, 0, left, top };
        panels[1] = (SDL_Rect){ right_x, 0, width - right_x, top };
        panels[2] = (SDL_Rect){ 0, bottom_y, left, height - bottom_y };
        panels[3] = (SDL_Rect){ right_x, bottom_y, width - right_x, height - bottom_y };
    }
}

static bool includes_photo(const int selected[], int size, int index)
{
    for (int i = 0; i < size; i++) {
        if (selected[i] == index)
            return true;
    }
    return false;
}

// Prefer the free panel that retains the most of the current photo under Cover.
static int best_panel(const SDL_Surface *photo, const SDL_Rect panels[], int count, unsigned int occupied)
{
    double aspect = (double)photo->w / photo->h;
    double best_score = -1.0;
    int best = 0;
    for (int i = 0; i < count; i++) {
        if (occupied & (1u << i))
            continue;
        double panel_aspect = (double)panels[i].w / panels[i].h;
        double score = SDL_min(aspect / panel_aspect, panel_aspect / aspect);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static int fill_collage(SDL_Surface *frame, int count, int selected[])
{
    SDL_Rect panels[MAX_COLLAGE_PANELS];
    collage_panels(panels, count);
    int filled = 0;
    unsigned int occupied = 0;

    // Only fall back to photos from the preceding collage if there are too few
    // other usable photos. Each pass visits at most one cycle of the catalog.
    for (int pass = 0; pass < 2 && filled < count; pass++) {
        for (int inspected = 0; inspected < background.files.count && filled < count; inspected++) {
            if (SDL_AtomicGet(&background.stop_requested))
                return filled;
            int index = background.collage_cursor;
            background.collage_cursor = (index + 1) % background.files.count;
            int file = background.order[index];
            if (file < 0 || includes_photo(selected, filled, index) ||
                (pass == 0 && includes_photo(background.last_collage, background.last_collage_size, index)))
                continue;

            SDL_Surface *photo = IMG_Load(background.files.paths[file]);
            if (photo == NULL) {
                skip_photo(index);
                continue;
            }
            int panel = best_panel(photo, panels, count, occupied);
            bool drawn = blit_photo(photo, frame, panels[panel], count == 1 ? PHOTO_CONTAIN : PHOTO_COVER);
            SDL_FreeSurface(photo);
            if (drawn) {
                occupied |= 1u << panel;
                selected[filled++] = index;
            }
            else
                skip_photo(index);
        }
    }
    return filled;
}

static SDL_Surface *next_collage(void)
{
    int panels = fixed_panel_count();
    if (panels == 0)
        panels = background.auto_panels;
    panels = SDL_min(panels, background.remaining_photos);
    if (background.width < 4 || background.height < 4)
        panels = SDL_min(panels, 1);

    while (panels > 0 && !SDL_AtomicGet(&background.stop_requested)) {
        SDL_Surface *frame = new_photo_frame();
        if (frame == NULL)
            return NULL;

        int selected[MAX_COLLAGE_PANELS];
        int filled = fill_collage(frame, panels, selected);
        if (filled == panels) {
            // An exhausted folder can contain exactly the same usable set as the
            // preceding frame. Keep it on screen without performing another fade.
            bool changed = panels != background.last_collage_size;
            for (int i = 0; i < filled && !changed; i++)
                changed = !includes_photo(background.last_collage, background.last_collage_size, selected[i]);
            if (!changed) {
                SDL_FreeSurface(frame);
                return NULL;
            }

            SDL_memcpy(background.last_collage, selected, (size_t)filled * sizeof(*selected));
            background.last_collage_size = filled;
            int max_panels = SDL_min(MAX_COLLAGE_PANELS, background.remaining_photos);
            background.auto_panels = filled < max_panels ? filled + 1 : 2;
            return frame;
        }

        SDL_FreeSurface(frame);
        panels = filled;
    }
    return NULL;
}

static int prepare_background(void *unused)
{
    (void)unused;
    if (config.background_mode == BACKGROUND_IMAGE)
        background.prepared = prepare_photo(config.background_image);
    else if (init_photo_catalog()) {
        background.prepared = config.background_mode == BACKGROUND_COLLAGE ?
            next_collage() : next_slideshow_photo();
    }
    SDL_AtomicSet(&background.ready, 1);
    return 0;
}

static bool start_background_worker(void)
{
    SDL_AtomicSet(&background.ready, 0);
    background.worker = SDL_CreateThread(prepare_background, "Background preparation", NULL);
    if (background.worker == NULL)
        log_error("Could not start background worker: %s", SDL_GetError());
    return background.worker != NULL;
}

void init_background(SDL_Renderer *renderer)
{
    if (!background_rotates() && config.background_mode != BACKGROUND_IMAGE)
        return;
    const char *path = background_path();
    if (path == NULL || path[0] == '\0') {
        log_error("No path configured for the photo background; using the background color");
        background.finished = true;
        return;
    }

    int width, height;
    SDL_RendererInfo info;
    if (SDL_GetRendererOutputSize(renderer, &width, &height) < 0 ||
        width <= 0 || height <= 0 || SDL_GetRendererInfo(renderer, &info) < 0) {
        log_error("Could not size the photo background: %s", SDL_GetError());
        background.finished = true;
        return;
    }
    int max_width = width >= height ? MAX_PHOTO_LONG_EDGE : MAX_PHOTO_SHORT_EDGE;
    int max_height = width >= height ? MAX_PHOTO_SHORT_EDGE : MAX_PHOTO_LONG_EDGE;
    if (info.max_texture_width > 0)
        max_width = SDL_min(max_width, info.max_texture_width);
    if (info.max_texture_height > 0)
        max_height = SDL_min(max_height, info.max_texture_height);
    double scale = SDL_min(1.0, SDL_min((double)max_width / width, (double)max_height / height));
    background.width = SDL_max(1, (int)(width * scale + 0.5));
    background.height = SDL_max(1, (int)(height * scale + 0.5));
    log_debug("Photo background canvas: %i x %i", background.width, background.height);

    background.finished = !start_background_worker();
}

void update_background(SDL_Renderer *renderer, Uint32 now, bool paused)
{
    if (background.finished || paused)
        return;

    if (background.incoming != NULL) {
        Uint32 elapsed = now - background.fade_started_at;
        if (elapsed < config.slideshow_transition_time) {
            Uint8 opacity = (Uint8)(255u * elapsed / config.slideshow_transition_time);
            SDL_SetTextureAlphaMod(background.incoming, opacity);
            return;
        }

        SDL_SetTextureAlphaMod(background.incoming, 255);
        SDL_DestroyTexture(background.current);
        background.current = background.incoming;
        background.incoming = NULL;
        background.shown_at = now;
        background.finished = !have_another_background();
        return;
    }

    bool due = background.current == NULL || now - background.shown_at >= config.slideshow_image_duration;
    if (background.worker == NULL) {
        if (!start_background_worker())
            background.finished = true;
        return;
    }
    if (!due || !SDL_AtomicGet(&background.ready))
        return;

    SDL_WaitThread(background.worker, NULL);
    background.worker = NULL;
    SDL_Texture *frame = background.prepared != NULL ?
        SDL_CreateTextureFromSurface(renderer, background.prepared) : NULL;
    SDL_FreeSurface(background.prepared);
    background.prepared = NULL;

    if (frame == NULL) {
        const char *path = background_path();
        if (background.current == NULL) {
            log_error("Could not prepare a photo background from '%s'; using the background color", path);
            background.finished = true;
        }
        else {
            if (config.background_mode == BACKGROUND_COLLAGE)
                log_debug("Collage kept the current scene: no replacement available");
            else
                log_debug("Slideshow kept the current image: no replacement texture available");
            background.shown_at = now;
            background.finished = !have_another_background();
        }
        return;
    }

    if (background.current != NULL && config.slideshow_transition_time > 0) {
        SDL_SetTextureBlendMode(frame, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(frame, 0);
        background.incoming = frame;
        background.fade_started_at = SDL_GetTicks();
        if (config.background_mode == BACKGROUND_COLLAGE)
            log_debug("Starting collage crossfade to %i panels (%u ms)",
                background.last_collage_size, (unsigned)config.slideshow_transition_time);
        else
            log_debug("Starting slideshow crossfade (%u ms)", (unsigned)config.slideshow_transition_time);
    }
    else {
        SDL_DestroyTexture(background.current);
        background.current = frame;
        background.shown_at = SDL_GetTicks();
        background.finished = !have_another_background();
        if (config.background_mode == BACKGROUND_COLLAGE)
            log_debug("Displayed a %i-panel collage from a catalog of %i files",
                background.last_collage_size, background.files.count);
        else if (config.background_mode == BACKGROUND_SLIDESHOW)
            log_debug("Displayed a slideshow photo from a catalog of %i files", background.files.count);
    }
}

void draw_background(SDL_Renderer *renderer)
{
    if (background.current != NULL)
        SDL_RenderCopy(renderer, background.current, NULL, NULL);
    if (background.incoming != NULL)
        SDL_RenderCopy(renderer, background.incoming, NULL, NULL);
}

void restart_background_interval(Uint32 now)
{
    background.shown_at = now;
}

void quit_background(void)
{
    if (background.worker != NULL) {
        SDL_AtomicSet(&background.stop_requested, 1);
        SDL_WaitThread(background.worker, NULL);
    }
    SDL_FreeSurface(background.prepared);
    SDL_DestroyTexture(background.current);
    SDL_DestroyTexture(background.incoming);
    for (int i = 0; i < background.files.count; i++)
        free(background.files.paths[i]);
    free(background.files.paths);
    free(background.order);
}
