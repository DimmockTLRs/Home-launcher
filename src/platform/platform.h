#ifdef _WIN32
#define FILE_MODE_WRITE "wt"
#else
#define FILE_MODE_WRITE "w"
#endif

// Abstracted platform function prototypes
bool file_exists(const char *path);
void get_region(char *buffer);
bool scan_slideshow_directory(PhotoList *files, const char *directory, SDL_atomic_t *cancel);
bool start_process(char *cmd, bool application);
bool valid_website_url(const char *url);
bool open_website(const char *url);
void scmd_shutdown(void);
void scmd_restart(void);
void scmd_sleep(void);

// Linux-specific function prototypes
#ifdef __unix__
void make_directory(const char *directory);
void print_usage(void);
#endif

// Windows-specific function prototypes
#ifdef _WIN32
bool has_exit_hotkey(void);
void set_exit_hotkey(SDL_Keycode keycode);
void register_exit_hotkey(void);
void check_exit_hotkey(SDL_SysWMmsg *msg);
void set_foreground_window(void);
void debug_foreground_window(void);
void make_window_transparent(void);
void restore_opaque_window(void);
void hide_cursor(Entry* entry);
#endif