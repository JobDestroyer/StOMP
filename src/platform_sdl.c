#include "platform_sdl.h"

#include <stdio.h>
#include <string.h>

#define X(t, n) t vibe_##n = NULL;
VIBE_GL_FNS
#undef X

static SDL_Window *s_win;
static SDL_GLContext s_gl;
static SDL_GameController *s_pads[VIBE_MAX_PADS];
static int s_pad_count;
static int s_win_w = 1920;
static int s_win_h = 1080;
static int s_windowed;
static char s_base[1024];

static void open_all_pads(void)
{
    int n = SDL_NumJoysticks();
    s_pad_count = 0;
    for (int i = 0; i < n && s_pad_count < VIBE_MAX_PADS; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController *pad = SDL_GameControllerOpen(i);
            if (pad) {
                s_pads[s_pad_count++] = pad;
                fprintf(stderr, "StOMP: controller %s\n", SDL_GameControllerName(pad));
            } else {
                fprintf(stderr, "StOMP: SDL_GameControllerOpen(%d): %s\n", i, SDL_GetError());
            }
        }
    }
}

int plat_gl_load(void)
{
#define X(t, n) \
    vibe_##n = (t)SDL_GL_GetProcAddress(#n); \
    if (!vibe_##n) { \
        fprintf(stderr, "StOMP: missing GL function %s\n", #n); \
        return -1; \
    }
    VIBE_GL_FNS
#undef X
    return 0;
}

static int create_window_and_gl(void)
{
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!s_windowed) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    } else {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    s_win = SDL_CreateWindow("StOMP",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             s_win_w, s_win_h, flags);
    if (!s_win) {
        fprintf(stderr, "StOMP: SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    s_gl = SDL_GL_CreateContext(s_win);
    if (!s_gl) {
        fprintf(stderr, "StOMP: GL 3.3 core failed (%s), trying compatibility\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        s_gl = SDL_GL_CreateContext(s_win);
    }
    if (!s_gl) {
        fprintf(stderr, "StOMP: GL 3.3 compatibility failed (%s), trying 3.2 core\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        s_gl = SDL_GL_CreateContext(s_win);
    }
    if (!s_gl) {
        fprintf(stderr, "StOMP: SDL_GL_CreateContext: %s\n", SDL_GetError());
        return -1;
    }
    if (SDL_GL_MakeCurrent(s_win, s_gl) != 0) {
        fprintf(stderr, "StOMP: SDL_GL_MakeCurrent: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GL_SetSwapInterval(0);
    SDL_GetWindowSize(s_win, &s_win_w, &s_win_h);
    if (plat_gl_load() != 0) {
        return -1;
    }
    {
        int dw = 0, dh = 0;
        const char *ver = (const char *)glGetString(GL_VERSION);
        const char *ren = (const char *)glGetString(GL_RENDERER);
        plat_drawable_size(&dw, &dh);
        fprintf(stderr, "StOMP: GL %s / %s\n", ver ? ver : "?", ren ? ren : "?");
        fprintf(stderr, "StOMP: window %dx%d drawable %dx%d\n", s_win_w, s_win_h, dw, dh);
    }
    return 0;
}

int plat_init(int windowed, int win_w, int win_h)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "StOMP: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GameControllerAddMappingsFromFile("/usr/share/sdl/gamecontrollerdb.txt");
    s_windowed = windowed;
    if (win_w > 0) {
        s_win_w = win_w;
    }
    if (win_h > 0) {
        s_win_h = win_h;
    }
    {
        char *base = SDL_GetBasePath();
        if (base) {
            snprintf(s_base, sizeof(s_base), "%s", base);
            SDL_free(base);
        } else {
            s_base[0] = '\0';
        }
    }
    if (create_window_and_gl() != 0) {
        return -1;
    }
    open_all_pads();
    return 0;
}

void plat_shutdown(void)
{
    for (int i = 0; i < s_pad_count; i++) {
        SDL_GameControllerClose(s_pads[i]);
        s_pads[i] = NULL;
    }
    s_pad_count = 0;
    if (s_gl) {
        SDL_GL_DeleteContext(s_gl);
        s_gl = NULL;
    }
    if (s_win) {
        SDL_DestroyWindow(s_win);
        s_win = NULL;
    }
    SDL_Quit();
}

SDL_Window *plat_window(void)
{
    return s_win;
}

SDL_GLContext plat_gl(void)
{
    return s_gl;
}

int plat_poll(SDL_Event *ev)
{
    return SDL_PollEvent(ev);
}

void plat_swap(void)
{
    SDL_GL_SwapWindow(s_win);
}

void plat_size(int *w, int *h)
{
    if (s_win) {
        SDL_GetWindowSize(s_win, &s_win_w, &s_win_h);
    }
    if (w) {
        *w = s_win_w;
    }
    if (h) {
        *h = s_win_h;
    }
}

void plat_drawable_size(int *w, int *h)
{
    int dw = 0, dh = 0;
    if (s_win) {
        SDL_GL_GetDrawableSize(s_win, &dw, &dh);
    }
    if (dw <= 0 || dh <= 0) {
        plat_size(&dw, &dh);
    }
    if (w) {
        *w = dw;
    }
    if (h) {
        *h = dh;
    }
}

int plat_display_h(void)
{
    SDL_DisplayMode m;
    int idx = s_win ? SDL_GetWindowDisplayIndex(s_win) : 0;
    if (idx < 0) {
        idx = 0;
    }
    if (SDL_GetDesktopDisplayMode(idx, &m) != 0) {
        return s_win_h;
    }
    return m.h;
}

int plat_on_battery(void)
{
    int secs = 0, pct = 0;
    SDL_PowerState st = SDL_GetPowerInfo(&secs, &pct);
    return st == SDL_POWERSTATE_ON_BATTERY;
}

SDL_GameController *plat_pad(void)
{
    return s_pad_count > 0 ? s_pads[0] : NULL;
}

int plat_pad_count(void)
{
    return s_pad_count;
}

SDL_GameController *plat_pad_at(int i)
{
    if (i < 0 || i >= s_pad_count) {
        return NULL;
    }
    return s_pads[i];
}

int plat_pad_index(SDL_JoystickID id)
{
    SDL_GameController *pad = SDL_GameControllerFromInstanceID(id);
    if (!pad) {
        return -1;
    }
    for (int i = 0; i < s_pad_count; i++) {
        if (s_pads[i] == pad) {
            return i;
        }
    }
    return -1;
}

void plat_pad_added(int device_index)
{
    if (s_pad_count >= VIBE_MAX_PADS) {
        return;
    }
    if (!SDL_IsGameController(device_index)) {
        return;
    }
    SDL_GameController *pad = SDL_GameControllerOpen(device_index);
    if (pad) {
        s_pads[s_pad_count++] = pad;
        fprintf(stderr, "StOMP: controller added: %s\n", SDL_GameControllerName(pad));
    }
}

void plat_pad_removed(SDL_JoystickID device_index)
{
    for (int i = 0; i < s_pad_count; i++) {
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(s_pads[i]);
        if (joy && SDL_JoystickInstanceID(joy) == device_index) {
            fprintf(stderr, "StOMP: controller removed: %s\n", SDL_GameControllerName(s_pads[i]));
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = s_pads[--s_pad_count];
            s_pads[s_pad_count] = NULL;
            break;
        }
    }
}

void plat_make_current(void)
{
    if (s_win && s_gl) {
        SDL_GL_MakeCurrent(s_win, s_gl);
    }
}

void plat_destroy_gl(void)
{
    if (s_gl) {
        SDL_GL_DeleteContext(s_gl);
        s_gl = NULL;
    }
}

int plat_recreate_gl(void)
{
    if (!s_win) {
        return -1;
    }
    s_gl = SDL_GL_CreateContext(s_win);
    if (!s_gl) {
        fprintf(stderr, "StOMP: recreate GL: %s\n", SDL_GetError());
        return -1;
    }
    if (SDL_GL_MakeCurrent(s_win, s_gl) != 0) {
        fprintf(stderr, "StOMP: recreate MakeCurrent: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GL_SetSwapInterval(0);
    return plat_gl_load();
}

const char *plat_base_path(void)
{
    return s_base;
}
