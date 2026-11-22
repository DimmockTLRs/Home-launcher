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

extern Config config;

static struct {
    PhotoList files;
    int *order;
    int current_index;
    int remaining_photos;
    bool catalog_scanned;
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
} background = { .current_index = -1 };

// Calculate the source crop or destination inset using the actual display aspect.
static void photo_rects(SDL_Rect *source, SDL_Rect *destination)
{
    Sint64 source_ratio = (Sint64)source->w * destination->h;
    Sint64 destination_ratio = (Sint64)destination->w * source->h;

    if (config.photo_fit == PHOTO_CONTAIN) {
        if (source_ratio > destination_ratio) {
            int height = SDL_max(1, (int)(destination_ratio / source->w));
            destination->y = (destination->h - height) / 2;
            destination->h = height;
        }
        else {
            int width = SDL_max(1, (int)(source_ratio / source->h));
            destination->x = (destination->w - width) / 2;
            destination->w = width;
        }
    }
    else if (config.photo_fit == PHOTO_COVER) {
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

// Prepare an opaque frame, so the bars around a portrait and transparent pixels
// participate in the same crossfade as the photo.
static SDL_Surface *prepare_photo(const char *path)
{
    SDL_Surface *photo = IMG_Load(path);
    if (photo == NULL)
        return NULL;

    SDL_Surface *frame = SDL_CreateRGBSurfaceWithFormat(0, background.width,
        background.height, 32, SDL_PIXELFORMAT_RGB888);
    if (frame != NULL) {
        SDL_Color fill = config.background_color;
        SDL_Rect source = { 0, 0, photo->w, photo->h };
        SDL_Rect destination = { 0, 0, frame->w, frame->h };
        photo_rects(&source, &destination);

        int status = SDL_FillRect(frame, NULL, SDL_MapRGB(frame->format, fill.r, fill.g, fill.b));
        if (status == 0)
            status = SDL_BlitScaled(photo, &source, frame, &destination);
        if (status < 0) {
            SDL_FreeSurface(frame);
            frame = NULL;
        }
    }

    SDL_FreeSurface(photo);
    return frame;
}

static SDL_Surface *next_slideshow_photo(void)
{
    if (!background.catalog_scanned) {
        background.catalog_scanned = true;
        if (!scan_slideshow_directory(&background.files, config.slideshow_directory,
                &background.stop_requested) || background.files.count == 0)
            return NULL;

        background.order = malloc((size_t)background.files.count * sizeof(*background.order));
        if (background.order == NULL)
            return NULL;
        background.remaining_photos = background.files.count;
        random_array(background.order, background.files.count);
    }

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
        background.order[index] = -1;
        background.remaining_photos--;
    }
    return NULL;
}

static int prepare_background(void *unused)
{
    (void)unused;
    background.prepared = config.background_mode == BACKGROUND_SLIDESHOW ?
        next_slideshow_photo() : prepare_photo(config.background_image);
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
    bool slideshow = config.background_mode == BACKGROUND_SLIDESHOW;
    if (!slideshow && config.background_mode != BACKGROUND_IMAGE)
        return;
    const char *path = slideshow ? config.slideshow_directory : config.background_image;
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
        const char *path = config.background_mode == BACKGROUND_SLIDESHOW ?
            config.slideshow_directory : config.background_image;
        if (background.current == NULL) {
            log_error("Could not prepare a photo background from '%s'; using the background color", path);
            background.finished = true;
        }
        else {
            log_debug("Slideshow kept the current image: no replacement texture available");
            background.shown_at = now;
            background.finished = background.remaining_photos < 2;
        }
        return;
    }

    if (background.current != NULL && config.slideshow_transition_time > 0) {
        SDL_SetTextureBlendMode(frame, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(frame, 0);
        background.incoming = frame;
        background.fade_started_at = SDL_GetTicks();
        log_debug("Starting slideshow crossfade (%u ms)", (unsigned)config.slideshow_transition_time);
    }
    else {
        SDL_DestroyTexture(background.current);
        background.current = frame;
        background.shown_at = SDL_GetTicks();
        background.finished = config.background_mode != BACKGROUND_SLIDESHOW || background.remaining_photos < 2;
        if (config.background_mode == BACKGROUND_SLIDESHOW)
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
