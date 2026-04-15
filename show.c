#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>

static void die(const char *msg)
{
    fprintf(stderr, "show: %s\n", msg);
    exit(1);
}

static void set_override_redirect(SDL_Window *window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return;
    if (info.subsystem != SDL_SYSWM_X11) return;

    Display *dpy = info.info.x11.display;
    Window xw = info.info.x11.window;

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    XChangeWindowAttributes(dpy, xw, CWOverrideRedirect, &attrs);
    XFlush(dpy);
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

    SDL_Surface *surface = IMG_Load(argv[1]);
    if (!surface)
        die(IMG_GetError());

    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) < 0)
        die(SDL_GetError());

    const int margin = 64;
    int max_w = dm.w - margin;
    int max_h = dm.h - margin;
    if (max_w < 1) max_w = dm.w;
    if (max_h < 1) max_h = dm.h;

    int img_w = surface->w;
    int img_h = surface->h;
    if (img_w <= 0 || img_h <= 0)
        die("invalid image dimensions");

    double scale_w = (double)max_w / (double)img_w;
    double scale_h = (double)max_h / (double)img_h;
    double scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale > 1.0) scale = 1.0;

    int win_w = (int)(img_w * scale + 0.5);
    int win_h = (int)(img_h * scale + 0.5);
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;

    SDL_Window *window = SDL_CreateWindow(
        "show",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        win_w,
        win_h,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS
    );
    if (!window)
        die(SDL_GetError());

    set_override_redirect(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

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

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture)
        die(SDL_GetError());

    SDL_FreeSurface(surface);

    bool quit = false;
    bool fullscreen = false;
    int saved_x = SDL_WINDOWPOS_CENTERED;
    int saved_y = SDL_WINDOWPOS_CENTERED;
    int saved_w = win_w;
    int saved_h = win_h;

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
                        SDL_GetWindowPosition(window, &saved_x, &saved_y);
                        SDL_GetWindowSize(window, &saved_w, &saved_h);

                        SDL_Rect bounds;
                        if (SDL_GetDisplayBounds(0, &bounds) == 0) {
                            SDL_SetWindowPosition(window, bounds.x, bounds.y);
                            SDL_SetWindowSize(window, bounds.w, bounds.h);
                            set_override_redirect(window);
                            SDL_RaiseWindow(window);
                            fullscreen = true;
                        }
                    } else {
                        SDL_SetWindowSize(window, saved_w, saved_h);
                        SDL_SetWindowPosition(window, saved_x, saved_y);
                        set_override_redirect(window);
                        SDL_RaiseWindow(window);
                        fullscreen = false;
                    }
                    break;
                default:
                    break;
                }
            }
        }

        int out_w, out_h;
        if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) < 0)
            die(SDL_GetError());

        SDL_Rect dst = fit_rect(img_w, img_h, out_w, out_h);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        SDL_RenderPresent(renderer);
        SDL_Delay(10);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
