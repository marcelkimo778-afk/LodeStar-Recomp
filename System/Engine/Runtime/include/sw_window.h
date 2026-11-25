/* sw_window.h - Window + OpenGL graphics API for standalone runtime. */

#ifndef SW_WINDOW_H
#define SW_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

int sw_window_init(int width, int height, const char *title);
void sw_window_pump(void);
void sw_window_present(void);
int sw_window_is_open(void);
void sw_window_shutdown(void);
void sw_window_set_test_pattern(int on);   /* triangle instead of plain clear */
long sw_window_present_count(void);        /* frames presented by the game */
void sw_window_flash_fps(int fps);         /* flash clear color on FPS toggle */
void sw_window_update_flash(void);         /* call from window thread each frame */

/* Graphics */
void sw_gfx_set_clear_color(float r, float g, float b, float a);
int sw_gfx_is_ready(void);

/* Audio */
void sw_audio_play_tone(double freq_hz, double volume, double duration_sec);

/* Controller (SDL2) */
void sw_controller_poll(int *buttons, float *left_x, float *left_y, float *right_x, float *right_y);

#ifdef __cplusplus
}
#endif

#endif /* SW_WINDOW_H */
