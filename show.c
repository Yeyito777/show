#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo.h>
#include <glib.h>
#include <poppler.h>

typedef enum {
    CONTENT_IMAGE,
    CONTENT_PDF,
} ContentKind;

typedef struct {
    ContentKind kind;
    SDL_Surface *image_surface;
    PopplerDocument *pdf_doc;
    int page_count;
    int page_index;

    SDL_Texture *texture;
    int base_w;
    int base_h;

    int rendered_page;
    int rendered_w;
    int rendered_h;
    int rendered_rotation;
} Content;

static void die(const char *msg)
{
    fprintf(stderr, "show: %s\n", msg);
    exit(1);
}

static void set_wm_class(SDL_Window *window, const char *name)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return;
    if (info.subsystem != SDL_SYSWM_X11) return;

    XClassHint hint;
    hint.res_name = (char *)name;
    hint.res_class = (char *)name;
    XSetClassHint(info.info.x11.display, info.info.x11.window, &hint);
    XFlush(info.info.x11.display);
}

static bool has_pdf_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".pdf") == 0;
}

static void invalidate_texture(Content *content)
{
    if (content->texture) {
        SDL_DestroyTexture(content->texture);
        content->texture = NULL;
    }
    content->rendered_page = -1;
    content->rendered_w = -1;
    content->rendered_h = -1;
    content->rendered_rotation = -1;
}

static void destroy_content(Content *content)
{
    invalidate_texture(content);
    if (content->image_surface)
        SDL_FreeSurface(content->image_surface);
    if (content->pdf_doc)
        g_object_unref(content->pdf_doc);
}

static void get_pdf_page_size(PopplerDocument *doc, int page_index, int *w, int *h)
{
    PopplerPage *page = poppler_document_get_page(doc, page_index);
    if (!page)
        die("cannot open PDF page");

    double pw, ph;
    poppler_page_get_size(page, &pw, &ph);
    g_object_unref(page);

    *w = (int)(pw + 0.5);
    *h = (int)(ph + 0.5);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static PopplerDocument *open_pdf_document(const char *path)
{
    char *absolute = realpath(path, NULL);
    if (!absolute)
        return NULL;

    GError *error = NULL;
    char *uri = g_filename_to_uri(absolute, NULL, &error);
    free(absolute);
    if (!uri) {
        if (error) g_error_free(error);
        return NULL;
    }

    PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, &error);
    g_free(uri);
    if (!doc) {
        if (error) g_error_free(error);
        return NULL;
    }

    return doc;
}

static void load_content(const char *path, Content *content)
{
    memset(content, 0, sizeof(*content));
    content->rendered_page = -1;
    content->rendered_w = -1;
    content->rendered_h = -1;
    content->rendered_rotation = -1;

    if (!has_pdf_extension(path)) {
        SDL_Surface *loaded = IMG_Load(path);
        if (loaded) {
            SDL_Surface *converted = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
            SDL_FreeSurface(loaded);
            if (!converted)
                die(SDL_GetError());

            content->kind = CONTENT_IMAGE;
            content->image_surface = converted;
            content->base_w = converted->w;
            content->base_h = converted->h;
            return;
        }
    }

    content->pdf_doc = open_pdf_document(path);
    if (content->pdf_doc) {
        content->kind = CONTENT_PDF;
        content->page_count = poppler_document_get_n_pages(content->pdf_doc);
        if (content->page_count < 1)
            die("PDF has no pages");
        content->page_index = 0;
        get_pdf_page_size(content->pdf_doc, content->page_index,
                          &content->base_w, &content->base_h);
        return;
    }

    if (has_pdf_extension(path))
        die("cannot open PDF");
    die(IMG_GetError());
}

static void update_window_title(SDL_Window *window, const Content *content, const char *path)
{
    char title[PATH_MAX + 64];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    if (content->kind == CONTENT_PDF) {
        snprintf(title, sizeof(title), "show - %s (%d/%d)",
                 base, content->page_index + 1, content->page_count);
    } else {
        snprintf(title, sizeof(title), "show - %s", base);
    }

    SDL_SetWindowTitle(window, title);
}

static void compute_display_size(const Content *content,
                                 int out_w,
                                 int out_h,
                                 double zoom,
                                 int rotation,
                                 int *display_w,
                                 int *display_h)
{
    int rotated_w = (rotation % 2 == 0) ? content->base_w : content->base_h;
    int rotated_h = (rotation % 2 == 0) ? content->base_h : content->base_w;

    double fit_w = (double)out_w / (double)rotated_w;
    double fit_h = (double)out_h / (double)rotated_h;
    double fit = fit_w < fit_h ? fit_w : fit_h;

    if (fit <= 0.0)
        fit = 1.0;

    *display_w = (int)(rotated_w * fit * zoom + 0.5);
    *display_h = (int)(rotated_h * fit * zoom + 0.5);
    if (*display_w < 1) *display_w = 1;
    if (*display_h < 1) *display_h = 1;
}

static void clamp_pan(int out_w, int out_h, int display_w, int display_h,
                      double *pan_x, double *pan_y)
{
    double max_x = display_w > out_w ? (double)(display_w - out_w) / 2.0 : 0.0;
    double max_y = display_h > out_h ? (double)(display_h - out_h) / 2.0 : 0.0;

    if (*pan_x > max_x) *pan_x = max_x;
    if (*pan_x < -max_x) *pan_x = -max_x;
    if (*pan_y > max_y) *pan_y = max_y;
    if (*pan_y < -max_y) *pan_y = -max_y;
}

static SDL_Surface *rotate_surface(SDL_Surface *src, int rotation)
{
    rotation = ((rotation % 4) + 4) % 4;
    if (rotation == 0)
        return NULL;

    int dst_w = (rotation % 2 == 0) ? src->w : src->h;
    int dst_h = (rotation % 2 == 0) ? src->h : src->w;

    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, dst_w, dst_h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst)
        die(SDL_GetError());

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    Uint32 *sp = (Uint32 *)src->pixels;
    Uint32 *dp = (Uint32 *)dst->pixels;
    int spitch = src->pitch / 4;
    int dpitch = dst->pitch / 4;

    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < src->w; x++) {
            Uint32 pixel = sp[y * spitch + x];
            int dx = 0;
            int dy = 0;

            switch (rotation) {
            case 1:
                dx = src->h - 1 - y;
                dy = x;
                break;
            case 2:
                dx = src->w - 1 - x;
                dy = src->h - 1 - y;
                break;
            case 3:
                dx = y;
                dy = src->w - 1 - x;
                break;
            default:
                break;
            }

            dp[dy * dpitch + dx] = pixel;
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
    return dst;
}

static void ensure_image_texture(Content *content, SDL_Renderer *renderer, int rotation)
{
    rotation = ((rotation % 4) + 4) % 4;
    if (content->texture && content->rendered_rotation == rotation)
        return;

    invalidate_texture(content);

    if (rotation == 0) {
        content->texture = SDL_CreateTextureFromSurface(renderer, content->image_surface);
    } else {
        SDL_Surface *rotated = rotate_surface(content->image_surface, rotation);
        content->texture = SDL_CreateTextureFromSurface(renderer, rotated);
        SDL_FreeSurface(rotated);
    }

    if (!content->texture)
        die(SDL_GetError());

    content->rendered_rotation = rotation;
}

static void render_pdf_page(Content *content,
                            SDL_Renderer *renderer,
                            int rotation,
                            int display_w,
                            int display_h)
{
    rotation = ((rotation % 4) + 4) % 4;

    if (content->texture &&
        content->rendered_page == content->page_index &&
        content->rendered_w == display_w &&
        content->rendered_h == display_h &&
        content->rendered_rotation == rotation)
        return;

    PopplerPage *page = poppler_document_get_page(content->pdf_doc, content->page_index);
    if (!page)
        die("cannot render PDF page");

    double page_w, page_h;
    poppler_page_get_size(page, &page_w, &page_h);
    if (page_w < 1.0) page_w = 1.0;
    if (page_h < 1.0) page_h = 1.0;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          display_w, display_h);
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    switch (rotation) {
    case 0:
        cairo_scale(cr, (double)display_w / page_w, (double)display_h / page_h);
        break;
    case 1:
        cairo_translate(cr, display_w, 0);
        cairo_rotate(cr, 3.14159265358979323846 / 2.0);
        cairo_scale(cr, (double)display_h / page_w, (double)display_w / page_h);
        break;
    case 2:
        cairo_translate(cr, display_w, display_h);
        cairo_rotate(cr, 3.14159265358979323846);
        cairo_scale(cr, (double)display_w / page_w, (double)display_h / page_h);
        break;
    case 3:
        cairo_translate(cr, 0, display_h);
        cairo_rotate(cr, -3.14159265358979323846 / 2.0);
        cairo_scale(cr, (double)display_h / page_w, (double)display_w / page_h);
        break;
    }

    poppler_page_render(page, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    SDL_Surface *sdl_surface = SDL_CreateRGBSurfaceWithFormatFrom(
        cairo_image_surface_get_data(surface),
        display_w,
        display_h,
        32,
        cairo_image_surface_get_stride(surface),
        SDL_PIXELFORMAT_BGRA32
    );
    if (!sdl_surface)
        die(SDL_GetError());

    invalidate_texture(content);
    content->texture = SDL_CreateTextureFromSurface(renderer, sdl_surface);
    SDL_FreeSurface(sdl_surface);
    cairo_surface_destroy(surface);
    g_object_unref(page);

    if (!content->texture)
        die(SDL_GetError());

    content->rendered_page = content->page_index;
    content->rendered_w = display_w;
    content->rendered_h = display_h;
    content->rendered_rotation = rotation;
}

static void ensure_texture(Content *content,
                           SDL_Renderer *renderer,
                           int rotation,
                           int display_w,
                           int display_h)
{
    if (content->kind == CONTENT_IMAGE) {
        ensure_image_texture(content, renderer, rotation);
        return;
    }

    render_pdf_page(content, renderer, rotation, display_w, display_h);
}

static bool set_pdf_page(Content *content, int new_index)
{
    if (content->kind != CONTENT_PDF)
        return false;
    if (new_index < 0 || new_index >= content->page_count)
        return false;
    if (new_index == content->page_index)
        return false;

    content->page_index = new_index;
    get_pdf_page_size(content->pdf_doc, content->page_index,
                      &content->base_w, &content->base_h);
    invalidate_texture(content);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: show <file>\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        die(SDL_GetError());

    int img_flags = IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_TIF | IMG_INIT_WEBP;
    IMG_Init(img_flags);

    Content content;
    load_content(argv[1], &content);

    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) < 0)
        die(SDL_GetError());

    const int margin = 64;
    int max_w = dm.w - margin;
    int max_h = dm.h - margin;
    if (max_w < 1) max_w = dm.w;
    if (max_h < 1) max_h = dm.h;

    double initial_scale_w = (double)max_w / (double)content.base_w;
    double initial_scale_h = (double)max_h / (double)content.base_h;
    double initial_scale = initial_scale_w < initial_scale_h ? initial_scale_w : initial_scale_h;
    if (initial_scale > 1.0) initial_scale = 1.0;

    int win_w = (int)(content.base_w * initial_scale + 0.5);
    int win_h = (int)(content.base_h * initial_scale + 0.5);
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;

    SDL_Window *window = SDL_CreateWindow(
        "show",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        win_w,
        win_h,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS
    );
    if (!window)
        die(SDL_GetError());

    set_wm_class(window, "show");
    update_window_title(window, &content, argv[1]);

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer)
            die(SDL_GetError());
    }

    double zoom = 1.0;
    double pan_x = 0.0;
    double pan_y = 0.0;
    int rotation = 0;
    int display_w = win_w;
    int display_h = win_h;
    compute_display_size(&content, win_w, win_h, zoom, rotation, &display_w, &display_h);
    ensure_texture(&content, renderer, rotation, display_w, display_h);

    SDL_ShowWindow(window);

    bool quit = false;
    bool fullscreen = false;

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                quit = true;
            } else if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE:
                    quit = true;
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_f:
                    if (!fullscreen) {
                        if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0)
                            fullscreen = true;
                    } else {
                        if (SDL_SetWindowFullscreen(window, 0) == 0) {
                            fullscreen = false;
                            SDL_SetWindowSize(window, win_w, win_h);
                            SDL_SetWindowPosition(window,
                                                  SDL_WINDOWPOS_CENTERED,
                                                  SDL_WINDOWPOS_CENTERED);
                            SDL_RaiseWindow(window);
                        }
                    }
                    break;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                case SDLK_KP_PLUS: {
                    int out_w, out_h;
                    int old_w, old_h;
                    int new_w, new_h;

                    if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) < 0)
                        die(SDL_GetError());

                    compute_display_size(&content, out_w, out_h, zoom, rotation, &old_w, &old_h);
                    zoom *= 1.25;
                    if (zoom > 64.0) zoom = 64.0;
                    compute_display_size(&content, out_w, out_h, zoom, rotation, &new_w, &new_h);
                    if (old_w > 0) pan_x *= (double)new_w / (double)old_w;
                    if (old_h > 0) pan_y *= (double)new_h / (double)old_h;
                    clamp_pan(out_w, out_h, new_w, new_h, &pan_x, &pan_y);
                    break;
                }
                case SDLK_MINUS:
                case SDLK_KP_MINUS: {
                    int out_w, out_h;
                    int old_w, old_h;
                    int new_w, new_h;

                    if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) < 0)
                        die(SDL_GetError());

                    compute_display_size(&content, out_w, out_h, zoom, rotation, &old_w, &old_h);
                    zoom /= 1.25;
                    if (zoom < 0.1) zoom = 0.1;
                    compute_display_size(&content, out_w, out_h, zoom, rotation, &new_w, &new_h);
                    if (old_w > 0) pan_x *= (double)new_w / (double)old_w;
                    if (old_h > 0) pan_y *= (double)new_h / (double)old_h;
                    clamp_pan(out_w, out_h, new_w, new_h, &pan_x, &pan_y);
                    break;
                }
                case SDLK_h:
                    pan_x += 80.0;
                    break;
                case SDLK_l:
                    if (!(ev.key.keysym.mod & KMOD_SHIFT))
                        pan_x -= 80.0;
                    break;
                case SDLK_j:
                    if ((ev.key.keysym.mod & KMOD_SHIFT) && content.kind == CONTENT_PDF) {
                        if (set_pdf_page(&content, content.page_index + 1)) {
                            pan_x = 0.0;
                            pan_y = 0.0;
                            update_window_title(window, &content, argv[1]);
                        }
                    } else {
                        pan_y -= 80.0;
                    }
                    break;
                case SDLK_k:
                    if ((ev.key.keysym.mod & KMOD_SHIFT) && content.kind == CONTENT_PDF) {
                        if (set_pdf_page(&content, content.page_index - 1)) {
                            pan_x = 0.0;
                            pan_y = 0.0;
                            update_window_title(window, &content, argv[1]);
                        }
                    } else {
                        pan_y += 80.0;
                    }
                    break;
                case SDLK_0:
                    zoom = 1.0;
                    pan_x = 0.0;
                    pan_y = 0.0;
                    break;
                case SDLK_r:
                    if (ev.key.keysym.mod & KMOD_SHIFT)
                        rotation = (rotation + 3) % 4;
                    else
                        rotation = (rotation + 1) % 4;
                    pan_x = 0.0;
                    pan_y = 0.0;
                    invalidate_texture(&content);
                    break;
                case SDLK_HOME:
                    if (content.kind == CONTENT_PDF && set_pdf_page(&content, 0)) {
                        pan_x = 0.0;
                        pan_y = 0.0;
                        update_window_title(window, &content, argv[1]);
                    }
                    break;
                default:
                    break;
                }
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (content.kind == CONTENT_PDF &&
                    (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                     ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
                    invalidate_texture(&content);
                }
            }
        }

        int out_w, out_h;
        if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) < 0)
            die(SDL_GetError());

        compute_display_size(&content, out_w, out_h, zoom, rotation, &display_w, &display_h);
        clamp_pan(out_w, out_h, display_w, display_h, &pan_x, &pan_y);
        ensure_texture(&content, renderer, rotation, display_w, display_h);

        SDL_Rect dst = {
            .x = (int)((double)(out_w - display_w) / 2.0 + pan_x + 0.5),
            .y = (int)((double)(out_h - display_h) / 2.0 + pan_y + 0.5),
            .w = display_w,
            .h = display_h,
        };

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, content.texture, NULL, &dst);
        SDL_RenderPresent(renderer);
        SDL_Delay(10);
    }

    destroy_content(&content);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
