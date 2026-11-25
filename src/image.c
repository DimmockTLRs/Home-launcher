#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include <launcher_config.h>
#include "image.h"
#include "util.h"
#include "debug.h"
#include <ini.h>
#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

extern Config config;
extern SDL_Renderer *renderer;
NSVGrasterizer *rasterizer = NULL;

// A function to initalize SVG rasterization
int init_svg()
{
    rasterizer = nsvgCreateRasterizer();
    if (rasterizer == NULL) {
        log_fatal("Could not initialize SVG rasterizer.");
        return 1;
    }
    return 0;
}

// A function to quit the SVG subsystem
void quit_svg()
{
    nsvgDeleteRasterizer(rasterizer);
}

// A function to load a texture from a file
SDL_Texture *load_texture_from_file(const char *path)
{
    SDL_Surface *surface = NULL;
    SDL_Texture *texture = NULL;
    if (path != NULL) {
        surface = IMG_Load(path);
        if (surface == NULL) {
            log_error(
                "Could not load image %s\n%s", 
                path, 
                IMG_GetError()
            );
        }
        else
            texture = load_texture(surface);
    }
    return texture;
}

SDL_Texture *default_app_icon(void)
{
    char icon[] = "<svg viewBox='0 0 96 96'>"
        "<rect x='10' y='16' width='76' height='64' rx='9' fill='none' "
        "stroke='#D1E4FC' stroke-width='5'/>"
        "<path d='M13 35 H83' fill='none' stroke='#D1E4FC' stroke-width='5'/>"
        "<circle cx='24' cy='26' r='3' fill='#D1E4FC'/>"
        "<circle cx='34' cy='26' r='3' fill='#D1E4FC'/></svg>";
    return rasterize_svg(icon, -1, -1, NULL);
}

// A function to load a texture from a    SDL surface
SDL_Texture *load_texture(SDL_Surface *surface)
{
    SDL_Texture *texture = NULL;
    if (surface == NULL)
        return NULL;

    //Convert surface to screen format
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == NULL)
        log_error("Could not create texture %s", SDL_GetError());
    SDL_FreeSurface(surface);
    return texture;
}

// A function to rasterize an SVG from an existing text buffer
SDL_Texture *rasterize_svg(char *buffer, int w, int h, SDL_Rect *rect)
{
    NSVGimage *image = NULL;
    unsigned char *pixel_buffer = NULL;
    int width, height, pitch;
    float scale;

    // Parse SVG to NSVGimage struct
    image = nsvgParse(buffer, "px", 96.0f);
    if (image == NULL) {
        log_error("could not open SVG image.");
        return NULL;
    }

    // Calculate scaling and dimensions
    if (w == -1 && h == -1) {
        scale = 1.0f;
        width = (int) image->width;
        height = (int) image->height;
    }
    else if (w == -1 && h != -1) {
        scale = (float) h / (float) image->height;
        width = (int) ceil((double) image->width * (double) scale);
        height = h;
    }
    else if (w != -1 && h == -1) {
        scale = (float) w / (float) image->width;
        width = w;
        height = (int) ceil((double) image->height * (double) scale);
    }
    else {
        scale = (float) w / (float) image->width;
        width = w;
        height = h;
    }
    
    // Allocate memory
    pitch = 4*width;
    pixel_buffer = malloc((size_t) (4*width*height));
    if (pixel_buffer == NULL) {
        log_error("Could not alloc SVG pixel buffer.");
        return NULL;
    }

    // Rasterize image
    nsvgRasterize(rasterizer, image, 0, 0, scale, pixel_buffer, width, height, pitch);
    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(pixel_buffer,
                               width,
                               height,
                               32,
                               pitch,
                               COLOR_MASKS
                           );
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (rect != NULL) {
        rect->w = width;
        rect->h = height;
    }
    free(pixel_buffer);
    SDL_FreeSurface(surface);
    nsvgDelete(image);
    return texture;
}

// A function to render the highlight for the buttons
SDL_Texture *render_highlight(int width, int height, SDL_Rect *rect)
{
    // Insert user config variables into SVG-formatted text buffer
    char *buffer = NULL;
    char *outline_buffer = NULL;
    if (config.highlight_outline_size) {
        float stroke_opacity = ((float) config.highlight_outline_color.a) / 255.0f;
        format_highlight_outline(&outline_buffer, 
            config.highlight_outline_size, 
            config.highlight_outline_color, 
            stroke_opacity
        );
    }
    else
        outline_buffer = "";

    float fill_opacity = ((float) config.highlight_fill_color.a) / 255.0f;
    format_highlight(&buffer, 
        width, 
        height, 
        config.highlight_rx, 
        config.highlight_fill_color, 
        fill_opacity, 
        outline_buffer
    );

    // Rasterize the SVG
    SDL_Texture *texture = rasterize_svg(buffer, -1, -1, rect);
    
    // Cleanup
    free(buffer);
    if (config.highlight_outline_size)
        free(outline_buffer);

    return texture;
}

// A function to render the scroll indicators
void render_scroll_indicators(Scroll *scroll, int height, Geometry *geo)
{
    // Format the SVG
    char *buffer = NULL;
    float opacity = (float) config.scroll_indicator_fill_color.a / 255.0f;
    format_scroll_indicator(&buffer, 
        config.scroll_indicator_fill_color, 
        config.scroll_indicator_outline_size, 
        config.scroll_indicator_outline_color, 
        opacity
    );

    // Rasterize the SVG
    scroll->texture = rasterize_svg(buffer,
                          -1,
                          height,
                          &scroll->rect_right
                      );
    free(buffer);
    scroll->rect_left.w = scroll->rect_right.w;
    scroll->rect_left.h = scroll->rect_right.h;
    if (scroll->texture == NULL) {
        log_error("Could not render scroll indicator, disabling feature");
        free(scroll);
        config.scroll_indicators = false;
        return;
    }

    // Calculate screen position
    scroll->rect_right.y = geo->screen_height - geo->screen_margin - scroll->rect_right.h;
    scroll->rect_right.x = geo->screen_width - geo->screen_margin - scroll->rect_right.w;
    scroll->rect_left.y = scroll->rect_right.y;
    scroll->rect_left.x = geo->screen_margin;
}

// A function to render text
SDL_Surface *render_text(const char *text, TextInfo *info, SDL_Rect *rect, int *text_height)
{
    TTF_Font *output_font = NULL;
    TTF_Font *reduced_font = NULL; // Font for Shrink text oversize mode
    int w, h;

    // Copy text into new buffer in case we need to manipulate it
    char *text_buffer = strdup(text);

    // Calculate size of the rendered title
    TTF_SizeUTF8(info->font, text_buffer, &w, &h);

    // If title is too large to fit
    if (info->oversize_mode != OVERSIZE_NONE && w > info->max_width) {

        // Truncate mode:
        if (info->oversize_mode == OVERSIZE_TRUNCATE) {
            utf8_truncate(text_buffer, w, info->max_width);
            TTF_SizeUTF8(info->font, text_buffer, &w, &h);
        }

        // Shrink mode:
        else if (info->oversize_mode == OVERSIZE_SHRINK) {
            int reduced_font_size = (int) info->font_size - 1;
            reduced_font = TTF_OpenFont(*info->font_path, reduced_font_size);
            TTF_SizeUTF8(reduced_font, text_buffer, &w, &h);

            // Keep trying smaller font until it fits
            while (w > info->max_width && reduced_font_size > 0) {
                TTF_CloseFont(reduced_font);
                reduced_font = NULL;
                reduced_font_size--;
                reduced_font = TTF_OpenFont(*info->font_path, reduced_font_size);
                TTF_SizeUTF8(reduced_font, text_buffer, &w, &h);
            }

            if (reduced_font_size)
                output_font = reduced_font;
            else
                reduced_font = NULL;
        }
    }
    if (reduced_font == NULL)
        output_font = info->font;

    // Render surface
    SDL_Surface *surface = NULL;
    if (info->shadow) {
        int shadow_offset = h / 40;
        if (shadow_offset < 2)
            shadow_offset = 2;
        SDL_Surface *foreground = TTF_RenderUTF8_Blended(output_font,
                                      text_buffer,
                                      *info->color
                                  );
        SDL_Surface *shadow = TTF_RenderUTF8_Blended(output_font, 
                                  text_buffer, 
                                  *info->shadow_color
                              );
        surface = SDL_CreateRGBSurfaceWithFormat(0, 
                      foreground->w + shadow_offset, 
                      foreground->h + shadow_offset, 
                      32,
                      SDL_PIXELFORMAT_ARGB8888
                  );
        Uint32 color = SDL_MapRGBA(surface->format, 0, 0, 0, 0);
        SDL_FillRect(surface, NULL, color);
        SDL_Rect shadow_rect = {shadow_offset, shadow_offset, shadow->w, shadow->h};
        SDL_BlitSurface(shadow, NULL, surface, &shadow_rect);
        SDL_Rect rect = {0, 0, foreground->w, foreground->h};
        SDL_BlitSurface(foreground, NULL, surface, &rect);
        SDL_FreeSurface(foreground);
        SDL_FreeSurface(shadow);
    }
    else
        surface = TTF_RenderUTF8_Blended(output_font,
                      text_buffer,
                      *info->color
                  );

    // Set geometry
    rect->w = surface->w;
    rect->h = surface->h;
    if (info->oversize_mode == OVERSIZE_SHRINK && text_height != NULL)
        *text_height = h;

    // Clean up
    if (reduced_font != NULL)
        TTF_CloseFont(reduced_font);
    free(text_buffer);
    
    return surface;
}

// A function to render text into a texture
SDL_Texture *render_text_texture(const char *text, TextInfo *info, SDL_Rect *rect, int *text_height)
{
    SDL_Surface *surface = render_text(text, info, rect, text_height);
    return load_texture(surface);
}

// A function to load a font from a file
int load_font(TextInfo *info, const char *default_font)
{
    char *font_path = *info->font_path;
    // Load user specified font
    if (font_path != NULL)
        info->font = TTF_OpenFont(font_path, info->font_size);

    // Try to load default font if we failed loading from config file
    if (info->font == NULL) {
        log_error("Could not initialize font from config file");
        const char *prefixes[2];
        char fonts_exe_buffer[MAX_PATH_CHARS + 1];
        prefixes[0] = join_paths(fonts_exe_buffer, sizeof(fonts_exe_buffer), 3, config.exe_path, PATH_ASSETS_EXE, PATH_FONTS_EXE);
#ifdef __unix__
        prefixes[1] = PATH_FONTS_SYSTEM;
#else
        prefixes[1] = PATH_FONTS_RELATIVE;
#endif
        char *default_font_path = find_file(default_font, 2, prefixes);

        // Replace user font with default in config
        if (default_font_path != NULL) {
            info->font = TTF_OpenFont(default_font_path, info->font_size);
            free(font_path);
            *(info->font_path) = strdup(default_font_path);
            free(default_font_path);
        }
        if (info->font == NULL) {
            log_fatal("Could not load default font");
            return 1;
        }
    }
    return 0;
}
