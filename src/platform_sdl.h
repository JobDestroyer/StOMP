#ifndef VIBE_PLATFORM_SDL_H
#define VIBE_PLATFORM_SDL_H

#include <SDL.h>

#define GL_GLEXT_PROTOTYPES 0
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

/* GL 1.1 symbols come from libGL. Load 2.0+ entry points ourselves. */
#define VIBE_GL_FNS \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)

#define X(t, n) extern t vibe_##n;
VIBE_GL_FNS
#undef X

#define glCreateShader vibe_glCreateShader
#define glShaderSource vibe_glShaderSource
#define glCompileShader vibe_glCompileShader
#define glGetShaderiv vibe_glGetShaderiv
#define glGetShaderInfoLog vibe_glGetShaderInfoLog
#define glCreateProgram vibe_glCreateProgram
#define glAttachShader vibe_glAttachShader
#define glLinkProgram vibe_glLinkProgram
#define glGetProgramiv vibe_glGetProgramiv
#define glGetProgramInfoLog vibe_glGetProgramInfoLog
#define glUseProgram vibe_glUseProgram
#define glDeleteShader vibe_glDeleteShader
#define glDeleteProgram vibe_glDeleteProgram
#define glGenBuffers vibe_glGenBuffers
#define glBindBuffer vibe_glBindBuffer
#define glBufferData vibe_glBufferData
#define glBufferSubData vibe_glBufferSubData
#define glDeleteBuffers vibe_glDeleteBuffers
#define glGenVertexArrays vibe_glGenVertexArrays
#define glBindVertexArray vibe_glBindVertexArray
#define glDeleteVertexArrays vibe_glDeleteVertexArrays
#define glEnableVertexAttribArray vibe_glEnableVertexAttribArray
#define glVertexAttribPointer vibe_glVertexAttribPointer
#define glGetUniformLocation vibe_glGetUniformLocation
#define glUniform1i vibe_glUniform1i
#define glUniform2f vibe_glUniform2f
#define glUniform4f vibe_glUniform4f
#define glActiveTexture vibe_glActiveTexture
#define glGenFramebuffers vibe_glGenFramebuffers
#define glBindFramebuffer vibe_glBindFramebuffer
#define glFramebufferTexture2D vibe_glFramebufferTexture2D
#define glCheckFramebufferStatus vibe_glCheckFramebufferStatus
#define glDeleteFramebuffers vibe_glDeleteFramebuffers
#define glBindAttribLocation vibe_glBindAttribLocation

int plat_init(int windowed, int win_w, int win_h);
void plat_shutdown(void);
SDL_Window *plat_window(void);
SDL_GLContext plat_gl(void);
int plat_poll(SDL_Event *ev);
void plat_swap(void);
void plat_size(int *w, int *h);
void plat_drawable_size(int *w, int *h);
int plat_display_h(void);
int plat_on_battery(void);
#define VIBE_MAX_PADS 8

SDL_GameController *plat_pad(void);
int plat_pad_count(void);
SDL_GameController *plat_pad_at(int i);
int plat_pad_index(SDL_JoystickID id);
void plat_pad_added(int device_index);
void plat_pad_removed(SDL_JoystickID device_index);
void plat_make_current(void);
void plat_destroy_gl(void);
int plat_recreate_gl(void);
int plat_gl_load(void);
const char *plat_base_path(void);

#endif
