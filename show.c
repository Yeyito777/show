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
    int content_w;
    int content_h;
    int rendered_page;
    int rendered_out_w;
    int rendered_out_h;
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

static SDL_Rect fit_rect(int src_w, int src_h, int dst_w, int dst_h)
{
    double scale_w = (double)dst_w / (double)src_w;
    double scale_h = (double)dst_h / (double)src_h;
    double scale = scale_w < scale_h ? scale_w : scale_h;

    int w = (int)(src_w * scale + 0.5);
    int h = (int)(src_h * scale + 0.5);

    SDL_Rect rect = {
        .x = (dst_w - w) / 2,
        .y = (dst_h - h) / 2,
        .w = w,
        .h = h,
    };
    return rect;
}

static bool has_pdf_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".pdf") == 0;
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

static void destroy_texture(Content *content)
{
    if (content->texture) {
        SDL_DestroyTexture(content->texture);
        content->texture = NULL;
    }
}

static void destroy_content(Content *content)
{
    destroy_texture(content);
    if (content->image_surface)
        SDL_FreeSurface(content->image_surface);
    if (content->pdf_doc)
        g_object_unref(content->pdf_doc);
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

static bool load_content(const char *path, Content *content)
{
    memset(content, 0, sizeof(*content));
    content->rendered_page = -1;
    content->rendered_out_w = -1;
    content->rendered_out_h = -1;

    if (!has_pdf_extension(path)) {
        content->image_surface = IMG_Load(path);
        if (content->image_surface) {
            content->kind = CONTENT_IMAGE;
            content->content_w = content->image_surface->w;
            content->content_h = content->image_surface->h;
            return true;
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
                          &content->content_w, &content->content_h);
        return true;
    }

    if (has_pdf_extension(path))
        die("cannot open PDF");
    die(IMG_GetError());
    return false;
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

static void create_image_texture(Content *content, SDL_Renderer *renderer)
{
    destroy_texture(content);
    content->texture = SDL_CreateTextureFromSurface(renderer, content->image_surface);
    if (!content->texture)
        die(SDL_GetError());
    content->rendered_out_w = content->content_w;
    content->rendered_out_h = content->content_h;
    content->rendered_page = -1;
}

static void render_pdf_page(Content *content, SDL_Renderer *renderer, int out_w, int out_h)
{
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    if (content->texture &&
        content->rendered_page == content->page_index &&
        content->rendered_out_w == out_w &&
        content->rendered_out_h == out_h)
        return;

    PopplerPage *page = poppler_document_get_page(content->pdf_doc, content->page_index);
    if (!page)
        die("cannot render PDF page");

    double page_w, page_h;
    poppler_page_get_size(page, &page_w, &page_h);
    if (page_w < 1.0) page_w = 1.0;
    if (page_h < 1.0) page_h = 1.0;

    double scale_w = (double)out_w / page_w;
    double scale_h = (double)out_h / page_h;
    double scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale <= 0.0) scale = 1.0;

    int render_w = (int)(page_w * scale + 0.5);
    int render_h = (int)(page_h * scale + 0.5);
    if (render_w < 1) render_w = 1;
    if (render_h < 1) render_h = 1;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          render_w, render_h);
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    SDL_Surface *sdl_surface = SDL_CreateRGBSurfaceWithFormatFrom(
        cairo_image_surface_get_data(surface),
        render_w,
        render_h,
        32,
        cairo_image_surface_get_stride(surface),
        SDL_PIXELFORMAT_BGRA32
    );
    if (!sdl_surface)
        die(SDL_GetError());

    destroy_texture(content);
    content->texture = SDL_CreateTextureFromSurface(renderer, sdl_surface);
    SDL_FreeSurface(sdl_surface);
    cairo_surface_destroy(surface);
    g_object_unref(page);

    if (!content->texture)
        die(SDL_GetError());

    content->content_w = render_w;
    content->content_h = render_h;
    content->rendered_page = content->page_index;
    content->rendered_out_w = out_w;
    content->rendered_out_h = out_h;
}

static void ensure_texture(Content *content, SDL_Renderer *renderer, int out_w, int out_h)
{
    if (content->kind == CONTENT_IMAGE) {
        if (!content->texture)
            create_image_texture(content, renderer);
        return;
    }

    render_pdf_page(content, renderer, out_w, out_h);
}

static bool change_pdf_page(Content *content, int delta)
{
    if (content->kind != CONTENT_PDF)
        return false;

    int next = content->page_index + delta;
    if (next < 0 || next >= content->page_count)
        return false;

    content->page_index = next;
    content->rendered_page = -1;
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

    int natural_w = content.content_w;
    int natural_h = content.content_h;
    if (natural_w <= 0 || natural_h <= 0)
        die("invalid dimensions");

    double scale_w = (double)max_w / (double)natural_w;
    double scale_h = (double)max_h / (double)natural_h;
    double scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale > 1.0) scale = 1.0;

    int win_w = (int)(natural_w * scale + 0.5);
    int win_h = (int)(natural_h * scale + 0.5);
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

    ensure_texture(&content, renderer, win_w, win_h);
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
                            if (content.kind == CONTENT_PDF)
                                content.rendered_page = -1;
                        }
                    }
                    break;
                case SDLK_RIGHT:
                case SDLK_l:
                case SDLK_PAGEDOWN:
                case SDLK_SPACE:
                    if (change_pdf_page(&content, +1))
                        update_window_title(window, &content, argv[1]);
                    break;
                case SDLK_LEFT:
                case SDLK_h:
                case SDLK_PAGEUP:
                    if (change_pdf_page(&content, -1))
                        update_window_title(window, &content, argv[1]);
                    break;
                case SDLK_0:
                case SDLK_HOME:
                    if (content.kind == CONTENT_PDF && content.page_index != 0) {
                        content.page_index = 0;
                        content.rendered_page = -1;
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
                    content.rendered_page = -1;
                }
            }
        }

        int out_w, out_h;
        if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) < 0)
            die(SDL_GetError());

        ensure_texture(&content, renderer, out_w, out_h);
        SDL_Rect dst = fit_rect(content.content_w, content.content_h, out_w, out_h);

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
