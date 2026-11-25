/* sw_menu.h - the in-game menu: state and input (sw_menu.c), drawn by the
 * Vulkan present overlay (gfx/vk/sw_menu_vk.c).
 *
 * Open / close: Minus and Plus held together (pad, or their keyboard keys -
 * default Tab/P + M), F1 on the keyboard.  B or "Continue" closes it.
 * While it is open the game gets no input (it keeps running).
 *
 * Kept out of Engine/Runtime/include on purpose: the object cache key of the
 * generated chunks hashes every header there, so a new header would recompile
 * all ~3,300 chunks. */
#ifndef SW_MENU_H
#define SW_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SW_MENU_CONTINUE, SW_MENU_FPS, SW_MENU_COUNTER, SW_MENU_FULLSCREEN, SW_MENU_MODS, SW_MENU_SHOT, SW_MENU_QUIT,
    SW_MENU_ITEMS
};
enum { SW_MENU_ACT_NONE, SW_MENU_ACT_FULLSCREEN, SW_MENU_ACT_SCREENSHOT, SW_MENU_ACT_QUIT };

/* HID poll (sw_hid.c), any game thread: takes the merged npad buttons
 * (libnx HidNpadButton bits incl. stick directions 16-23) and returns what the
 * game should see.  *neutral_sticks = 1: report centred sticks. */
uint64_t sw_menu_filter(uint64_t buttons, int *neutral_sticks);

int  sw_menu_is_open(void);
void sw_menu_toggle(void);                 /* F1 (present thread) */
int  sw_menu_selected(void);
int  sw_menu_quit_armed(void);             /* "Quit game" pressed once: the next A quits */
int  sw_menu_take_action(void);            /* present thread: next SW_MENU_ACT_*, then cleared */

/* performance overlay: 0 off, 1 FPS line, 2 detailed panel (ui/sw_perfhud.cpp).  SW_FPS_OVERLAY=0/1/2 at start,
 * else the saved choice; F3 off <-> FPS, F10 cycles all three.  Set saves it (Config/ingame_settings.json). */
int  sw_menu_fps_counter(void);
void sw_menu_set_fps_counter(int mode);
void sw_menu_set_fullscreen_state(int on); /* present thread reports the window state */
int  sw_menu_fullscreen_state(void);

/* The ImGui overlay (ui/sw_overlay.cpp: mods menu, mod browser).  "Mods" in this menu or F4 opens it; while it is open
 * the game gets no input (as with this menu), Minus+Plus closes it.  sw_menu_pad_raw: the buttons of the last poll
 * before filtering (the overlay's pad navigation). */
int  sw_menu_overlay_open(void);
void sw_menu_overlay_set(int open);
uint64_t sw_menu_pad_raw(void);

/* The overlay's pages: the game menu (Minus+Plus / F1: Graphics, Audio, Controls, Game tabs) and the mods menu
 * (F4, or Game > Mods).  The old bitmap menu above is no longer opened. */
enum { SW_OVL_MODS = 0, SW_OVL_MENU = 1 };
int  sw_menu_overlay_page(void);
void sw_menu_overlay_open_page(int page);  /* opens the overlay on that page (or switches page if open) */
void sw_menu_request(int act);             /* an SW_MENU_ACT_* from the overlay (run by the present path) */

/* live sound volume (src/sw_ipc.c, waveOut): 0..100 */
int  sw_audio_volume(void);
void sw_audio_set_volume(int percent);

#ifdef __cplusplus
}
#endif

#endif /* SW_MENU_H */
