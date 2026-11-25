/* sw_window.c - Win32 window + OpenGL rendering context for standalone runtime.
 *
 * Runs on a separate thread with its own message pump so the window
 * stays responsive even when the game thread is busy.
 *
 * Graphics: OpenGL via Windows' built-in opengl32.dll (wgl). No external
 * dependencies required — OpenGL ships with Windows.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

/* WGL_ARB_create_context extension constants - not in Windows SDK headers */
#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB             0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB             0x2092
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB              0x9126
#endif
#ifndef WGL_CONTEXT_CORE_PROFILE_BIT_ARB
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB          0x00000001
#endif

#ifdef USE_SDL_CONTROLLER
#include <SDL.h>
#endif

typedef BOOL (WINAPI *wglSwapBuffers_t)(HDC);
typedef HGLRC (WINAPI *wglCreateContext_t)(HDC);
typedef BOOL (WINAPI *wglMakeCurrent_t)(HDC, HGLRC);
typedef BOOL (WINAPI *wglDeleteContext_t)(HGLRC);
typedef PROC (WINAPI *wglGetProcAddress_t)(LPCSTR);
typedef HGLRC (WINAPI *wglCreateContextAttribsARB_t)(HDC, HGLRC, const int *);

typedef void (APIENTRY *glClearColor_t)(float, float, float, float);
typedef void (APIENTRY *glClear_t)(unsigned int);
typedef void (APIENTRY *glViewport_t)(int, int, unsigned int, unsigned int);
typedef void (APIENTRY *glEnable_t)(unsigned int);
typedef void (APIENTRY *glDisable_t)(unsigned int);
typedef void (APIENTRY *glDrawArrays_t)(unsigned int, int, int);
typedef void (APIENTRY *glVertexAttribPointer_t)(unsigned int, int, unsigned int, int, int, const void *);
typedef void (APIENTRY *glEnableVertexAttribArray_t)(unsigned int);
typedef void (APIENTRY *glUseProgram_t)(unsigned int);
typedef unsigned int (APIENTRY *glCreateProgram_t)(void);
typedef void (APIENTRY *glDeleteProgram_t)(unsigned int);
typedef unsigned int (APIENTRY *glCreateShader_t)(unsigned int);
typedef void (APIENTRY *glDeleteShader_t)(unsigned int);
typedef void (APIENTRY *glShaderSource_t)(unsigned int, int, const char *const *, const int *);
typedef void (APIENTRY *glCompileShader_t)(unsigned int);
typedef void (APIENTRY *glAttachShader_t)(unsigned int, unsigned int);
typedef void (APIENTRY *glLinkProgram_t)(unsigned int);
typedef void (APIENTRY *glGetShaderiv_t)(unsigned int, unsigned int, int *);
typedef void (APIENTRY *glGetShaderInfoLog_t)(unsigned int, int, int *, char *);
typedef void (APIENTRY *glGetProgramiv_t)(unsigned int, unsigned int, int *);
typedef void (APIENTRY *glGetProgramInfoLog_t)(unsigned int, int, int *, char *);
typedef void (APIENTRY *glGenBuffers_t)(int, unsigned int *);
typedef void (APIENTRY *glBindBuffer_t)(unsigned int, unsigned int);
typedef void (APIENTRY *glBufferData_t)(unsigned int, int, const void *, unsigned int);
typedef void (APIENTRY *glGenVertexArrays_t)(int, unsigned int *);
typedef void (APIENTRY *glBindVertexArray_t)(unsigned int);
typedef void (APIENTRY *glDeleteBuffers_t)(int, const unsigned int *);
typedef void (APIENTRY *glDeleteVertexArrays_t)(int, const unsigned int *);
typedef const unsigned char *(APIENTRY *glGetString_t)(unsigned int);
typedef void (APIENTRY *glGetIntegerv_t)(unsigned int, int *);

static wglSwapBuffers_t        p_wglSwapBuffers        = NULL;
static wglCreateContext_t      p_wglCreateContext      = NULL;
static wglMakeCurrent_t        p_wglMakeCurrent        = NULL;
static wglDeleteContext_t      p_wglDeleteContext      = NULL;
static wglGetProcAddress_t     p_wglGetProcAddress     = NULL;

static glClearColor_t          p_glClearColor          = NULL;
static glClear_t               p_glClear               = NULL;
static glViewport_t            p_glViewport            = NULL;
static glEnable_t              p_glEnable              = NULL;
static glDisable_t             p_glDisable             = NULL;
static glDrawArrays_t          p_glDrawArrays          = NULL;
static glVertexAttribPointer_t p_glVertexAttribPointer = NULL;
static glEnableVertexAttribArray_t p_glEnableVertexAttribArray = NULL;
static glUseProgram_t          p_glUseProgram          = NULL;
static glCreateProgram_t       p_glCreateProgram       = NULL;
static glDeleteProgram_t       p_glDeleteProgram       = NULL;
static glCreateShader_t        p_glCreateShader        = NULL;
static glDeleteShader_t        p_glDeleteShader        = NULL;
static glShaderSource_t        p_glShaderSource        = NULL;
static glCompileShader_t       p_glCompileShader       = NULL;
static glAttachShader_t        p_glAttachShader        = NULL;
static glLinkProgram_t         p_glLinkProgram         = NULL;
static glGetShaderiv_t         p_glGetShaderiv         = NULL;
static glGetShaderInfoLog_t    p_glGetShaderInfoLog    = NULL;
static glGetProgramiv_t        p_glGetProgramiv        = NULL;
static glGetProgramInfoLog_t   p_glGetProgramInfoLog   = NULL;
static glGenBuffers_t          p_glGenBuffers          = NULL;
static glBindBuffer_t          p_glBindBuffer          = NULL;
static glBufferData_t          p_glBufferData          = NULL;
static glGenVertexArrays_t     p_glGenVertexArrays     = NULL;
static glBindVertexArray_t     p_glBindVertexArray     = NULL;
static glDeleteBuffers_t       p_glDeleteBuffers       = NULL;
static glDeleteVertexArrays_t  p_glDeleteVertexArrays  = NULL;
static wglCreateContextAttribsARB_t p_wglCreateContextAttribsARB = NULL;
static glGetString_t           p_glGetString           = NULL;
static glGetIntegerv_t         p_glGetIntegerv         = NULL;

static HWND g_hwnd = NULL;
static HDC  g_hdc = NULL;
static HGLRC g_hrc = NULL;
static int  g_window_width = 1280;
static int  g_window_height = 720;
static volatile int  g_window_open = 0;
static volatile int  g_window_should_close = 0;
static HANDLE g_window_thread = NULL;
static DWORD  g_window_thread_id = 0;
static wchar_t g_window_class[] = L"SwWindowClass";

/* Written by the game thread, read by the window thread each frame. */
static volatile float g_clear_r = 0.1f;
static volatile float g_clear_g = 0.1f;
static volatile float g_clear_b = 0.15f;
static volatile float g_clear_a = 1.0f;
static volatile LONG g_present_count = 0;
static volatile int g_test_pattern = 0;

volatile int g_fps_mode = 0;  /* 0=off/auto, 30, 60 */
static volatile int g_flash_frames = 0;
static float g_flash_saved_r = 0.1f, g_flash_saved_g = 0.1f, g_flash_saved_b = 0.15f;

static unsigned int g_shader_program = 0;
static unsigned int g_vao = 0;
static unsigned int g_vbo = 0;
static int g_gfx_ready = 0;

/* Forward declarations for functions called before definition. */
void sw_window_update_flash(void);

/* ------------------------------------------------------------------ */
/* Audio (Windows winmm.lib waveOut)                                  */
/* ------------------------------------------------------------------ */

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_CHANNELS    2
#define AUDIO_BITS        16

static HWAVEOUT g_waveOut = NULL;
static WAVEHDR  g_waveHeaders[2];
static short    g_audioBuffers[2][AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8) / 10];
static int      g_audioBufferIndex = 0;
static int      g_audioInitialized = 0;

/* Forward declarations for audio functions */
static int audio_init(void);
static void audio_shutdown(void);

/* ------------------------------------------------------------------ */
/* SDL Controller Support                                             */
/* ------------------------------------------------------------------ */

#ifdef USE_SDL_CONTROLLER
static SDL_GameController *g_controller = NULL;
#endif

/* ------------------------------------------------------------------ */
/* OpenGL function loading                                            */
/* ------------------------------------------------------------------ */

static void *load_gl_proc(const char *name)
{
    void *proc = NULL;
    if (p_wglGetProcAddress) {
        proc = (void *)p_wglGetProcAddress(name);
    }
    if (!proc) {
        HMODULE h = LoadLibraryA("opengl32.dll");
        if (h) {
            proc = (void *)GetProcAddress(h, name);
        }
    }
    return proc;
}

static int load_wgl_functions(void)
{
    p_wglCreateContext         = (wglCreateContext_t)         load_gl_proc("wglCreateContext");
    p_wglMakeCurrent           = (wglMakeCurrent_t)           load_gl_proc("wglMakeCurrent");
    p_wglDeleteContext         = (wglDeleteContext_t)         load_gl_proc("wglDeleteContext");
    p_wglGetProcAddress        = (wglGetProcAddress_t)        load_gl_proc("wglGetProcAddress");
    p_wglSwapBuffers           = (wglSwapBuffers_t)           load_gl_proc("wglSwapBuffers");
    p_wglCreateContextAttribsARB = (wglCreateContextAttribsARB_t)load_gl_proc("wglCreateContextAttribsARB");

    if (!p_wglCreateContext || !p_wglMakeCurrent || !p_wglSwapBuffers || !p_wglGetProcAddress) {
        return -1;
    }
    return 0;
}

/* Load GL entry points via wglGetProcAddress — requires a current OpenGL context.
 * Called after create_opengl_context() has made a context current. */
static int load_gl_functions(void)
{
    p_glClearColor             = (glClearColor_t)             load_gl_proc("glClearColor");
    p_glClear                  = (glClear_t)                  load_gl_proc("glClear");
    p_glViewport               = (glViewport_t)               load_gl_proc("glViewport");
    p_glEnable                 = (glEnable_t)                 load_gl_proc("glEnable");
    p_glDisable                = (glDisable_t)                load_gl_proc("glDisable");
    p_glDrawArrays             = (glDrawArrays_t)             load_gl_proc("glDrawArrays");
    p_glVertexAttribPointer    = (glVertexAttribPointer_t)    load_gl_proc("glVertexAttribPointer");
    p_glEnableVertexAttribArray= (glEnableVertexAttribArray_t)load_gl_proc("glEnableVertexAttribArray");
    p_glUseProgram             = (glUseProgram_t)             load_gl_proc("glUseProgram");
    p_glCreateProgram          = (glCreateProgram_t)          load_gl_proc("glCreateProgram");
    p_glDeleteProgram          = (glDeleteProgram_t)          load_gl_proc("glDeleteProgram");
    p_glCreateShader           = (glCreateShader_t)           load_gl_proc("glCreateShader");
    p_glDeleteShader           = (glDeleteShader_t)           load_gl_proc("glDeleteShader");
    p_glShaderSource           = (glShaderSource_t)           load_gl_proc("glShaderSource");
    p_glCompileShader          = (glCompileShader_t)          load_gl_proc("glCompileShader");
    p_glAttachShader           = (glAttachShader_t)           load_gl_proc("glAttachShader");
    p_glLinkProgram            = (glLinkProgram_t)            load_gl_proc("glLinkProgram");
    p_glGetShaderiv            = (glGetShaderiv_t)            load_gl_proc("glGetShaderiv");
    p_glGetShaderInfoLog       = (glGetShaderInfoLog_t)       load_gl_proc("glGetShaderInfoLog");
    p_glGetProgramiv           = (glGetProgramiv_t)           load_gl_proc("glGetProgramiv");
    p_glGetProgramInfoLog      = (glGetProgramInfoLog_t)      load_gl_proc("glGetProgramInfoLog");
    p_glGenBuffers             = (glGenBuffers_t)             load_gl_proc("glGenBuffers");
    p_glBindBuffer             = (glBindBuffer_t)             load_gl_proc("glBindBuffer");
    p_glBufferData             = (glBufferData_t)             load_gl_proc("glBufferData");
    p_glGenVertexArrays        = (glGenVertexArrays_t)        load_gl_proc("glGenVertexArrays");
    p_glBindVertexArray        = (glBindVertexArray_t)        load_gl_proc("glBindVertexArray");
    p_glDeleteBuffers          = (glDeleteBuffers_t)          load_gl_proc("glDeleteBuffers");
    p_glDeleteVertexArrays     = (glDeleteVertexArrays_t)     load_gl_proc("glDeleteVertexArrays");
    p_glGetString              = (glGetString_t)              load_gl_proc("glGetString");
    p_glGetIntegerv            = (glGetIntegerv_t)            load_gl_proc("glGetIntegerv");

    if (!p_glClearColor || !p_glClear || !p_glViewport || !p_glCreateProgram ||
        !p_glCreateShader || !p_glShaderSource || !p_glCompileShader ||
        !p_glAttachShader || !p_glLinkProgram || !p_glGenBuffers ||
        !p_glBindBuffer || !p_glBufferData || !p_glGenVertexArrays ||
        !p_glBindVertexArray || !p_glDeleteBuffers || !p_glDeleteVertexArrays || !p_glVertexAttribPointer ||
        !p_glEnableVertexAttribArray || !p_glUseProgram ||
        !p_glGetString || !p_glGetIntegerv) {
        return -1;
    }
    return 0;
}

static unsigned int compile_shader(unsigned int type, const char *source)
{
    unsigned int shader = p_glCreateShader(type);
    if (!shader) return 0;

    p_glShaderSource(shader, 1, &source, NULL);
    p_glCompileShader(shader);

    int status = 0;
    p_glGetShaderiv(shader, 0x8B81, &status);
    if (!status) {
        char log[1024];
        int len = 0;
        p_glGetShaderInfoLog(shader, sizeof(log), &len, log);
        fprintf(stderr, "Shader compile error: %.*s\n", len, log);
        p_glDeleteShader(shader);
        return 0;
    }
    fprintf(stderr, "Shader compiled OK\n");
    return shader;
}

static unsigned int create_simple_shader_program(void)
{
    const char *vert_src =
        "#version 330 core\n"
        "layout(location = 0) in vec2 aPos;\n"
        "layout(location = 1) in vec3 aColor;\n"
        "out vec3 ourColor;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "  ourColor = aColor;\n"
        "}\n";

    const char *frag_src =
        "#version 330 core\n"
        "in vec3 ourColor;\n"
        "out vec4 FragColor;\n"
        "void main() {\n"
        "  FragColor = vec4(ourColor, 1.0);\n"
        "}\n";

    unsigned int vert = compile_shader(0x8B31, vert_src);
    unsigned int frag = compile_shader(0x8B30, frag_src);
    if (!vert || !frag) {
        fprintf(stderr, "Shader program creation failed: vertex=%u fragment=%u\n", vert, frag);
        return 0;
    }

    unsigned int program = p_glCreateProgram();
    p_glAttachShader(program, vert);
    p_glAttachShader(program, frag);
    p_glLinkProgram(program);

    int status = 0;
    p_glGetProgramiv(program, 0x8B82, &status);
    if (!status) {
        char log[1024];
        int len = 0;
        p_glGetProgramInfoLog(program, sizeof(log), &len, log);
        fprintf(stderr, "Shader link error: %.*s\n", len, log);
        p_glDeleteProgram(program);
        p_glDeleteShader(vert);
        p_glDeleteShader(frag);
        return 0;
    }
    fprintf(stderr, "Shader program linked OK (program=%u)\n", program);

    p_glDeleteShader(vert);
    p_glDeleteShader(frag);
    return program;
}

/* ------------------------------------------------------------------ */
/* OpenGL 3.3 Core context creation                                   */
/* ------------------------------------------------------------------ */

static int create_opengl_context(HDC hdc)
{
    /* Step 1: Set pixel format */
    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hdc, &pfd);
    if (pixelFormat == 0) {
        fprintf(stderr, "OpenGL: ChoosePixelFormat failed\n");
        return -1;
    }

    if (!SetPixelFormat(hdc, pixelFormat, &pfd)) {
        fprintf(stderr, "OpenGL: SetPixelFormat failed\n");
        return -1;
    }

    /* Step 2: Create temporary context to get wglCreateContextAttribsARB */
    HGLRC tempContext = p_wglCreateContext(hdc);
    if (!tempContext) {
        fprintf(stderr, "OpenGL: wglCreateContext (temp) failed\n");
        return -1;
    }

    if (!p_wglMakeCurrent(hdc, tempContext)) {
        fprintf(stderr, "OpenGL: wglMakeCurrent (temp) failed\n");
        p_wglDeleteContext(tempContext);
        return -1;
    }

    p_wglCreateContextAttribsARB = (wglCreateContextAttribsARB_t)load_gl_proc("wglCreateContextAttribsARB");

    p_wglMakeCurrent(NULL, NULL);
    p_wglDeleteContext(tempContext);

    if (!p_wglCreateContextAttribsARB) {
        fprintf(stderr, "OpenGL: wglCreateContextAttribsARB not available - falling back to legacy context\n");
        g_hrc = p_wglCreateContext(hdc);
        if (!g_hrc) {
            fprintf(stderr, "OpenGL: wglCreateContext (fallback) failed\n");
            return -1;
        }
        if (!p_wglMakeCurrent(hdc, g_hrc)) {
            fprintf(stderr, "OpenGL: wglMakeCurrent (fallback) failed\n");
            return -1;
        }
        return 0;
    }

    /* Step 4: Create OpenGL 3.3 Core context */
    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    g_hrc = p_wglCreateContextAttribsARB(hdc, NULL, attribs);
    if (!g_hrc) {
        fprintf(stderr, "OpenGL: wglCreateContextAttribsARB (3.3 core) failed - trying compatibility profile\n");
        int attribs2[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            0
        };
        g_hrc = p_wglCreateContextAttribsARB(hdc, NULL, attribs2);
        if (!g_hrc) {
            fprintf(stderr, "OpenGL: wglCreateContextAttribsARB (3.3 compat) failed\n");
            return -1;
        }
    }

    if (!p_wglMakeCurrent(hdc, g_hrc)) {
        fprintf(stderr, "OpenGL: wglMakeCurrent (3.3) failed\n");
        return -1;
    }

    if (p_glGetString) {
        const unsigned char *version = p_glGetString(0x1F01);
        fprintf(stderr, "OpenGL: %s\n", version ? version : "(unknown)");
    }

    return 0;
}

static LRESULT CALLBACK sw_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        g_window_should_close = 1;
        g_window_open = 0;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        g_window_open = 0;
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        g_window_width = LOWORD(lp);
        g_window_height = HIWORD(lp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI sw_window_thread_proc(LPVOID lpParam)
{
    (void)lpParam;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = sw_window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = g_window_class;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return 1;
        }
    }

    /* SW_FULLSCREEN=1: borderless window covering the primary monitor. */
    const char *fs = getenv("SW_FULLSCREEN");
    int fullscreen = fs && fs[0] == '1';
    DWORD style = fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    int win_x = CW_USEDEFAULT, win_y = CW_USEDEFAULT;
    if (fullscreen) {
        g_window_width = GetSystemMetrics(SM_CXSCREEN);
        g_window_height = GetSystemMetrics(SM_CYSCREEN);
        win_x = 0; win_y = 0;
    }
    RECT rc = {0, 0, g_window_width, g_window_height};
    if (!fullscreen) AdjustWindowRect(&rc, style, FALSE);

    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    g_hwnd = CreateWindowExW(
            0, g_window_class, L"Pokemon Legends: Arceus", style,
            win_x, win_y, win_w, win_h,
            NULL, NULL, GetModuleHandleW(NULL), NULL);

        if (!g_hwnd) return 1;

        g_hdc = GetDC(g_hwnd);
        if (!g_hdc) return 1;

        audio_init();

            if (load_wgl_functions() != 0) {
                fprintf(stderr, "warning: could not load WGL functions\n");
            } else if (create_opengl_context(g_hdc) != 0) {
                fprintf(stderr, "warning: could not create OpenGL context\n");
            } else if (load_gl_functions() != 0) {
                fprintf(stderr, "warning: could not load OpenGL functions\n");
            } else {
                p_glViewport(0, 0, g_window_width, g_window_height);
                p_glClearColor(g_clear_r, g_clear_g, g_clear_b, g_clear_a);

                g_shader_program = create_simple_shader_program();
                    if (g_shader_program) {
                        float vertices[] = {
                            0.0f,  0.5f,       1.0f, 0.0f, 0.0f,
                           -0.5f, -0.5f,       0.0f, 1.0f, 0.0f,
                            0.5f, -0.5f,       0.0f, 0.0f, 1.0f,
                        };

                        p_glGenVertexArrays(1, &g_vao);
                        p_glGenBuffers(1, &g_vbo);

                        p_glBindVertexArray(g_vao);
                        p_glBindBuffer(0x8892, g_vbo);
                        p_glBufferData(0x8892, sizeof(vertices), vertices, 0x88E4);

                        p_glVertexAttribPointer(0, 2, 0x1406, 0, 5 * sizeof(float), (void *)0);
                        p_glEnableVertexAttribArray(0);
                        p_glVertexAttribPointer(1, 3, 0x1406, 0, 5 * sizeof(float), (void *)(2 * sizeof(float)));
                        p_glEnableVertexAttribArray(1);

                        p_glBindVertexArray(0);
                        g_gfx_ready = 1;
                    }
            }

    g_window_open = 1;

    if (g_gfx_ready) {
        /* Log OpenGL version and renderer */
        const unsigned char *ver = p_glGetString(0x1F02); /* GL_VERSION */
        const unsigned char *ren = p_glGetString(0x1F01); /* GL_RENDERER */
        const unsigned char *ven = p_glGetString(0x1F00); /* GL_VENDOR */
        fprintf(stderr, "OpenGL: version=%s renderer=%s vendor=%s\n",
                ver ? (const char *)ver : "(unknown)",
                ren ? (const char *)ren : "(unknown)",
                ven ? (const char *)ven : "(unknown)");

        /* Render loop.  The GL context is current on THIS thread only, so all
         * drawing happens here; the game thread just publishes a clear colour
         * and bumps g_present_count (sw_window_present / sw_gfx_set_clear_color). */
        LONG shown_presents = -1;
        DWORD last_title = 0;
        while (g_window_open && !g_window_should_close) {
            Sleep(16); /* ~60 FPS */
            sw_window_update_flash();
            p_glClearColor(g_clear_r, g_clear_g, g_clear_b, g_clear_a);
            p_glClear(0x00004000); /* GL_COLOR_BUFFER_BIT */
            if (g_test_pattern && g_shader_program && g_vao) {
                p_glUseProgram(g_shader_program);
                p_glBindVertexArray(g_vao);
                p_glDrawArrays(0x0004, 0, 3);
                p_glBindVertexArray(0);
            }
            p_wglSwapBuffers(g_hdc);

            /* Title shows what the game has presented, so a screenshot proves
             * where the pixels came from. */
            DWORD now = GetTickCount();
            LONG presents = g_present_count;
            if (presents != shown_presents && now - last_title > 500) {
                wchar_t title[160];
                extern int sw_fps_get(void);
                int fps = sw_fps_get();
                if (presents > 0)
                    swprintf(title, 160, L"Pokemon Legends: Arceus [recomp] - game frames: %ld - %ls - clear rgba(%.2f, %.2f, %.2f, %.2f)",
                             presents, fps == 60 ? L"60 FPS" : (fps == 30 ? L"30 FPS" : L"Auto"), g_clear_r, g_clear_g, g_clear_b, g_clear_a);
                else
                    swprintf(title, 160, L"Pokemon Legends: Arceus [recomp] - %ls",
                             g_test_pattern ? L"test pattern (no game)" : L"waiting for first game frame");
                SetWindowTextW(g_hwnd, title);
                shown_presents = presents;
                last_title = now;
            }

            /* Pump messages without blocking */
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    g_window_open = 0;
                    g_window_should_close = 1;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    audio_shutdown();

#ifdef USE_SDL_CONTROLLER
    sdl_controller_shutdown();
#endif

    if (g_gfx_ready) {
        if (g_vbo)   p_glDeleteBuffers(1, &g_vbo);
        if (g_vao)   p_glDeleteVertexArrays(1, &g_vao);
        if (g_shader_program) p_glDeleteProgram(g_shader_program);
    }
    if (g_hrc) {
        p_wglMakeCurrent(NULL, NULL);
        p_wglDeleteContext(g_hrc);
        g_hrc = NULL;
    }
    if (g_hdc) {
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
    }
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    UnregisterClassW(g_window_class, GetModuleHandleW(NULL));

    return 0;
}

int sw_window_init(int width, int height, const char *title)
{
    g_window_width = width;
    g_window_height = height;
    /* SW_WINDOW_SIZE=WIDTHxHEIGHT (launcher setting), e.g. 1920x1080 */
    const char *ws = getenv("SW_WINDOW_SIZE");
    if (ws) {
        int w = 0, h = 0;
        if (sscanf(ws, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 7680 && h <= 4320) {
            g_window_width = w;
            g_window_height = h;
        }
    }

    g_window_thread = CreateThread(NULL, 0, sw_window_thread_proc, NULL, 0, &g_window_thread_id);
    if (!g_window_thread) return -1;

    /* Wait for window to be created (up to 5 seconds for shader compilation) */
    for (int i = 0; i < 500 && !g_window_open; i++) {
        Sleep(10);
    }

    if (!g_window_open) {
        return -1;
    }

    /* The window thread owns the title (it shows game frames presented and
     * the clear colour); setting it here raced with that and hid the status. */
    (void)title;

    return 0;
}

void sw_window_pump(void)
{
    /* No-op: message pump runs on window thread */
}

/* Called from the game thread when the game presents a frame.  No GL here:
 * the context belongs to the window thread, which draws every 16 ms. */
void sw_window_present(void)
{
    InterlockedIncrement(&g_present_count);
}

/* Show the test triangle (no-game mode) instead of a plain clear. */
void sw_window_set_test_pattern(int on)
{
    g_test_pattern = on;
}

/* Flash the clear color to indicate FPS mode change (30=amber, 60=cyan). */
void sw_window_flash_fps(int fps)
{
    if (g_flash_frames == 0) {
        g_flash_saved_r = g_clear_r; g_flash_saved_g = g_clear_g; g_flash_saved_b = g_clear_b;
        if (fps == 60) { g_clear_r = 0.0f; g_clear_g = 1.0f; g_clear_b = 1.0f; }  /* cyan */
        else { g_clear_r = 1.0f; g_clear_g = 0.6f; g_clear_b = 0.0f; }            /* amber */
        g_flash_frames = 10;  /* ~10 frames at 60Hz = ~160ms */
    }
}

/* Called from window thread to decay the flash. */
void sw_window_update_flash(void)
{
    if (g_flash_frames > 0) {
        g_flash_frames--;
        if (g_flash_frames == 0) {
            g_clear_r = g_flash_saved_r; g_clear_g = g_flash_saved_g; g_clear_b = g_flash_saved_b;
        }
    }
}

long sw_window_present_count(void)
{
    return g_present_count;
}

void sw_gfx_set_clear_color(float r, float g, float b, float a)
{
    g_clear_r = r;
    g_clear_g = g;
    g_clear_b = b;
    g_clear_a = a;
}

int sw_gfx_is_ready(void)
{
    return g_gfx_ready;
}

int sw_window_is_open(void)
{
    return g_window_open && !g_window_should_close;
}

void sw_window_shutdown(void)
{
    if (!g_window_open) return;

    g_window_should_close = 1;
    g_window_open = 0;

    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }

    if (g_window_thread) {
        WaitForSingleObject(g_window_thread, 5000);
        CloseHandle(g_window_thread);
        g_window_thread = NULL;
    }
}

static void generate_tone(short *buf, int samples, double freq_hz, double volume) {
    for (int i = 0; i < samples; i++) {
        double t = (double)i / (double)AUDIO_SAMPLE_RATE;
        double sample = sin(2.0 * 3.14159265358979323846 * freq_hz * t);
        buf[i] = (short)(sample * volume * 32767.0);
    }
}

static int audio_init(void) {
    WAVEFORMATEX wfx;
    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = AUDIO_CHANNELS;
    wfx.nSamplesPerSec  = AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = AUDIO_BITS;
    wfx.nBlockAlign     = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mmr = waveOutOpen(&g_waveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mmr != MMSYSERR_NOERROR) {
        fprintf(stderr, "Audio: waveOutOpen failed (%u)\n", mmr);
        return -1;
    }

    int samples_per_buffer = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8) / 10;
    int bytes_per_buffer   = samples_per_buffer * sizeof(short);

    for (int i = 0; i < 2; i++) {
        ZeroMemory(&g_waveHeaders[i], sizeof(WAVEHDR));
        g_waveHeaders[i].lpData           = (char *)g_audioBuffers[i];
        g_waveHeaders[i].dwBufferLength   = bytes_per_buffer;
        g_waveHeaders[i].dwBytesRecorded  = 0;
        g_waveHeaders[i].dwUser           = 0;
        g_waveHeaders[i].dwFlags          = 0;
        g_waveHeaders[i].dwLoops          = 0;
        g_waveHeaders[i].lpNext           = NULL;
        g_waveHeaders[i].reserved         = 0;

        mmr = waveOutPrepareHeader(g_waveOut, &g_waveHeaders[i], sizeof(WAVEHDR));
        if (mmr != MMSYSERR_NOERROR) {
            fprintf(stderr, "Audio: waveOutPrepareHeader failed (%u)\n", mmr);
            return -1;
        }
    }

    g_audioBufferIndex = 0;
    g_audioInitialized = 1;
    return 0;
}

static void audio_play_tone(double freq_hz, double volume, double duration_sec) {
    if (!g_audioInitialized || !g_waveOut) return;

    int samples_per_buffer = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8) / 10;
    int total_samples      = (int)(AUDIO_SAMPLE_RATE * duration_sec);
    int samples_written    = 0;

    while (samples_written < total_samples) {
        int samples_this_buffer = samples_per_buffer;
        if (samples_written + samples_this_buffer > total_samples)
            samples_this_buffer = total_samples - samples_written;

        generate_tone(g_audioBuffers[g_audioBufferIndex], samples_this_buffer * AUDIO_CHANNELS,
                      freq_hz, volume);

        if (samples_this_buffer < samples_per_buffer) {
            memset(g_audioBuffers[g_audioBufferIndex] + samples_this_buffer * AUDIO_CHANNELS,
                   0, (samples_per_buffer - samples_this_buffer) * AUDIO_CHANNELS * sizeof(short));
        }

        MMRESULT mmr = waveOutWrite(g_waveOut, &g_waveHeaders[g_audioBufferIndex], sizeof(WAVEHDR));
        if (mmr != MMSYSERR_NOERROR) {
            fprintf(stderr, "Audio: waveOutWrite failed (%u)\n", mmr);
            break;
        }

        while ((g_waveHeaders[g_audioBufferIndex].dwFlags & WHDR_DONE) == 0) {
            Sleep(5);
        }

        samples_written += samples_this_buffer;
        g_audioBufferIndex = 1 - g_audioBufferIndex;
    }
}

static void audio_shutdown(void) {
    if (g_waveOut) {
        waveOutReset(g_waveOut);
        for (int i = 0; i < 2; i++) {
            waveOutUnprepareHeader(g_waveOut, &g_waveHeaders[i], sizeof(WAVEHDR));
        }
        waveOutClose(g_waveOut);
        g_waveOut = NULL;
    }
    g_audioInitialized = 0;
}

#ifdef USE_SDL_CONTROLLER
static int sdl_controller_init(void) {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL: could not init gamecontroller: %s\n", SDL_GetError());
        return -1;
    }

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller) {
                fprintf(stderr, "SDL: controller connected: %s\n", SDL_GameControllerName(g_controller));
                break;
            }
        }
    }

    if (!g_controller) {
        fprintf(stderr, "SDL: no game controller found\n");
        return -1;
    }
    return 0;
}

static void sdl_controller_poll(int *buttons, float *left_x, float *left_y, float *right_x, float *right_y) {
    if (!g_controller) {
        *left_x = *left_y = *right_x = *right_y = 0.0f;
        *buttons = 0;
        return;
    }

    SDL_GameControllerUpdate();

    *buttons = 0;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A))    *buttons |= 0x0001;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B))    *buttons |= 0x0002;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X))    *buttons |= 0x0004;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y))    *buttons |= 0x0008;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LB))   *buttons |= 0x0010;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RB))   *buttons |= 0x0020;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_BACK)) *buttons |= 0x0040;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START)) *buttons |= 0x0080;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_GUIDE)) *buttons |= 0x0100;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP))    *buttons |= 0x0200;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  *buttons |= 0x0400;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  *buttons |= 0x0800;
    if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) *buttons |= 0x1000;

    *left_x  = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX)  / 32768.0f;
    *left_y  = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY)  / 32768.0f;
    *right_x = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0f;
    *right_y = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0f;

    if (*left_x  > -0.15f && *left_x  < 0.15f) *left_x  = 0.0f;
    if (*left_y  > -0.15f && *left_y  < 0.15f) *left_y  = 0.0f;
    if (*right_x > -0.15f && *right_x < 0.15f) *right_x = 0.0f;
    if (*right_y > -0.15f && *right_y < 0.15f) *right_y = 0.0f;
}

static void sdl_controller_shutdown(void) {
    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}
#endif /* USE_SDL_CONTROLLER */

void sw_audio_play_tone(double freq_hz, double volume, double duration_sec) {
    audio_play_tone(freq_hz, volume, duration_sec);
}

#ifdef USE_SDL_CONTROLLER
void sw_controller_poll(int *buttons, float *left_x, float *left_y, float *right_x, float *right_y) {
    sdl_controller_poll(buttons, left_x, left_y, right_x, right_y);
}
#else
void sw_controller_poll(int *buttons, float *left_x, float *left_y, float *right_x, float *right_y) {
    *buttons = 0;
    *left_x = *left_y = *right_x = *right_y = 0.0f;
}
#endif
