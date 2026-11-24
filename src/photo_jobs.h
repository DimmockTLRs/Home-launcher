#ifndef HOME_LAUNCHER_PHOTO_JOBS_H
#define HOME_LAUNCHER_PHOTO_JOBS_H

// Include launcher.h before this header.
#define PHOTO_PAGE_SIZE 6

typedef enum {
    PHOTO_JOB_SCAN,
    PHOTO_JOB_THUMBNAILS,
    PHOTO_JOB_IMPORT,
    PHOTO_JOB_REMOVE
} PhotoJobKind;

typedef struct {
    PhotoJobKind kind;
    bool ok;
    char message[256];
    PhotoList photos;
    SDL_Surface *thumbnails[PHOTO_PAGE_SIZE];
    int first;
    int count;
} PhotoJobResult;

// One job may be prepared at a time. Accepted import jobs take ownership of
// files. Thumbnail jobs copy the requested page's paths and clamp to its end.
bool start_photo_scan(const char *folder);
bool start_photo_thumbnails(const PhotoList *photos, int first, int count);
bool start_photo_import(const char *folder, PhotoList *files);
bool start_photo_remove(const char *folder, const char *path);

// On success the caller takes ownership of result photos and thumbnails.
// For imports, count reports how many copies succeeded, including partial imports.
bool poll_photo_job(PhotoJobResult *result);
bool photo_job_busy(void);
void photo_job_quit(void);

#endif
