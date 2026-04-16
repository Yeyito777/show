#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

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
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        win_w,
        win_h,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS
    );
    if (!window)
        die(SDL_GetError());

    set_wm_class(window, "show");

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
                        }
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
