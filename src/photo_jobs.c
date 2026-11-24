#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_image.h>
#include "launcher.h"
#include "photo_files.h"
#include "photo_jobs.h"
#include "platform/platform.h"

#define THUMBNAIL_MAX_WIDTH 512
#define THUMBNAIL_MAX_HEIGHT 352

typedef struct {
    SDL_Thread *thread;
    SDL_atomic_t ready;
    SDL_atomic_t cancelled;
    char *folder;
    char *removed_path;
    PhotoList input;
    PhotoJobResult result;
} PhotoJob;

static PhotoJob job;

static void clear_photo_job(void)
{
    free(job.folder);
    free(job.removed_path);
    free_photo_list(&job.input);
    free_photo_list(&job.result.photos);
    for (int i = 0; i < PHOTO_PAGE_SIZE; i++)
        SDL_FreeSurface(job.result.thumbnails[i]);
    job = (PhotoJob){0};
}

static int compare_photo_paths(const void *left, const void *right)
{
    const char *a = *(char *const *)left;
    const char *b = *(char *const *)right;
    int comparison = SDL_strcasecmp(a, b);
    return comparison != 0 ? comparison : strcmp(a, b);
}

static void scan_photos(void)
{
    PhotoList *photos = &job.result.photos;
    if (!scan_slideshow_directory(photos, job.folder, &job.cancelled)) {
        free_photo_list(photos);
        SDL_strlcpy(job.result.message, "Could not read the photo folder.", sizeof(job.result.message));
        return;
    }
    if (photos->count > 1)
        qsort(photos->paths, (size_t)photos->count, sizeof(*photos->paths), compare_photo_paths);
    if (photos->count == 0)
        SDL_strlcpy(job.result.message, "No JPEG, PNG or WebP photos found.", sizeof(job.result.message));
    job.result.ok = true;
}

static SDL_Surface *prepare_thumbnail(const char *path)
{
    SDL_Surface *photo = IMG_Load(path);
    if (photo == NULL)
        return NULL;
    if (SDL_AtomicGet(&job.cancelled) || photo->w <= 0 || photo->h <= 0) {
        SDL_FreeSurface(photo);
        return NULL;
    }
    if (photo->format->palette != NULL || SDL_HasColorKey(photo)) {
        // SDL cannot directly scale a paletted source. Convert its palette
        // and transparent colour key before preparing the small surface.
        SDL_Surface *rgba = SDL_ConvertSurfaceFormat(photo, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(photo);
        if (rgba == NULL)
            return NULL;
        photo = rgba;
    }

    double scale = SDL_min(1.0, SDL_min((double)THUMBNAIL_MAX_WIDTH / photo->w,
        (double)THUMBNAIL_MAX_HEIGHT / photo->h));
    int width = SDL_max(1, (int)(photo->w * scale + 0.5));
    int height = SDL_max(1, (int)(photo->h * scale + 0.5));
    SDL_Surface *thumbnail = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (thumbnail != NULL) {
        SDL_SetSurfaceBlendMode(photo, SDL_BLENDMODE_NONE);
        if (SDL_BlitScaled(photo, NULL, thumbnail, NULL) < 0) {
            SDL_FreeSurface(thumbnail);
            thumbnail = NULL;
        }
    }
    SDL_FreeSurface(photo);
    return thumbnail;
}

static void prepare_page(void)
{
    for (int i = 0; i < job.input.count && !SDL_AtomicGet(&job.cancelled); i++) {
        job.result.thumbnails[i] = prepare_thumbnail(job.input.paths[i]);
        if (job.result.thumbnails[i] == NULL)
            SDL_strlcpy(job.result.message, "Some thumbnails could not be opened.", sizeof(job.result.message));
    }
    job.result.ok = true;
}

static void import_photos(void)
{
    char first_error[192] = "";
    int copied = 0;
    for (int i = 0; i < job.input.count && !SDL_AtomicGet(&job.cancelled); i++) {
        char error[192] = "";
        if (copy_photo_into_folder(job.input.paths[i], job.folder, error, sizeof(error)))
            copied++;
        else if (first_error[0] == '\0')
            SDL_strlcpy(first_error, error[0] != '\0' ? error : "A photo could not be copied.", sizeof(first_error));
    }
    job.result.count = copied;
    job.result.ok = copied == job.input.count;
    if (job.result.ok)
        SDL_snprintf(job.result.message, sizeof(job.result.message), "Imported %d %s.", copied,
            copied == 1 ? "photo" : "photos");
    else
        SDL_snprintf(job.result.message, sizeof(job.result.message), "Imported %d of %d photos. %s",
            copied, job.input.count, first_error);
}

static int run_photo_job(void *unused)
{
    (void)unused;
    switch (job.result.kind) {
        case PHOTO_JOB_SCAN:
            scan_photos();
            break;
        case PHOTO_JOB_THUMBNAILS:
            prepare_page();
            break;
        case PHOTO_JOB_IMPORT:
            import_photos();
            break;
        case PHOTO_JOB_REMOVE:
            if (!SDL_AtomicGet(&job.cancelled)) {
                job.result.ok = remove_managed_photo(job.removed_path, job.folder,
                    job.result.message, sizeof(job.result.message));
                if (job.result.ok)
                    SDL_strlcpy(job.result.message, "Photo removed.", sizeof(job.result.message));
            }
            break;
    }
    SDL_AtomicSet(&job.ready, 1);
    return 0;
}

static bool valid_path(const char *path)
{
    return path != NULL && path[0] != '\0';
}

static bool start_photo_job(PhotoJobKind kind)
{
    job.result.kind = kind;
    job.thread = SDL_CreateThread(run_photo_job, "Manage background photos", NULL);
    return job.thread != NULL;
}

bool start_photo_scan(const char *folder)
{
    if (photo_job_busy() || !valid_path(folder))
        return false;
    job.folder = strdup(folder);
    if (job.folder != NULL && start_photo_job(PHOTO_JOB_SCAN))
        return true;
    clear_photo_job();
    return false;
}

bool start_photo_thumbnails(const PhotoList *photos, int first, int count)
{
    if (photo_job_busy() || photos == NULL || photos->paths == NULL ||
        first < 0 || first >= photos->count || count <= 0)
        return false;
    count = SDL_min(PHOTO_PAGE_SIZE, SDL_min(count, photos->count - first));
    job.input.paths = calloc((size_t)count, sizeof(*job.input.paths));
    if (job.input.paths == NULL)
        return false;
    job.input.capacity = count;
    for (int i = 0; i < count; i++) {
        char *path = photos->paths[first + i] != NULL ? strdup(photos->paths[first + i]) : NULL;
        if (path == NULL) {
            clear_photo_job();
            return false;
        }
        job.input.paths[job.input.count++] = path;
    }
    job.result.first = first;
    job.result.count = count;
    if (start_photo_job(PHOTO_JOB_THUMBNAILS))
        return true;
    clear_photo_job();
    return false;
}

bool start_photo_import(const char *folder, PhotoList *files)
{
    if (photo_job_busy() || !valid_path(folder) || files == NULL || files->paths == NULL || files->count <= 0)
        return false;
    job.folder = strdup(folder);
    if (job.folder == NULL)
        return false;

    job.input = *files;
    if (start_photo_job(PHOTO_JOB_IMPORT)) {
        *files = (PhotoList){0};
        return true;
    }
    // The caller retains its selection if no worker could be started.
    job.input = (PhotoList){0};
    clear_photo_job();
    return false;
}

bool start_photo_remove(const char *folder, const char *path)
{
    if (photo_job_busy() || !valid_path(folder) || !valid_path(path))
        return false;
    job.folder = strdup(folder);
    job.removed_path = strdup(path);
    if (job.folder != NULL && job.removed_path != NULL && start_photo_job(PHOTO_JOB_REMOVE))
        return true;
    clear_photo_job();
    return false;
}

bool photo_job_busy(void)
{
    return job.thread != NULL;
}

bool poll_photo_job(PhotoJobResult *result)
{
    if (result == NULL || !photo_job_busy() || !SDL_AtomicGet(&job.ready))
        return false;
    SDL_WaitThread(job.thread, NULL);
    *result = job.result;
    job.result = (PhotoJobResult){0};
    clear_photo_job();
    return true;
}

void photo_job_quit(void)
{
    if (photo_job_busy()) {
        SDL_AtomicSet(&job.cancelled, 1);
        SDL_WaitThread(job.thread, NULL);
    }
    clear_photo_job();
}
