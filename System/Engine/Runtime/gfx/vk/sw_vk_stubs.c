/* Neutral stand-ins for the optional modules of totk-pc-recomp's Vulkan back
 * end that this port leaves out: DLSS/DLAA, FSR, HDR output, colour grading,
 * contact shadows, motion-vector helpers, uniform snapshots, frame dumps, GPU
 * crash breadcrumbs, RenderDoc, the in-game ImGui menu, the flight recorder,
 * TotK resolution patches and its input layer.  Each reports "off" / "none"
 * so the core takes its plain path. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "input_win.h"
#include "ui/menu.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"

#pragma comment(lib, "imm32.lib")

/* ---- variables */
int flight_on;
int vk_hdr_track;

/* ---- contact shadows (colour grading and FSR: vk_color.c, vk_fsr.c, ported) */
int vk_contact_strength(void) { return 0; }
void vk_contact_release(void) {}
void vk_contact_flush(VkExec *ex) { (void)ex; }
void vk_contact_pass_begin(VkExec *ex) { (void)ex; }

/* ---- HDR output: SDR only */
int vk_hdr_wanted(void) { return 0; }
void vk_hdr_off(void) {}
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out)
{ (void)f; (void)n; (void)out; return 0; }
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) { (void)sc; (void)new_sc; }
void vk_hdr_set_active(int on) { (void)on; }
int vk_hdr_active(void) { return 0; }
VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4])
{ (void)cb; (void)slot; (void)fmt; (void)crop; return src; }
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) { (void)cb; (void)w; (void)h; return VK_NULL_HANDLE; }
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h)
{ (void)cb; (void)dst_view; (void)fmt; (void)w; (void)h; }
void vk_hdr_release(void) {}
void vk_hdr_support(float out[4]) { out[0] = out[1] = out[2] = out[3] = 0.0f; }
void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words) { (void)spirv; (void)words; }
void vk_hdr_present_source(const VkTexImage *win, VkImage *image, VkFormat *format) { (void)win; (void)image; (void)format; }
void vk_hdr_image_gone(VkImage image) { (void)image; }
void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info) { (void)ex; (void)info; }
void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r)
{ (void)a2r; memcpy(out, in, px * 4); }

/* ---- device memory headroom tracking */
const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void) { return NULL; }
void vk_headroom_start(void) {}
void vk_headroom_stop(void) {}

/* ---- DLSS / DLAA: vk_dlaa.c (ported).  DLAA only: the scene keeps the game's size, so the resolution module of the
 * original (scene sizes for DLSS upscaling modes) answers "output = scene"; the motion checks are test tools. */
void res_sizes(unsigned out[2], unsigned scene[2]) { out[0] = out[1] = scene[0] = scene[1] = 0; }
int res_dlss_mode(const char *mode) { (void)mode; return 0; }
float res_dlss_lod_bias(void) { return 0.0f; }
void vk_mvcheck_camera(VkCommandBuffer cb, VkImage mv, uint32_t w, uint32_t h, float jx, float jy)
{ (void)cb; (void)mv; (void)w; (void)h; (void)jx; (void)jy; }
void vk_mvcheck_final(VkCommandBuffer cb, uint64_t fence, VkImage mv, VkImage color, VkFormat f, uint32_t level,
                      uint32_t layer, uint32_t w, uint32_t h, float jx, float jy)
{ (void)cb; (void)fence; (void)mv; (void)color; (void)f; (void)level; (void)layer; (void)w; (void)h; (void)jx; (void)jy; }
void vk_mvcheck_output(VkCommandBuffer cb, VkImage out, uint32_t w, uint32_t h) { (void)cb; (void)out; (void)w; (void)h; }

/* ---- motion-vector helpers */
uint64_t vk_mvprev_step(int stage, int index, uint64_t size) { (void)stage; (void)index; (void)size; return 0; }
int vk_mvprev_on(void) { return 0; }
void vk_mvprev_want(uint32_t index_mask) { (void)index_mask; }
int vk_mvprev_find(int index, uint64_t addr, uint64_t size, uint64_t *prev, int *how)
{ (void)index; (void)addr; (void)size; (void)prev; (void)how; return 0; }
void vk_mvprev_frame(void) {}
void vk_mvprev_draw(VkExec *ex) { (void)ex; }
void vk_mvsurvey_draw(VkExec *ex) { (void)ex; }
void vk_mvsurvey_frame(void) {}

/* ---- uniform snapshots: vk_usnap.c (ported; TOTK_UBO_SNAPSHOT / SW_UBO_SNAPSHOT=0 binds live memory) */

/* ---- frame dumps: vk_dump.c (SW_DUMP=<frame>, ported) */

/* ---- GPU crash breadcrumbs */
void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) { (void)cb; (void)kind; (void)n; }
void vk_crumbs_init(VkCtx *c) { (void)c; }
void vk_crumbs_release(void) {}
void vk_crumbs_report(void) {}
VkPassMark *vk_pass_mark_begin(VkCommandBuffer cb, int kind, uint64_t fence) { (void)cb; (void)kind; (void)fence; return NULL; }
void vk_pass_mark_end(VkCommandBuffer cb, const VkPassMark *m) { (void)cb; (void)m; }
void vk_pass_marks_report(void) {}
int vk_pass_marks_want_nv(void) { return 0; }

/* ---- RenderDoc */
void rdoc_init(void) {}
int rdoc_active(void) { return 0; }
void rdoc_after_present(uint64_t frame) { (void)frame; }

/* ---- in-game menu, FPS counter and the screenshot hook: sw_menu_vk.c */

/* ---- input (later: this runtime's sw_input.c) */
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) { (void)h; (void)m; (void)w; (void)l; (void)res; return 0; }
void input_cursor_suspend(int on) { (void)on; }

/* ---- system: applet messages, saves, settings writes, misc */
void oe_push_message(uint32_t message) { (void)message; }
void oe_set_focus(int in_focus) { (void)in_focus; }
void fs_save_flush(void) {}
/* cfg_set / cfg_save: gfx/sw_cfg.c */
void kern_signal_system_event(uint64_t ev) { (void)ev; }   /* every event already reads as signalled */
void prof_write(void) {}
void res_fov_live(void) {}
int probe_census_dir(char *out, size_t n) { (void)out; (void)n; return 0; }

/* PNG writing (screenshots): an uncompressed BMP next to the requested name instead.
 * [arceus] whole rows per write (was one fwrite per pixel: ~50 ms a 1080p shot, the game slowed to ~16 fps under
 * SW_SHOT_FRAMES); SW_SHOT_CROP=x,y,w,h keeps only that part; screenshots/times.txt gets "<file> <ms since start>"
 * (QPC) per shot, so a test can time animations frame by frame. */
void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    static int init; static uint32_t cx, cy, cw, ch; static LARGE_INTEGER fq, t0;
    if (!init) {
        init = 1;
        QueryPerformanceFrequency(&fq), QueryPerformanceCounter(&t0);
        const char *e = getenv("SW_SHOT_CROP");
        if (e) sscanf(e, "%u,%u,%u,%u", &cx, &cy, &cw, &ch);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint32_t x0 = 0, y0 = 0, ow = w, oh = h;
    if (cw && ch && cx < w && cy < h) {
        x0 = cx, y0 = cy;
        ow = cx + cw > w ? w - cx : cw, oh = cy + ch > h ? h - cy : ch;
    }
    char bmp[MAX_PATH];
    snprintf(bmp, sizeof bmp, "%s.bmp", path);
    FILE *f = fopen(bmp, "wb");
    if (!f) return;
    uint32_t row = ow * 4, size = 54 + row * oh;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, dib = 40, planes_bpp = 1 | (32u << 16);
    int32_t sw = (int32_t)ow, sh = -(int32_t)oh;
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &sw, 4); memcpy(hdr + 22, &sh, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t *line = (uint8_t *)malloc(row);
    if (line) {
        for (uint32_t y = 0; y < oh; y++) {
            const uint8_t *src = rgba + ((size_t)(y0 + y) * w + x0) * 4;
            for (uint32_t i = 0; i < ow; i++)
                line[i * 4] = src[i * 4 + 2], line[i * 4 + 1] = src[i * 4 + 1], line[i * 4 + 2] = src[i * 4],
                line[i * 4 + 3] = src[i * 4 + 3];
            fwrite(line, 1, row, f);
        }
        free(line);
    }
    fclose(f);
    FILE *t = fopen("screenshots/times.txt", "a");
    if (t) {
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
        fprintf(t, "%s %.3f\n", base, (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
        fclose(t);
    }
}
