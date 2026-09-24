#include "input.h"

#include "platform_sdl.h"

#include <string.h>

static int s_dup, s_ddown, s_dleft, s_dright;
static int s_sup, s_sdown, s_sleft, s_sright;
static int s_prev_sup, s_prev_sdown, s_prev_sleft, s_prev_sright;
static int s_l2[VIBE_MAX_PADS];
static int s_r2[VIBE_MAX_PADS];
static uint32_t s_repeat_at;
static VibeCmd s_repeat_cmd;
static int s_confirm_down;
static uint32_t s_confirm_at;
static int s_confirm_hold_sent;

static const int s_default_binds[VIBE_BIND_COUNT] = {
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK
};

static const VibeCmd s_bind_cmds[VIBE_BIND_COUNT] = {
    VIBE_CMD_CONFIRM,
    VIBE_CMD_BACK,
    VIBE_CMD_QUEUE_ADD,
    VIBE_CMD_SEARCH_OR_REMOVE,
    VIBE_CMD_PREV_TRACK,
    VIBE_CMD_NEXT_TRACK,
    VIBE_CMD_QUEUE,
    VIBE_CMD_LIBRARY,
    VIBE_CMD_QUEUE
};

static const char *s_bind_keys[VIBE_BIND_COUNT] = {
    "bind_confirm",
    "bind_back",
    "bind_add_queue",
    "bind_search_remove",
    "bind_prev_track",
    "bind_next_track",
    "bind_queue",
    "bind_library",
    "bind_r3"
};

static const char *s_bind_names[VIBE_BIND_COUNT] = {
    "Play / confirm",
    "Back / hide HUD",
    "Add to queue",
    "Search / queue remove",
    "Previous track",
    "Next track",
    "Now-playing queue",
    "Library",
    "Queue (alt)"
};

static int s_binds[VIBE_BIND_COUNT];

void input_init(void)
{
    s_dup = s_ddown = s_dleft = s_dright = 0;
    s_sup = s_sdown = s_sleft = s_sright = 0;
    s_prev_sup = s_prev_sdown = s_prev_sleft = s_prev_sright = 0;
    for (int i = 0; i < VIBE_MAX_PADS; i++) {
        s_l2[i] = s_r2[i] = 0;
    }
    s_repeat_at = 0;
    s_repeat_cmd = VIBE_CMD_NONE;
    s_confirm_down = 0;
    s_confirm_at = 0;
    s_confirm_hold_sent = 0;
}

void input_reset_repeat(void)
{
    s_repeat_cmd = VIBE_CMD_NONE;
}

const int *input_default_bindings(void)
{
    return s_default_binds;
}

const int *input_binds(void)
{
    return s_binds;
}

void input_apply_bindings(const int *binds)
{
    if (!binds) {
        input_reset_bindings();
        return;
    }
    memcpy(s_binds, binds, sizeof(s_binds));
}

void input_reset_bindings(void)
{
    memcpy(s_binds, s_default_binds, sizeof(s_binds));
}

void input_set_binding(VibeBind b, SDL_GameControllerButton button)
{
    if (b < 0 || b >= VIBE_BIND_COUNT) {
        return;
    }
    s_binds[b] = (int)button;
}

const char *input_bind_key(VibeBind b)
{
    if (b < 0 || b >= VIBE_BIND_COUNT) {
        return "bind_unknown";
    }
    return s_bind_keys[b];
}

const char *input_binding_name(VibeBind b)
{
    if (b < 0 || b >= VIBE_BIND_COUNT) {
        return "Unknown";
    }
    return s_bind_names[b];
}

const char *input_button_name(int button)
{
    static const struct {
        int btn;
        const char *name;
    } pretty[] = {
        { SDL_CONTROLLER_BUTTON_A, "A" },
        { SDL_CONTROLLER_BUTTON_B, "B" },
        { SDL_CONTROLLER_BUTTON_X, "X" },
        { SDL_CONTROLLER_BUTTON_Y, "Y" },
        { SDL_CONTROLLER_BUTTON_BACK, "Select" },
        { SDL_CONTROLLER_BUTTON_GUIDE, "Guide" },
        { SDL_CONTROLLER_BUTTON_START, "Start" },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3" },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3" },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L1" },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1" },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, "D-pad Up" },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, "D-pad Down" },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, "D-pad Left" },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "D-pad Right" },
    };
    if (button <= SDL_CONTROLLER_BUTTON_INVALID) {
        return "Unbound";
    }
    for (size_t i = 0; i < sizeof(pretty) / sizeof(pretty[0]); i++) {
        if (pretty[i].btn == button) {
            return pretty[i].name;
        }
    }
    {
        const char *name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)button);
        return name ? name : "Button";
    }
}

static VibeCmd nav_cmd(int up, int down, int left, int right)
{
    if (up) {
        return VIBE_CMD_UP;
    }
    if (down) {
        return VIBE_CMD_DOWN;
    }
    if (left) {
        return VIBE_CMD_LEFT;
    }
    if (right) {
        return VIBE_CMD_RIGHT;
    }
    return VIBE_CMD_NONE;
}

static void arm_repeat(VibeCmd cmd)
{
    s_repeat_cmd = cmd;
    s_repeat_at = SDL_GetTicks() + 280;
}

static int held_up(void)
{
    return s_dup || s_sup;
}

static int held_down(void)
{
    return s_ddown || s_sdown;
}

static int held_left(void)
{
    return s_dleft || s_sleft;
}

static int held_right(void)
{
    return s_dright || s_sright;
}

static VibeCmd button_to_cmd(SDL_GameControllerButton b, int down)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_GUIDE:
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return VIBE_CMD_NONE;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        if (down) {
            s_dup++;
            arm_repeat(VIBE_CMD_UP);
            return VIBE_CMD_UP;
        }
        if (s_dup > 0) {
            s_dup--;
        }
        if (s_dup == 0) {
            input_reset_repeat();
        }
        return VIBE_CMD_NONE;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        if (down) {
            s_ddown++;
            arm_repeat(VIBE_CMD_DOWN);
            return VIBE_CMD_DOWN;
        }
        if (s_ddown > 0) {
            s_ddown--;
        }
        if (s_ddown == 0) {
            input_reset_repeat();
        }
        return VIBE_CMD_NONE;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        if (down) {
            s_dleft++;
            arm_repeat(VIBE_CMD_LEFT);
            return VIBE_CMD_LEFT;
        }
        if (s_dleft > 0) {
            s_dleft--;
        }
        if (s_dleft == 0) {
            input_reset_repeat();
        }
        return VIBE_CMD_NONE;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        if (down) {
            s_dright++;
            arm_repeat(VIBE_CMD_RIGHT);
            return VIBE_CMD_RIGHT;
        }
        if (s_dright > 0) {
            s_dright--;
        }
        if (s_dright == 0) {
            input_reset_repeat();
        }
        return VIBE_CMD_NONE;
    default:
        break;
    }
    for (int i = 0; i < VIBE_BIND_COUNT; i++) {
        if (s_binds[i] == (int)b) {
            if (s_bind_cmds[i] == VIBE_CMD_CONFIRM) {
                if (down) {
                    s_confirm_down = 1;
                    s_confirm_at = SDL_GetTicks();
                    s_confirm_hold_sent = 0;
                    return VIBE_CMD_CONFIRM;
                }
                s_confirm_down = 0;
                if (!s_confirm_hold_sent) {
                    return VIBE_CMD_CONFIRM_UP;
                }
                return VIBE_CMD_NONE;
            }
            return down ? s_bind_cmds[i] : VIBE_CMD_NONE;
        }
    }
    return VIBE_CMD_NONE;
}

static VibeCmd key_to_cmd(SDL_Keycode k, int down)
{
    if (!down) {
        if (k == SDLK_RETURN || k == SDLK_SPACE) {
            s_confirm_down = 0;
            if (!s_confirm_hold_sent) {
                return VIBE_CMD_CONFIRM_UP;
            }
            return VIBE_CMD_NONE;
        }
        if (k == SDLK_UP) {
            s_dup = 0;
        }
        if (k == SDLK_DOWN) {
            s_ddown = 0;
        }
        if (k == SDLK_LEFT) {
            s_dleft = 0;
        }
        if (k == SDLK_RIGHT) {
            s_dright = 0;
        }
        input_reset_repeat();
        return VIBE_CMD_NONE;
    }
    switch (k) {
    case SDLK_RETURN:
    case SDLK_SPACE:
        s_confirm_down = 1;
        s_confirm_at = SDL_GetTicks();
        s_confirm_hold_sent = 0;
        return VIBE_CMD_CONFIRM;
    case SDLK_ESCAPE:
        return VIBE_CMD_BACK;
    case SDLK_BACKSPACE:
        return VIBE_CMD_SEARCH_BACKSPACE;
    case SDLK_DELETE:
        return VIBE_CMD_SEARCH_CLEAR;
    case SDLK_x:
        return VIBE_CMD_QUEUE_ADD;
    case SDLK_y:
        return VIBE_CMD_SEARCH_OR_REMOVE;
    case SDLK_UP:
        s_dup = 1;
        arm_repeat(VIBE_CMD_UP);
        return VIBE_CMD_UP;
    case SDLK_DOWN:
        s_ddown = 1;
        arm_repeat(VIBE_CMD_DOWN);
        return VIBE_CMD_DOWN;
    case SDLK_LEFT:
        s_dleft = 1;
        arm_repeat(VIBE_CMD_LEFT);
        return VIBE_CMD_LEFT;
    case SDLK_RIGHT:
        s_dright = 1;
        arm_repeat(VIBE_CMD_RIGHT);
        return VIBE_CMD_RIGHT;
    case SDLK_LEFTBRACKET:
        return VIBE_CMD_PRESET_PREV;
    case SDLK_RIGHTBRACKET:
        return VIBE_CMD_PRESET_NEXT;
    case SDLK_p:
        return VIBE_CMD_PREV_TRACK;
    case SDLK_n:
        return VIBE_CMD_NEXT_TRACK;
    case SDLK_AUDIOPREV:
        return VIBE_CMD_PREV_TRACK;
    case SDLK_AUDIONEXT:
        return VIBE_CMD_NEXT_TRACK;
    case SDLK_MINUS:
        return VIBE_CMD_VOL_DOWN;
    case SDLK_EQUALS:
        return VIBE_CMD_VOL_UP;
    case SDLK_TAB:
        return VIBE_CMD_LIBRARY;
    case SDLK_END:
        return VIBE_CMD_QUEUE;
    case SDLK_q:
        return VIBE_CMD_QUEUE;
    case SDLK_COMMA:
        return VIBE_CMD_SEEK_BACK;
    case SDLK_PERIOD:
        return VIBE_CMD_SEEK_FWD;
    case SDLK_PAGEUP:
        return VIBE_CMD_PAGE_UP;
    case SDLK_PAGEDOWN:
        return VIBE_CMD_PAGE_DOWN;
    case SDLK_F4:
        return VIBE_CMD_QUIT;
    default:
        return VIBE_CMD_NONE;
    }
}

int input_process_event(const SDL_Event *ev, VibeCmd *out)
{
    VibeCmd cmd = VIBE_CMD_NONE;
    if (!ev || !out) {
        return 0;
    }
    *out = VIBE_CMD_NONE;
    switch (ev->type) {
    case SDL_QUIT:
        cmd = VIBE_CMD_QUIT;
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        cmd = button_to_cmd((SDL_GameControllerButton)ev->cbutton.button, 1);
        break;
    case SDL_CONTROLLERBUTTONUP:
        cmd = button_to_cmd((SDL_GameControllerButton)ev->cbutton.button, 0);
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
            int idx = plat_pad_index(ev->caxis.which);
            int down = ev->caxis.value > 16000;
            if (idx >= 0) {
                if (down && !s_l2[idx]) {
                    cmd = VIBE_CMD_PRESET_PREV;
                }
                s_l2[idx] = down;
            }
        } else if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            int idx = plat_pad_index(ev->caxis.which);
            int down = ev->caxis.value > 16000;
            if (idx >= 0) {
                if (down && !s_r2[idx]) {
                    cmd = VIBE_CMD_PRESET_NEXT;
                }
                s_r2[idx] = down;
            }
        }
        break;
    case SDL_KEYDOWN:
        if (!ev->key.repeat) {
            cmd = key_to_cmd(ev->key.keysym.sym, 1);
        }
        break;
    case SDL_KEYUP:
        cmd = key_to_cmd(ev->key.keysym.sym, 0);
        break;
    default:
        break;
    }
    *out = cmd;
    return cmd != VIBE_CMD_NONE;
}

static void sample_left_stick(VibeCmd *rising)
{
    const float dead = 0.45f;
    *rising = VIBE_CMD_NONE;
    s_prev_sup = s_sup;
    s_prev_sdown = s_sdown;
    s_prev_sleft = s_sleft;
    s_prev_sright = s_sright;
    s_sup = s_sdown = s_sleft = s_sright = 0;
    int count = plat_pad_count();
    for (int i = 0; i < count; i++) {
        SDL_GameController *pad = plat_pad_at(i);
        if (!pad) {
            continue;
        }
        float x = (float)SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
        float y = (float)SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f;
        if (y < -dead) {
            s_sup = 1;
        } else if (y > dead) {
            s_sdown = 1;
        }
        if (x < -dead) {
            s_sleft = 1;
        } else if (x > dead) {
            s_sright = 1;
        }
    }
    /* Rising edge only if D-pad is not already holding that direction. */
    if (s_sup && !s_prev_sup && !s_dup) {
        *rising = VIBE_CMD_UP;
    } else if (s_sdown && !s_prev_sdown && !s_ddown) {
        *rising = VIBE_CMD_DOWN;
    } else if (s_sleft && !s_prev_sleft && !s_dleft) {
        *rising = VIBE_CMD_LEFT;
    } else if (s_sright && !s_prev_sright && !s_dright) {
        *rising = VIBE_CMD_RIGHT;
    }
}

int input_poll_repeat(VibeCmd *out, int *repeat)
{
    VibeCmd stick_rise = VIBE_CMD_NONE;
    uint32_t now;
    if (repeat) {
        *repeat = 0;
    }
    if (!out) {
        return 0;
    }
    *out = VIBE_CMD_NONE;
    now = SDL_GetTicks();
    if (s_confirm_down && !s_confirm_hold_sent && now - s_confirm_at >= 500) {
        s_confirm_hold_sent = 1;
        *out = VIBE_CMD_CONFIRM_HOLD;
        return 1;
    }
    sample_left_stick(&stick_rise);
    if (stick_rise != VIBE_CMD_NONE) {
        arm_repeat(stick_rise);
        *out = stick_rise;
        return 1;
    }
    if (!held_up() && !held_down() && !held_left() && !held_right()) {
        s_repeat_cmd = VIBE_CMD_NONE;
        return 0;
    }
    if (s_repeat_cmd == VIBE_CMD_NONE) {
        return 0;
    }
    now = SDL_GetTicks();
    if (now < s_repeat_at) {
        return 0;
    }
    s_repeat_at = now + 70;
    *out = nav_cmd(held_up(), held_down(), held_left(), held_right());
    if (*out != VIBE_CMD_NONE && repeat) {
        *repeat = 1;
    }
    return *out != VIBE_CMD_NONE;
}

void input_axes(float *seek_x, float *page_y)
{
    float x = 0.f, y = 0.f;
    const float dead = 0.35f;
    int count = plat_pad_count();
    for (int i = 0; i < count; i++) {
        SDL_GameController *pad = plat_pad_at(i);
        if (!pad) {
            continue;
        }
        x += (float)SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f;
        y += (float)SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
    }
    if (x > 1.f) {
        x = 1.f;
    }
    if (x < -1.f) {
        x = -1.f;
    }
    if (y > 1.f) {
        y = 1.f;
    }
    if (y < -1.f) {
        y = -1.f;
    }
    if (x < dead && x > -dead) {
        x = 0.f;
    }
    if (y < dead && y > -dead) {
        y = 0.f;
    }
    if (seek_x) {
        *seek_x = x;
    }
    if (page_y) {
        *page_y = y;
    }
}
