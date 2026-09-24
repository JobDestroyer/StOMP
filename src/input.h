#ifndef VIBE_INPUT_H
#define VIBE_INPUT_H

#include <SDL.h>

#include "config.h"

typedef enum {
    VIBE_CMD_NONE = 0,
    VIBE_CMD_CONFIRM,
    VIBE_CMD_CONFIRM_UP,
    VIBE_CMD_CONFIRM_HOLD,
    VIBE_CMD_BACK,
    VIBE_CMD_QUEUE_ADD,
    VIBE_CMD_SEARCH_OR_REMOVE,
    VIBE_CMD_SEARCH_BACKSPACE,
    VIBE_CMD_SEARCH_CLEAR,
    VIBE_CMD_UP,
    VIBE_CMD_DOWN,
    VIBE_CMD_LEFT,
    VIBE_CMD_RIGHT,
    VIBE_CMD_PREV_TRACK,
    VIBE_CMD_NEXT_TRACK,
    VIBE_CMD_VOL_DOWN,
    VIBE_CMD_VOL_UP,
    VIBE_CMD_LIBRARY,
    VIBE_CMD_STOP,
    VIBE_CMD_QUEUE,
    VIBE_CMD_PRESET_NEXT,
    VIBE_CMD_PRESET_PREV,
    VIBE_CMD_PAGE_UP,
    VIBE_CMD_PAGE_DOWN,
    VIBE_CMD_SEEK_BACK,
    VIBE_CMD_SEEK_FWD,
    VIBE_CMD_QUIT
} VibeCmd;

void input_init(void);
void input_reset_repeat(void);
int input_process_event(const SDL_Event *ev, VibeCmd *out);
int input_poll_repeat(VibeCmd *out, int *repeat);
void input_axes(float *seek_x, float *page_y);

/* Controller button layout. The user may remap each bindable action to any
 * gamepad button; bindings persist in vibe.conf. */
const int *input_default_bindings(void);
const int *input_binds(void);
void input_apply_bindings(const int *binds);
void input_reset_bindings(void);
void input_set_binding(VibeBind b, SDL_GameControllerButton button);
const char *input_bind_key(VibeBind b);
const char *input_binding_name(VibeBind b);
const char *input_button_name(int button);

#endif
