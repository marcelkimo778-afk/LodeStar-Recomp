// sw_overlay.cpp - the in-game Dear ImGui overlay: the mods menu now, the mod browser next (MODS.md).
//
// Seams (all called from C):
//   ovl_frame / ovl_draw / ovl_release - from the present path via gfx/vk/sw_menu_vk.c (menu_overlay_*), after the
//       bitmap menu; inside dynamic rendering on the swapchain image, present lock held.  ImGui runs on that thread only.
//   ovl_window_msg - from the window procedure (menu_window_msg): keyboard, mouse and typed text while it is open.
//   Open / close state lives in src/sw_menu.c (sw_menu_overlay_open / _set): "Mods" in the in-game menu or F4 opens it;
//   B, Esc, F4 or Minus+Plus close it.  While it is open the game gets no input and the mouse camera lets the cursor go.
//
// Cost: nothing is created before the first open (no ImGui context, no pipelines, no font); after that, a closed
// overlay costs one load per frame.  Fonts come from Windows (Segoe UI) at run time - nothing is bundled; without it
// ImGui's built-in font is used.
//
// Test hook: SW_OVL_TEST=<frame> opens the overlay at that presented frame (with SW_SHOT_FRAMES for a screenshot).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "ui/menu.h"
#include "sw_menu.h"
#include "sw_mods.h"
#include "sw_browser_ui.h"

extern "C" void perfhud_layout(float W, float H, float s, ImFont *body, ImFont *small);   // sw_perfhud.cpp
extern "C" void vk_queue_lock(void);
extern "C" void vk_queue_unlock(void);

// ---- events from the window thread, drained on the present thread
enum { EV_KEY, EV_CHAR, EV_POS, EV_BUTTON, EV_WHEEL };
struct Ev { int type, a, b; float x, y; };
static SRWLOCK g_ev_lock = SRWLOCK_INIT;
static Ev g_ev[256];
static int g_nev;

static void push(Ev e)
{
    AcquireSRWLockExclusive(&g_ev_lock);
    if (e.type == EV_POS && g_nev && g_ev[g_nev - 1].type == EV_POS) g_ev[g_nev - 1] = e;
    else if (g_nev < 256) g_ev[g_nev++] = e;
    ReleaseSRWLockExclusive(&g_ev_lock);
}

static ImGuiKey vk_to_key(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (int)(vk - 'A'));
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (int)(vk - '0'));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_MENU: case VK_LMENU: return ImGuiKey_LeftAlt;
    case VK_RMENU: return ImGuiKey_RightAlt;
    }
    return ImGuiKey_None;
}

extern "C" int ovl_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res)
{
    (void)h;
    // F10 (polled in sw_menu_vk.c: performance overlay) is a system key: Windows would enter the window's menu loop
    // and eat the next key press
    if ((m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && w == VK_F10) { *res = 0; return 1; }
    if (!sw_menu_overlay_open()) return 0;
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        int down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
        if (w == VK_F4) { *res = 0; return 1; }         // F4 is polled (sw_menu_vk.c): open / close
        if (w == VK_F11) return 0;                       // the window's own fullscreen toggle
        ImGuiKey k = vk_to_key(w);
        if (k != ImGuiKey_None) push({EV_KEY, (int)k, down, 0, 0});
        *res = 0;
        return 1;
    }
    case WM_CHAR:
        push({EV_CHAR, (int)w, 0, 0, 0});
        *res = 0;
        return 1;
    case WM_MOUSEMOVE:
        push({EV_POS, 0, 0, (float)(short)LOWORD(l), (float)(short)HIWORD(l)});
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: push({EV_BUTTON, 0, m == WM_LBUTTONDOWN, 0, 0}); return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: push({EV_BUTTON, 1, m == WM_RBUTTONDOWN, 0, 0}); return 0;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: push({EV_BUTTON, 2, m == WM_MBUTTONDOWN, 0, 0}); return 0;
    case WM_MOUSEWHEEL: push({EV_WHEEL, 0, 0, 0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA}); return 0;
    }
    return 0;
}

static void drain(ImGuiIO &io)
{
    Ev q[256];
    AcquireSRWLockExclusive(&g_ev_lock);
    int n = g_nev;
    memcpy(q, g_ev, sizeof(Ev) * (size_t)n);
    g_nev = 0;
    ReleaseSRWLockExclusive(&g_ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY:
            io.AddKeyEvent((ImGuiKey)e.a, e.b != 0);
            if (e.a == ImGuiKey_LeftCtrl || e.a == ImGuiKey_RightCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
            if (e.a == ImGuiKey_LeftShift || e.a == ImGuiKey_RightShift) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
            if (e.a == ImGuiKey_LeftAlt || e.a == ImGuiKey_RightAlt) io.AddKeyEvent(ImGuiMod_Alt, e.b != 0);
            break;
        case EV_CHAR: io.AddInputCharacterUTF16((ImWchar16)e.a); break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        }
    }
}

// ---- pad: Switch buttons (HidNpadButton bits, src/sw_menu.c sw_menu_pad_raw) to ImGui's gamepad keys.  Nintendo
// layout: A (right face button) activates, B cancels.  The left stick moves like the D-pad.
static const struct { int bit; ImGuiKey key; } kPad[] = {
    {0, ImGuiKey_GamepadFaceDown}, {1, ImGuiKey_GamepadFaceRight}, {2, ImGuiKey_GamepadFaceUp},
    {3, ImGuiKey_GamepadFaceLeft}, {6, ImGuiKey_GamepadL1}, {7, ImGuiKey_GamepadR1},
    {8, ImGuiKey_GamepadL2}, {9, ImGuiKey_GamepadR2}, {12, ImGuiKey_GamepadDpadLeft}, {13, ImGuiKey_GamepadDpadUp},
    {14, ImGuiKey_GamepadDpadRight}, {15, ImGuiKey_GamepadDpadDown},
};
static uint64_t g_pad_prev;

static void pad_events(ImGuiIO &io, int reset)
{
    uint64_t b = sw_menu_pad_raw();
    b |= ((b >> 16) & 0xFull) << 12;                    // stick L/U/R/D (bits 16-19) -> D-pad L/U/R/D (12-15)
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (reset) { g_pad_prev = b; return; }               // buttons held while opening count once released
    for (const auto &k : kPad) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ g_pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    g_pad_prev = b;
}

// ---- ImGui + Vulkan back end
static int g_ctx, g_vk, g_failed, g_was_open, g_have;   // g_have: ovl_frame laid out something to draw
static VkFormat g_fmt;
static float g_font_scale, g_style_scale;
static ImGuiStyle g_base;
static ImFont *g_font_title;
static ImFont *g_font_serif_big, *g_font_serif, *g_font_small;   // the game menu (Palatino / Segoe UI from Windows)
static LARGE_INTEGER g_qpf, g_last;

// Dark fantasy: near-black panels, gold / amber accents, thin borders.
static const ImVec4 kGold(0.91f, 0.72f, 0.33f, 1), kGoldDim(0.62f, 0.49f, 0.23f, 1), kText(0.93f, 0.91f, 0.86f, 1),
    kMuted(0.60f, 0.58f, 0.54f, 1), kWarn(0.95f, 0.62f, 0.25f, 1), kBad(0.90f, 0.36f, 0.30f, 1), kGood(0.45f, 0.78f, 0.45f, 1);

static void style_init(ImGuiStyle &s)
{
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10; s.FrameRounding = 6; s.GrabRounding = 6; s.TabRounding = 6; s.ChildRounding = 8;
    s.PopupRounding = 8; s.WindowPadding = ImVec2(26, 22); s.FramePadding = ImVec2(12, 7); s.ItemSpacing = ImVec2(12, 10);
    s.WindowBorderSize = 1; s.FrameBorderSize = 1; s.ScrollbarSize = 16;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.040f, 0.035f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.065f, 0.055f, 0.90f);
    c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.055f, 0.05f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.42f, 0.33f, 0.16f, 0.55f);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.10f, 0.085f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.16f, 0.09f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.21f, 0.10f, 1);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.12f, 0.09f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.32f, 0.25f, 0.11f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.35f, 0.14f, 1);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.15f, 0.09f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.24f, 0.11f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.40f, 0.31f, 0.13f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.10f, 0.09f, 0.07f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.32f, 0.25f, 0.11f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.19f, 0.10f, 1);
    c[ImGuiCol_TabSelectedOverline] = kGold;
    c[ImGuiCol_Separator] = ImVec4(0.42f, 0.33f, 0.16f, 0.45f);
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.24f, 0.12f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
}

static int vk_up(const MenuTarget *t)
{
    if (g_vk && g_fmt == t->format) return 1;
    if (g_vk) {                                         // the swapchain changed format: pipelines for the new one
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        ImGui_ImplVulkan_Shutdown();
        g_vk = 0;
        g_font_scale = 0;
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 64;                         // the font + mod thumbnails (browser)
    ii.MinImageCount = 2;
    ii.ImageCount = 4;                                  // vertex buffers rotate per recorded frame (2 frames in flight)
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk = 1;
    return 1;
}

static int fonts_up(float scale)
{
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    char win[MAX_PATH], p[MAX_PATH + 32];
    UINT n = GetWindowsDirectoryA(win, MAX_PATH);
    if (!n || n >= MAX_PATH) strcpy(win, "C:\\Windows");
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x2190, 0x2193, 0};
    ImFontConfig fc;
    fc.OversampleH = 2;
    snprintf(p, sizeof p, "%s\\Fonts\\segoeui.ttf", win);
    ImFont *body = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? fa->AddFontFromFileTTF(p, 22.0f * scale, &fc, ranges) : NULL;
    if (!body) { ImFontConfig d; d.SizePixels = 15.0f * scale; fa->AddFontDefault(&d); }
    snprintf(p, sizeof p, "%s\\Fonts\\segoeuib.ttf", win);
    g_font_title = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? fa->AddFontFromFileTTF(p, 34.0f * scale, &fc, ranges) : NULL;
    {   // the game menu: a serif for its title and tabs (journal feel), a small sans for hints; fall back to Segoe UI
        char q[MAX_PATH + 32];
        snprintf(q, sizeof q, "%s\\Fonts\\palab.ttf", win);
        if (GetFileAttributesA(q) == INVALID_FILE_ATTRIBUTES) snprintf(q, sizeof q, "%s\\Fonts\\georgiab.ttf", win);
        int serif = GetFileAttributesA(q) != INVALID_FILE_ATTRIBUTES;
        g_font_serif_big = serif ? fa->AddFontFromFileTTF(q, 40.0f * scale, &fc, ranges) : g_font_title;
        g_font_serif = serif ? fa->AddFontFromFileTTF(q, 26.0f * scale, &fc, ranges) : g_font_title;
        snprintf(q, sizeof q, "%s\\Fonts\\segoeui.ttf", win);
        g_font_small = GetFileAttributesA(q) != INVALID_FILE_ATTRIBUTES ? fa->AddFontFromFileTTF(q, 18.0f * scale, &fc, ranges) : NULL;
    }
    vk_queue_lock();                                    // the upload submits on the queue and waits for it
    int ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    fa->ClearInputData();
    fa->ClearTexData();
    fprintf(stderr, "[overlay] fonts at %.0f px (%s)\n", 22.0f * scale, body ? "Segoe UI" : "built-in");
    g_font_scale = ok ? scale : 0;
    return ok;
}

static int gm_back_to_menu(void);
// ---- the mods menu (installed mods; the browser tabs come next)
static int g_sel_mod = -1;

static void badge(const char *text, ImVec4 col)
{
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

static void layout_mods(float W, float H, float scale)
{
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 150));   // dim the game
    float w = W * 0.90f < 1800 * scale ? W * 0.90f : 1800 * scale, h = H * 0.88f;
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::Begin("##mods", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (g_font_title) ImGui::PushFont(g_font_title);
    ImGui::TextColored(kGold, "Mods");
    if (g_font_title) ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("B / Esc: close").x + ImGui::GetCursorPosX() - 6);
    ImGui::TextColored(kMuted, "B / Esc: close");
    {   /* the folder, shortened from the left to fit ("...\end\of\the\path") */
        const char *f = sw_mods_ui_folder();
        float avail = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Folder: ...").x;
        while (*f && ImGui::CalcTextSize(f).x > avail) f++;
        ImGui::TextColored(kMuted, "Folder: %s%s", f == sw_mods_ui_folder() ? "" : "...", f);
    }
    // the mod browser lives in the launcher (Mods tab > Browse); in the game: the installed mods only
    bool browse = false;

    int n = sw_mods_ui_count();
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    if (!browse) ImGui::BeginChild("##list", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (!browse) {
    if (!n) {
        ImGui::Spacing();
        ImGui::TextWrapped("No mods found.");
        ImGui::TextColored(kMuted, "Put mods into the folder above (any emulator layout: romfs/, exefs/, cheats/) and "
                                   "start the game again. Mods are off with --no-mods or when 'Load mods' is off in the "
                                   "launcher. See MODS.md.");
    }
    for (int i = 0; i < n; i++) {
        SwModUi m;
        if (!sw_mods_ui_get(i, &m)) continue;
        ImGui::PushID(i);
        bool on = m.enabled_next != 0;
        if (m.marker) ImGui::BeginDisabled();
        if (ImGui::Checkbox("##on", &on)) sw_mods_ui_set_enabled(i, on);
        if (m.marker) ImGui::EndDisabled();
        if (m.marker && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Switched off by a .disabled file in the mod folder");
        ImGui::SameLine();
        char label[256];
        snprintf(label, sizeof label, "%d. %s", i + 1, m.name);
        bool sel = g_sel_mod == i;
        if (ImGui::Selectable(label, sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(w * 0.34f, 0))) g_sel_mod = sel ? -1 : i;
        if (m.romfs_files >= 0) {
            char t[64];
            if (m.enabled_now) snprintf(t, sizeof t, "romfs %d (%d replaced, %d new)", m.romfs_files, m.romfs_replaced, m.romfs_added);
            else snprintf(t, sizeof t, "romfs %d files", m.romfs_files);
            badge(t, kMuted);
        }
        if (m.patches_applied) { char t[48]; snprintf(t, sizeof t, "patches %d", m.patches_applied); badge(t, kMuted); }
        if (m.ncheats) { char t[48]; snprintf(t, sizeof t, "cheats %d", m.ncheats); badge(t, kMuted); }
        if (m.code_patches) {
            badge("(!) code patches skipped", kWarn);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("This mod patches game code (e.g. a 60 FPS or camera patch). That cannot work in the\n"
                                  "recompiled game, so those patches are skipped - see mods.log. Use the frame rate\n"
                                  "setting of the in-game menu instead.");
        }
        if (m.enabled_now != m.enabled_next && !m.marker) badge(m.enabled_next ? "on after restart" : "off after restart", kGoldDim);
        // priority: move up / down
        float bw = ImGui::GetFrameHeight() * 1.6f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 2 * bw - ImGui::GetStyle().ItemSpacing.x);
        if (i == 0) ImGui::BeginDisabled();
        if (ImGui::Button("Up", ImVec2(bw, 0))) { sw_mods_ui_move(i, -1); if (g_sel_mod == i) g_sel_mod--; }
        if (i == 0) ImGui::EndDisabled();
        ImGui::SameLine();
        if (i == n - 1) ImGui::BeginDisabled();
        if (ImGui::Button("Down", ImVec2(bw, 0))) { sw_mods_ui_move(i, 1); if (g_sel_mod == i) g_sel_mod++; }
        if (i == n - 1) ImGui::EndDisabled();
        if (g_sel_mod == i) {                           // details: layout, cheats (live)
            ImGui::Indent(ImGui::GetFrameHeight() * 1.5f);
            ImGui::TextColored(kMuted, "%s%s%s", m.layout, m.why_off[0] ? " - " : "", m.why_off);
            for (int k = 0;; k++) {
                int con = 0;
                const char *cn = sw_mods_ui_cheat(i, k, &con);
                if (!cn) break;
                ImGui::PushID(1000 + k);
                bool c = con != 0;
                if (!m.enabled_now) ImGui::BeginDisabled();
                if (ImGui::Checkbox(cn, &c)) sw_mods_ui_set_cheat(i, k, c);
                if (!m.enabled_now) ImGui::EndDisabled();
                ImGui::PopID();
            }
            if (m.ncheats && !m.enabled_now) ImGui::TextColored(kMuted, "Cheats can be switched once the mod is loaded (restart).");
            ImGui::Unindent(ImGui::GetFrameHeight() * 1.5f);
        }
        ImGui::PopID();
    }
    bui_draw_installed_extra(scale);
    ImGui::EndChild();
    if (sw_mods_ui_restart_needed())
        ImGui::TextColored(kWarn, "Restart the game to apply mod changes (cheats switch at once). Changes are saved.");
    else
        ImGui::TextColored(kMuted, "On/off and order apply at the next start; cheats switch at once. First = highest priority.");
    ImGui::TextColored(kMuted, "A: select   B: close   D-pad / stick: move   (find new mods: launcher > Mods > Browse)");
    }
    // B / Esc: back from a sub page, else close - when nothing else wants them
    if ((ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) &&
        !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopup) && !bui_back()) {
        if (gm_back_to_menu()) sw_menu_overlay_open_page(SW_OVL_MENU);
        else sw_menu_overlay_set(0);
    }
    ImGui::End();
}


// ---- the game menu (Minus+Plus / F1): Graphics, Sound, Controls, Game ------------------------------------------
// Drawn like a page of the game's own menus: a cream journal page with a gold frame, serif title and tabs, a gold
// line under the chosen tab, a navy hint bar at the bottom.  Pad: d-pad / stick choose, left / right change, A
// select, B close, L / R tabs.  Keyboard: arrows, Enter, Esc, Q / E.  Mouse: hover, click, click the arrows.
extern "C" int sw_fps_get(void);
extern "C" void sw_fps_set(int fps);
extern "C" int sw_drawdist_get(void);        // sw_romfs.c: render distance level 0..4
extern "C" void sw_drawdist_set(int level);
extern "C" int sw_detail_get(void);          // sw_romfs.c: detail distance level 0..4
extern "C" void sw_detail_set(int level);
extern "C" int sw_npcdist_get(void);         // sw_romfs.c: people & pokemon distance 0..4
extern "C" void sw_npcdist_set(int level);
extern "C" int sw_shadow_get(void);          // sw_romfs.c: shadow quality 1..4 (next start)
extern "C" int sw_shadow_running(void);
extern "C" void sw_shadow_set(int scale);
extern "C" void sw_game_speed_set(float s);   // sw_hooks.c: world step multiplier
extern "C" float sw_game_speed_get(void);
extern "C" int sw_camera_pos(float out[3]);
extern "C" int sw_multicore(void);
static const float kSpeeds[6] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
extern "C" int sw_res_get(void);
extern "C" void sw_res_set(int scale);
extern "C" int sw_aniso_get(void);
extern "C" void sw_aniso_set(int level);
/* Presets: {resolution scale, shadow, tree, detail, simulation, texture filtering}; Simulation stays low (CPU) */
static const int kPreset[4][6] = {{1, 1, 0, 0, 0, 4}, {1, 2, 2, 2, 1, 4}, {2, 4, 4, 4, 1, 4}, {3, 4, 4, 4, 2, 4}};
static const char *const kPresetName[4] = {"Performance", "Balanced", "Quality", "Ultra (GPU)"};
static int preset_now(void)
{
    for (int i = 0; i < 4; i++) {
        const int *p = kPreset[i];
        if (sw_res_get() == p[0] && sw_shadow_get() == p[1] && sw_drawdist_get() == p[2] && sw_detail_get() == p[3] &&
            sw_npcdist_get() == p[4] && sw_aniso_get() == p[5]) return i;
    }
    return -1;
}
static void preset_apply(int i)
{
    const int *p = kPreset[i];
    sw_res_set(p[0]); sw_shadow_set(p[1]); sw_drawdist_set(p[2]); sw_detail_set(p[3]); sw_npcdist_set(p[4]); sw_aniso_set(p[5]);
}
static int speed_index(void) { float s = sw_game_speed_get(); int k = 3; for (int i = 0; i < 6; i++) if (kSpeeds[i] == s) k = i; return k; }
extern "C" int sw_dlaa_get(void);
extern "C" int sw_dlaa_state(void);
extern "C" void sw_dlaa_set(int on);
extern "C" int sw_upscale_get(void);         // sw_menu.c: picture settings
extern "C" void sw_upscale_set(int v);
extern "C" int sw_sharp_get(void);
extern "C" void sw_sharp_set(int v);
extern "C" int sw_colors_get(void);
extern "C" void sw_colors_set(int v);
extern "C" int sw_res_get(void);             // sw_menu.c: resolution scale 1..4
extern "C" void sw_res_set(int scale);
extern "C" int sw_aniso_get(void);           // sw_menu.c: anisotropic filtering 0 (game's) .. 4 (16x)
extern "C" void sw_aniso_set(int level);
extern "C" int sw_present_get(void);         // sw_menu.c: 0 V-Sync, 1 fast, 2 off
extern "C" void sw_present_set(int mode);
extern "C" int sw_fg_level(void);            // gfx/vk/sw_framegen.c: frame generation 1 (off) .. 10
extern "C" void sw_fg_set(int n);
extern "C" int sw_fg_effective(void);        // the multiplier in use (limited by the monitor)

namespace gm {
enum { T_GRAPHICS, T_WORLD, T_SOUND, T_CONTROLS, T_GAME, T_DEBUG, T_COUNT };
static const char *kTabs[T_COUNT] = {"Display", "World", "Sound", "Controls", "Game", "Debug"};
enum Kind { K_CHOICE, K_TOGGLE, K_SLIDER, K_ACTION, K_INFO };
struct Row { Kind kind; const char *label, *help; int id; };
enum {
    R_FPS, R_FRAMEGEN, R_DRAWDIST, R_DETAILDIST, R_NPCDIST, R_RES, R_DLAA, R_UPSCALE, R_SHARP, R_COLORS, R_SHADOW, R_ANISO, R_PRESENT, R_COUNTER, R_FULLSCREEN, R_VOLUME, R_SOUND, R_CONTINUE, R_MODS, R_SHOT, R_QUIT,
    R_PRESET, R_RESET, R_SPEED, R_I_CAM, R_I_CPU, R_I_GPU,
    R_I_MENU, R_I_FPS, R_I_PERF, R_I_MODS, R_I_FULL, R_I_REMAP
};
static const Row kGraphics[] = {
    {K_CHOICE, "Preset", "Sets the World and Display options together. The higher presets put the extra work on the graphics card and keep Simulation distance (processor work) low.", R_PRESET},
    {K_CHOICE, "Frame rate", "30 is the original. 60 / 90 / 120 / 240 keep the world at real-time speed.", R_FPS},
    {K_CHOICE, "Frame generation", "Shows 2x to 10x the game's frame rate with pictures made in between (changes at once). Limited by your monitor's refresh rate; adds a little input delay.", R_FRAMEGEN},
    {K_CHOICE, "Resolution scale", "Draws the 3D world and the interface at 2x to 4x the game's resolution (2x docked = 3840x2160), shown at your window's size: much sharper edges and distant detail. Costs GPU time (2x about 4 times the pixels). Changes at once.", R_RES},
    {K_CHOICE, "Anti-aliasing (DLAA)", "NVIDIA DLAA: smooth edges without shimmer, using the graphics card's AI cores (RTX cards; more GPU work). The first time it applies at the next start, then it switches at once.", R_DLAA},
    {K_CHOICE, "Upscaling", "How the picture is scaled up when your window is larger than it: FSR 1 keeps edges sharp, Bilinear is softer. Changes at once.", R_UPSCALE},
    {K_CHOICE, "Sharpness", "Sharpening after FSR upscaling (0 = none). Changes at once.", R_SHARP},
    {K_CHOICE, "Colors", "Original, or a more vivid / rich picture (saturation and contrast). Changes at once.", R_COLORS},
    {K_CHOICE, "Texture filtering", "Anisotropic filtering: ground and walls seen at a shallow angle stay sharp into the distance. 16x costs almost nothing on a PC GPU. Changes at once.", R_ANISO},
    {K_CHOICE, "V-Sync", "On: no tearing, waits for the screen. Fast: no tearing, no waiting (more GPU work). Off: lowest input delay, may tear. Changes at once.", R_PRESENT},
    {K_CHOICE, "Performance overlay", "FPS: frame rate in the corner. Detailed: 1% lows, a frame-time graph, CPU / GPU load, memory and what limits the frame rate. F10 switches it.", R_COUNTER},
    {K_TOGGLE, "Fullscreen", "The whole screen, without a window frame.", R_FULLSCREEN},
};
static const Row kWorld[] = {
    {K_CHOICE, "Tree distance", "How far away trees are drawn in full (changes at once). Higher costs performance.", R_DRAWDIST},
    {K_CHOICE, "Detail distance", "How far away rocks, props and other objects keep their full detail, and thin far objects stay visible (changes at once). Higher costs performance.", R_DETAILDIST},
    {K_CHOICE, "Simulation distance", "How far away people, Pokemon and the world around them are loaded and kept running (the Switch: 100 m), also during events, and Jubilife Village's crowd. CPU work: higher lowers the frame rate on most PCs. Changes at once.", R_NPCDIST},
    {K_CHOICE, "Shadow quality", "Sharper sun shadows: the shadow map 2x to 4x larger on each side. Applies right away.", R_SHADOW},
};
static const Row kSound[] = {
    {K_SLIDER, "Volume", "Music, voices, Pokemon cries and effects together.", R_VOLUME},
    {K_TOGGLE, "Sound", "Mute and unmute the game.", R_SOUND},
};
static const Row kControls[] = {
    {K_INFO, "Open this menu", "Minus + Plus on the controller, or F1 on the keyboard.", R_I_MENU},
    {K_INFO, "FPS counter", "Shows or hides the frame rate.", R_I_FPS},
    {K_INFO, "Performance overlay", "Off, FPS, then the detailed panel.", R_I_PERF},
    {K_INFO, "Mods", "Your installed mods, their order and their cheats.", R_I_MODS},
    {K_INFO, "Fullscreen", "Switches between the window and the whole screen.", R_I_FULL},
    {K_INFO, "Change buttons", "Every key and controller button can be remapped in the launcher.", R_I_REMAP},
};
static const Row kGame[] = {
    {K_ACTION, "Continue", "Back to the game.", R_CONTINUE},
    {K_ACTION, "Mods", "Turn mods on and off, change their order, switch cheats.", R_MODS},
    {K_ACTION, "Reset graphics to defaults", "Display and World options back to their defaults (Original distances and resolution, FSR, 16x filtering, V-Sync on, DLAA off). Frame rate and frame generation stay.", R_RESET},
    {K_ACTION, "Take a screenshot", "Saved in the screenshots folder next to the game.", R_SHOT},
    {K_ACTION, "Quit game", "Closes the game. Progress is kept only where the game saved it.", R_QUIT},
};
static const Row kDebug[] = {
    {K_CHOICE, "Game speed", "Slow motion or fast forward for the world (photos, testing). Effects and some animations keep their own pace. Back to 1x for normal play.", R_SPEED},
    {K_INFO, "Camera position", "Where the camera is in the area (x, height, z), in metres.", R_I_CAM},
    {K_INFO, "CPU mode", "How the game's threads run on your processor (launcher Options > Game).", R_I_CPU},
    {K_INFO, "GPU frame time", "Detailed numbers: the performance overlay (F10).", R_I_GPU},
};
static const Row *rows_of(int t, int *n)
{
    switch (t) {
    case T_DEBUG: *n = (int)(sizeof kDebug / sizeof *kDebug); return kDebug;
    case T_GRAPHICS: *n = (int)(sizeof kGraphics / sizeof *kGraphics); return kGraphics;
    case T_WORLD: *n = (int)(sizeof kWorld / sizeof *kWorld); return kWorld;
    case T_SOUND: *n = (int)(sizeof kSound / sizeof *kSound); return kSound;
    case T_CONTROLS: *n = (int)(sizeof kControls / sizeof *kControls); return kControls;
    default: *n = (int)(sizeof kGame / sizeof *kGame); return kGame;
    }
}
static int g_tab, g_row[T_COUNT], g_quit_armed, g_mods_from_menu, g_last_volume = 100;
static float g_tab_x0, g_tab_x1, g_open_t;           // the gold underline (eased), open fade (s)

static const char *info_value(int id)
{
    static char b[96];
    switch (id) {
    case R_I_CAM: {
        float p[3];
        if (sw_camera_pos(p)) snprintf(b, sizeof b, "%.1f   %.1f   %.1f", p[0], p[1], p[2]);
        else snprintf(b, sizeof b, "--");
        return b;
    }
    case R_I_CPU: {
        const char *c = getenv("SW_CORES");
        snprintf(b, sizeof b, "%s, %s cores", sw_multicore() ? "multi-core" : "single core", c && *c ? c : "auto");
        return b;
    }
    case R_I_GPU: return "F10";
    case R_I_MENU: return "-  +   /   F1";
    case R_I_FPS: return "F3";
    case R_I_PERF: return "F10";
    case R_I_MODS: return "F4";
    case R_I_FULL: return "F11";
    default: return "Launcher  >  Controls";
    }
}
static void value_text(const Row &r, char *buf, size_t n)
{
    switch (r.id) {
    case R_FPS: snprintf(buf, n, "%d fps", sw_fps_get() ? sw_fps_get() : 30); break;
    case R_DRAWDIST: case R_DETAILDIST: {
        static const char *k[5] = {"Original", "High (2x)", "Very high (3x)", "Ultra (5x)", "Max (10x)"};
        snprintf(buf, n, "%s", k[r.id == R_DRAWDIST ? sw_drawdist_get() : sw_detail_get()]);
        break;
    }
    case R_NPCDIST: {
        static const char *k[5] = {"Original (100 m)", "150 m", "200 m", "300 m", "Max (400 m)"};
        snprintf(buf, n, "%s", k[sw_npcdist_get()]);
        break;
    }
    case R_SHADOW: {
        int s = sw_shadow_get(), run = sw_shadow_running();
        if (s == run) snprintf(buf, n, s == 1 ? "Original" : "%dx", s);
        else snprintf(buf, n, s == 1 ? "Original (next start)" : "%dx (next start)", s);
        break;
    }
    case R_SPEED: snprintf(buf, n, "%gx", (double)kSpeeds[speed_index()]); break;
    case R_PRESET: { int k = preset_now(); snprintf(buf, n, "%s", k < 0 ? "Custom" : kPresetName[k]); break; }
    case R_DLAA: { static const char *k[4] = {"Off", "On", "On (next start)", "On (not available)"}; snprintf(buf, n, "%s", k[sw_dlaa_state()]); break; }
    case R_UPSCALE: snprintf(buf, n, "%s", sw_upscale_get() ? "FSR 1" : "Bilinear"); break;
    case R_SHARP: snprintf(buf, n, "%d", sw_sharp_get() * 10); break;
    case R_COLORS: { static const char *k[3] = {"Original", "Vivid", "Rich"}; snprintf(buf, n, "%s", k[sw_colors_get()]); break; }
    case R_RES: { static const char *k[5] = {"", "Original", "2x", "3x", "4x"}; snprintf(buf, n, "%s", k[sw_res_get()]); break; }
    case R_ANISO: { static const char *k[5] = {"Original", "2x", "4x", "8x", "16x"}; snprintf(buf, n, "%s", k[sw_aniso_get()]); break; }
    case R_PRESENT: { static const char *k[3] = {"On", "Fast", "Off"}; snprintf(buf, n, "%s", k[sw_present_get()]); break; }
    case R_FRAMEGEN: {
        int lv = sw_fg_level(), e = sw_fg_effective();
        if (lv < 2) snprintf(buf, n, "Off");
        else if (e > 1 && e < lv) snprintf(buf, n, "%dx (%dx on this monitor)", lv, e);
        else snprintf(buf, n, "%dx", lv);
        break;
    }
    case R_COUNTER: { static const char *k[3] = {"Off", "FPS", "Detailed"}; snprintf(buf, n, "%s", k[sw_menu_fps_counter() % 3]); break; }
    case R_FULLSCREEN: snprintf(buf, n, "%s", sw_menu_fullscreen_state() ? "On" : "Off"); break;
    case R_VOLUME: snprintf(buf, n, "%d%%", sw_audio_volume()); break;
    case R_SOUND: snprintf(buf, n, "%s", sw_audio_volume() > 0 ? "On" : "Off"); break;
    case R_QUIT: snprintf(buf, n, "%s", g_quit_armed ? "Press again to quit" : ""); break;
    default: buf[0] = 0; break;
    }
}
static bool toggled_on(int id)
{
    switch (id) {
    case R_FULLSCREEN: return sw_menu_fullscreen_state() != 0;
    case R_SOUND: return sw_audio_volume() > 0;
    default: return false;
    }
}
static void change(const Row &r, int dir)       // left / right
{
    switch (r.id) {
    case R_FPS: {
        static const int rates[] = {30, 60, 90, 120, 240};
        int cur = sw_fps_get() ? sw_fps_get() : 30, k = 0;
        for (int i = 0; i < 5; i++) if (rates[i] == cur) k = i;
        sw_fps_set(rates[(k + (dir < 0 ? 4 : 1)) % 5]);
        break;
    }
    case R_FRAMEGEN:
        sw_fg_set((sw_fg_level() - 1 + (dir < 0 ? 9 : 1)) % 10 + 1);   // Off, 2x .. 10x
        break;
    case R_DRAWDIST:
        sw_drawdist_set((sw_drawdist_get() + (dir < 0 ? 4 : 1)) % 5);
        break;
    case R_DETAILDIST:
        sw_detail_set((sw_detail_get() + (dir < 0 ? 4 : 1)) % 5);
        break;
    case R_NPCDIST:
        sw_npcdist_set((sw_npcdist_get() + (dir < 0 ? 4 : 1)) % 5);
        break;
    case R_SHADOW:
        sw_shadow_set((sw_shadow_get() - 1 + (dir < 0 ? 3 : 1)) % 4 + 1);   // Original, 2x, 3x, 4x
        break;
    case R_PRESET: { int k = preset_now(); preset_apply(k < 0 ? (dir < 0 ? 3 : 0) : (k + (dir < 0 ? 3 : 1)) % 4); break; }
    case R_SPEED: sw_game_speed_set(kSpeeds[(speed_index() + (dir < 0 ? 5 : 1)) % 6]); break;
    case R_DLAA: sw_dlaa_set(!sw_dlaa_get()); break;
    case R_UPSCALE: sw_upscale_set(!sw_upscale_get()); break;
    case R_SHARP: sw_sharp_set((sw_sharp_get() + (dir < 0 ? 10 : 1)) % 11); break;
    case R_COLORS: sw_colors_set((sw_colors_get() + (dir < 0 ? 2 : 1)) % 3); break;
    case R_RES:
        sw_res_set((sw_res_get() - 1 + (dir < 0 ? 3 : 1)) % 4 + 1);   // Original, 2x, 3x, 4x
        break;
    case R_ANISO:
        sw_aniso_set((sw_aniso_get() + (dir < 0 ? 4 : 1)) % 5);
        break;
    case R_PRESENT:
        sw_present_set((sw_present_get() + (dir < 0 ? 2 : 1)) % 3);
        break;
    case R_VOLUME: {
        int v = sw_audio_volume() + dir * 5;
        sw_audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
        if (sw_audio_volume() > 0) g_last_volume = sw_audio_volume();
        break;
    }
    case R_COUNTER:
        sw_menu_set_fps_counter((sw_menu_fps_counter() + (dir < 0 ? 2 : 1)) % 3);
        break;
    case R_FULLSCREEN: case R_SOUND: break;   // toggles: A or click (left / right too)
    default: return;
    }
    if (r.kind == K_TOGGLE) {
        if (r.id == R_FULLSCREEN) sw_menu_request(SW_MENU_ACT_FULLSCREEN);
        else if (r.id == R_SOUND) {
            if (sw_audio_volume() > 0) { g_last_volume = sw_audio_volume(); sw_audio_set_volume(0); }
            else sw_audio_set_volume(g_last_volume > 0 ? g_last_volume : 100);
        }
    }
}
static void activate(const Row &r)
{
    if (r.id != R_QUIT) g_quit_armed = 0;
    switch (r.kind) {
    case K_CHOICE: case K_SLIDER: change(r, 1); return;
    case K_TOGGLE: change(r, 1); return;
    case K_INFO: return;
    default: break;
    }
    switch (r.id) {
    case R_CONTINUE: sw_menu_overlay_set(0); break;
    case R_MODS: g_mods_from_menu = 1; sw_menu_overlay_open_page(SW_OVL_MODS); break;
    case R_RESET:
        preset_apply(0);                           // resolution 1x, shadows 1x, distances Original, 16x filtering
        sw_upscale_set(1); sw_sharp_set(5); sw_colors_set(0); sw_present_set(0); sw_dlaa_set(0);
        break;
    case R_SHOT: sw_menu_request(SW_MENU_ACT_SCREENSHOT); sw_menu_overlay_set(0); break;   // the shot: the game only
    case R_QUIT:
        if (g_quit_armed) sw_menu_request(SW_MENU_ACT_QUIT);
        else g_quit_armed = 1;
        break;
    }
}

// colours of the page (Hisui paper, the launcher's palette)
static ImU32 C(int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, (int)(255 * a)); }
static const int PAPER[3] = {246, 238, 218}, INK[3] = {43, 39, 33}, MUTED[3] = {139, 125, 99}, GOLD[3] = {176, 134, 47},
                 NAVY[3] = {43, 68, 112}, NAVYSOFT[3] = {221, 227, 238}, CREAM[3] = {251, 246, 234};
#define RGB3(c, a) C((c)[0], (c)[1], (c)[2], (a))

static ImVec2 text_size(ImFont *f, float size, const char *t) { return f->CalcTextSizeA(size, FLT_MAX, 0, t); }
static void text(ImDrawList *dl, ImFont *f, float size, ImVec2 pos, ImU32 col, const char *t)
{
    dl->AddText(f, size, pos, col, t);
}
static void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
}
static void badge(ImDrawList *dl, ImFont *f, float size, ImVec2 c, const char *t, bool round, float a)
{   // a pad button as in the game's hints: a cream disc / pill with the letter in navy
    ImVec2 ts = text_size(f, size, t);
    float r = size * 0.62f;
    if (round) dl->AddCircleFilled(c, r, RGB3(CREAM, a), 24);
    else dl->AddRectFilled(ImVec2(c.x - ts.x / 2 - r * 0.6f, c.y - r * 0.8f), ImVec2(c.x + ts.x / 2 + r * 0.6f, c.y + r * 0.8f),
                           RGB3(CREAM, a), r * 0.5f);
    text(dl, f, size, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), RGB3(NAVY, a), t);
}
}   // namespace gm

static void layout_menu(float W, float H, float s)
{
    using namespace gm;
    ImGuiIO &io = ImGui::GetIO();
    g_open_t += io.DeltaTime;
    float a = g_open_t / 0.16f;                                  // fade + rise in
    a = a > 1 ? 1 : a;
    float ease = 1 - (1 - a) * (1 - a) * (1 - a);
    ImFont *body = ImGui::GetFont(), *serif = g_font_serif ? g_font_serif : body,
           *big = g_font_serif_big ? g_font_serif_big : body, *small = g_font_small ? g_font_small : body;
    float fb = body->FontSize, fs = serif->FontSize, fbig = big->FontSize, fsm = small->FontSize;

    // --- input (own navigation: the window has no ImGui nav)
    int n = 0;
    const Row *rows = rows_of(g_tab, &n);
    int &sel = g_row[g_tab];
    if (sel >= n) sel = n - 1;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (pressed(ImGuiKey_GamepadL1) || pressed(ImGuiKey_Q) || pressed(ImGuiKey_PageUp)) { g_tab = (g_tab + T_COUNT - 1) % T_COUNT; g_quit_armed = 0; }
    if (pressed(ImGuiKey_GamepadR1) || pressed(ImGuiKey_E) || pressed(ImGuiKey_PageDown) || pressed(ImGuiKey_Tab)) { g_tab = (g_tab + 1) % T_COUNT; g_quit_armed = 0; }
    rows = rows_of(g_tab, &n);
    int &cur = g_row[g_tab];
    if (cur >= n) cur = n - 1;
    if (pressed(ImGuiKey_GamepadDpadUp) || pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_W)) { cur = (cur + n - 1) % n; g_quit_armed = 0; }
    if (pressed(ImGuiKey_GamepadDpadDown) || pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_S)) { cur = (cur + 1) % n; g_quit_armed = 0; }
    if (pressed(ImGuiKey_GamepadDpadLeft) || pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_A)) change(rows[cur], -1);
    if (pressed(ImGuiKey_GamepadDpadRight) || pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_D)) change(rows[cur], 1);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Space, false))
        activate(rows[cur]);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        sw_menu_overlay_set(0);
        return;
    }

    // --- the page
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(8, 10, 18, (int)(120 * ease)));   // dim the game
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##gamemenu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pw = W * 0.80f < 1000 * s ? W * 0.80f : 1000 * s, ph = H * 0.84f < 640 * s ? H * 0.84f : 640 * s;
    float x0 = (W - pw) / 2, y0 = (H - ph) / 2 + (1 - ease) * 18 * s, x1 = x0 + pw, y1 = y0 + ph;
    float R = 14 * s;
    dl->AddRectFilled(ImVec2(x0 + 8 * s, y0 + 10 * s), ImVec2(x1 + 8 * s, y1 + 10 * s), IM_COL32(0, 0, 0, (int)(70 * ease)), R);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), RGB3(PAPER, ease), R);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), RGB3(GOLD, ease), R, 0, 2.5f * s);
    float in = 9 * s;
    dl->AddRect(ImVec2(x0 + in, y0 + in), ImVec2(x1 - in, y1 - in), RGB3(GOLD, 0.45f * ease), R * 0.7f, 0, 1.0f * s);
    for (int k = 0; k < 4; k++)                                   // small gold diamonds at the inner corners
        diamond(dl, ImVec2(k & 1 ? x1 - in : x0 + in, k & 2 ? y1 - in - 58 * s : y0 + in), 5 * s, RGB3(GOLD, ease));

    // header: title, and where the menu is opened from
    float hx = x0 + 44 * s, hy = y0 + 26 * s;
    text(dl, big, fbig, ImVec2(hx, hy), RGB3(INK, ease), "Options");
    {
        const char *hint = "Minus + Plus   |   F1";
        ImVec2 ts = text_size(small, fsm, hint);
        text(dl, small, fsm, ImVec2(x1 - 44 * s - ts.x, hy + fbig - ts.y - 4 * s), RGB3(MUTED, ease), hint);
    }
    float ry = hy + fbig + 14 * s;                                 // gold rule with a centre diamond
    dl->AddLine(ImVec2(x0 + 40 * s, ry), ImVec2(x1 - 40 * s, ry), RGB3(GOLD, 0.8f * ease), 1.2f * s);
    diamond(dl, ImVec2((x0 + x1) / 2, ry), 4.5f * s, RGB3(GOLD, ease));

    // tabs, L / R on the sides, the gold underline easing to the chosen one
    float ty = ry + 18 * s, gap = 46 * s, total = 0;
    float tw[T_COUNT];
    for (int t = 0; t < T_COUNT; t++) { tw[t] = text_size(serif, fs, kTabs[t]).x; total += tw[t] + (t ? gap : 0); }
    float tx = (x0 + x1) / 2 - total / 2;
    badge(dl, small, fsm, ImVec2(tx - 40 * s, ty + fs / 2), "L", false, ease);
    badge(dl, small, fsm, ImVec2(tx + total + 40 * s, ty + fs / 2), "R", false, ease);
    float want0 = 0, want1 = 0;
    for (int t = 0; t < T_COUNT; t++) {
        bool on = t == g_tab;
        ImVec2 p(tx, ty);
        ImGui::SetCursorScreenPos(ImVec2(tx - 6 * s, ty - 4 * s));
        char id[16];
        snprintf(id, sizeof id, "##tab%d", t);
        if (ImGui::InvisibleButton(id, ImVec2(tw[t] + 12 * s, fs + 10 * s))) { g_tab = t; g_quit_armed = 0; }
        bool hov = ImGui::IsItemHovered();
        text(dl, serif, fs, p, on ? RGB3(INK, ease) : hov ? RGB3(INK, 0.75f * ease) : RGB3(MUTED, ease), kTabs[t]);
        if (on) { want0 = tx; want1 = tx + tw[t]; }
        tx += tw[t] + gap;
    }
    if (g_tab_x1 <= 0) { g_tab_x0 = want0; g_tab_x1 = want1; }
    float k = 1 - powf(0.0005f, io.DeltaTime);                   // ~0.15 s ease
    g_tab_x0 += (want0 - g_tab_x0) * k;
    g_tab_x1 += (want1 - g_tab_x1) * k;
    float uy = ty + fs + 6 * s;
    dl->AddRectFilled(ImVec2(g_tab_x0 - 4 * s, uy), ImVec2(g_tab_x1 + 4 * s, uy + 3 * s), RGB3(GOLD, ease), 2 * s);

    // rows
    rows = rows_of(g_tab, &n);
    float rx0 = x0 + 48 * s, rx1 = x1 - 48 * s, rh = 56 * s, rowy = uy + 26 * s;
    // a scrolling list: as many rows as fit above the help line; the view follows the chosen row (pad / keys), the
    // mouse wheel moves it (and the choice with it), and it slides there; gold arrows when rows are hidden
    float step = rh + 6 * s, list_y1 = y1 - 58 * s - 14 * s - 2 * fsm - 12 * s;   // room for two help lines
    int visible = (int)((list_y1 - rowy + 6 * s) / step);
    if (visible < 1) visible = 1;
    static int g_first[T_COUNT];
    static float g_first_f[T_COUNT];
    int &first = g_first[g_tab], &chosen = g_row[g_tab];
    int most = n > visible ? n - visible : 0;
    bool over_list = io.MousePos.x >= x0 && io.MousePos.x <= x1 && io.MousePos.y >= rowy && io.MousePos.y <= list_y1;
    if (over_list && io.MouseWheel != 0) {
        first -= io.MouseWheel > 0 ? 1 : -1;
        first = first < 0 ? 0 : first > most ? most : first;
        if (chosen < first) chosen = first;
        if (chosen > first + visible - 1) chosen = first + visible - 1;
    } else {
        if (chosen < first) first = chosen;
        if (chosen > first + visible - 1) first = chosen - visible + 1;
    }
    first = first < 0 ? 0 : first > most ? most : first;
    float &ff = g_first_f[g_tab];
    ff += (first - ff) * k;
    if (ff - first < 0.01f && first - ff < 0.01f) ff = (float)first;
    dl->PushClipRect(ImVec2(x0, rowy - 4 * s), ImVec2(x1, list_y1 + 4 * s), true);
    for (int i = 0; i < n; i++) {
        const Row &r = rows[i];
        float y = rowy + (i - ff) * step;
        if (y + rh < rowy - 4 * s || y > list_y1 + 4 * s) continue;         // out of the list: not drawn
        ImGui::SetCursorScreenPos(ImVec2(rx0, y));
        char id[16];
        snprintf(id, sizeof id, "##row%d", i);
        bool clicked = ImGui::InvisibleButton(id, ImVec2(rx1 - rx0, rh));
        if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            if (g_row[g_tab] != i) g_quit_armed = 0;
            g_row[g_tab] = i;
        }
        bool sel_row = g_row[g_tab] == i;
        if (sel_row) {
            dl->AddRectFilled(ImVec2(rx0, y), ImVec2(rx1, y + rh), RGB3(NAVYSOFT, ease), 10 * s);
            dl->AddRectFilled(ImVec2(rx0, y + 8 * s), ImVec2(rx0 + 4 * s, y + rh - 8 * s), RGB3(GOLD, ease), 2 * s);
        } else if (i < n - 1) {
            dl->AddLine(ImVec2(rx0 + 12 * s, y + rh + 3 * s), ImVec2(rx1 - 12 * s, y + rh + 3 * s), RGB3(GOLD, 0.25f * ease), 1);
        }
        float cy = y + rh / 2;
        ImU32 ink = r.kind == K_INFO ? RGB3(INK, 0.85f * ease) : RGB3(INK, ease);
        text(dl, body, fb, ImVec2(rx0 + 22 * s, cy - fb / 2), ink, r.label);
        char val[64];
        value_text(r, val, sizeof val);
        float vx1 = rx1 - 22 * s;
        if (r.kind == K_CHOICE) {                           // < value >
            float vw = 170 * s, vx0 = vx1 - vw;
            ImVec2 ts = text_size(body, fb, val);
            text(dl, body, fb, ImVec2(vx0 + vw / 2 - ts.x / 2, cy - fb / 2), RGB3(INK, ease), val);
            float t = 7 * s;
            dl->AddTriangleFilled(ImVec2(vx0, cy), ImVec2(vx0 + t, cy - t), ImVec2(vx0 + t, cy + t), RGB3(GOLD, ease));
            dl->AddTriangleFilled(ImVec2(vx1, cy), ImVec2(vx1 - t, cy - t), ImVec2(vx1 - t, cy + t), RGB3(GOLD, ease));
            if (clicked) change(r, io.MousePos.x < vx0 + vw / 2 ? -1 : 1);
        } else if (r.kind == K_TOGGLE) {                    // a switch: navy when on
            bool on = toggled_on(r.id);
            float sw = 58 * s, sh = 28 * s, sx0 = vx1 - sw;
            dl->AddRectFilled(ImVec2(sx0, cy - sh / 2), ImVec2(vx1, cy + sh / 2), on ? RGB3(NAVY, ease) : C(221, 210, 184, ease), sh / 2);
            float kx = on ? vx1 - sh / 2 : sx0 + sh / 2;
            dl->AddCircleFilled(ImVec2(kx, cy), sh / 2 - 3 * s, RGB3(CREAM, ease), 24);
            ImVec2 ts = text_size(body, fb, val);
            text(dl, body, fb, ImVec2(sx0 - 14 * s - ts.x, cy - fb / 2), on ? RGB3(INK, ease) : RGB3(MUTED, ease), val);
            if (clicked) change(r, 1);
        } else if (r.kind == K_SLIDER) {                    // volume: gold fill on a parchment track
            float bw = 260 * s, bx0 = vx1 - bw - 64 * s, bx1 = bx0 + bw, th = 6 * s;
            float f = sw_audio_volume() / 100.0f;
            dl->AddRectFilled(ImVec2(bx0, cy - th / 2), ImVec2(bx1, cy + th / 2), C(221, 210, 184, ease), th);
            dl->AddRectFilled(ImVec2(bx0, cy - th / 2), ImVec2(bx0 + bw * f, cy + th / 2), RGB3(GOLD, ease), th);
            dl->AddCircleFilled(ImVec2(bx0 + bw * f, cy), 10 * s, RGB3(CREAM, ease), 24);
            dl->AddCircle(ImVec2(bx0 + bw * f, cy), 10 * s, RGB3(GOLD, ease), 24, 2 * s);
            ImVec2 ts = text_size(body, fb, val);
            text(dl, body, fb, ImVec2(vx1 - ts.x, cy - fb / 2), RGB3(INK, ease), val);
            if (ImGui::IsItemActive() && io.MousePos.x >= bx0 - 10 * s && io.MousePos.x <= bx1 + 10 * s) {
                float v = (io.MousePos.x - bx0) / bw;
                int pct = (int)((v < 0 ? 0 : v > 1 ? 1 : v) * 100 + 0.5f);
                sw_audio_set_volume(pct);
            }
        } else if (r.kind == K_INFO) {                      // a key: a small cream badge, muted
            ImVec2 ts = text_size(small, fsm, info_value(r.id));
            float bx0 = vx1 - ts.x - 20 * s;
            dl->AddRectFilled(ImVec2(bx0, cy - fsm * 0.8f), ImVec2(vx1, cy + fsm * 0.8f), C(241, 227, 191, ease), 6 * s);
            text(dl, small, fsm, ImVec2(bx0 + 10 * s, cy - ts.y / 2), C(135, 99, 28, ease), info_value(r.id));
        } else {                                            // an action: a chevron, or the quit warning
            if (val[0]) {
                ImVec2 ts = text_size(body, fb, val);
                text(dl, body, fb, ImVec2(vx1 - 24 * s - ts.x, cy - fb / 2), C(168, 64, 47, ease), val);
            }
            float t = 6 * s;
            dl->AddTriangleFilled(ImVec2(vx1, cy), ImVec2(vx1 - t, cy - t), ImVec2(vx1 - t, cy + t), RGB3(GOLD, ease));
            if (clicked) { g_row[g_tab] = i; activate(r); }
        }
        if (clicked && r.kind != K_ACTION) g_row[g_tab] = i;
    }
    dl->PopClipRect();
    {   // more rows above / below: small gold arrows at the list's right edge
        float ax = rx1 + 22 * s, t = 7 * s;
        if (first > 0)
            dl->AddTriangleFilled(ImVec2(ax, rowy + 2 * s), ImVec2(ax - t, rowy + 2 * s + t), ImVec2(ax + t, rowy + 2 * s + t),
                                  RGB3(GOLD, ease));
        if (first + visible < n)
            dl->AddTriangleFilled(ImVec2(ax, list_y1 - 2 * s), ImVec2(ax - t, list_y1 - 2 * s - t),
                                  ImVec2(ax + t, list_y1 - 2 * s - t), RGB3(GOLD, ease));
    }
    // the chosen row's explanation, above the hint bar
    {
        int c = g_row[g_tab] < n ? g_row[g_tab] : 0;
        float wrap = (x1 - x0) - 140 * s;                     // long help wraps; a second line moves it up
        ImVec2 hs = small->CalcTextSizeA(fsm, FLT_MAX, wrap, rows[c].help);
        float hy2 = y1 - 58 * s - 10 * s - hs.y;
        dl->AddText(small, fsm, ImVec2(x0 + 70 * s, hy2), RGB3(MUTED, ease), rows[c].help, NULL, wrap);
    }
    // hint bar: navy, the pad buttons like the game shows them
    float by0 = y1 - 58 * s;
    dl->AddRectFilled(ImVec2(x0 + 1.5f * s, by0), ImVec2(x1 - 1.5f * s, y1 - 1.5f * s), RGB3(NAVY, ease), R,
                      ImDrawFlags_RoundCornersBottom);
    {
        struct H { const char *b; const char *t; bool round; } hs[] = {
            {"A", "Select", true}, {"B", "Back", true}, {"<  >", "Change", false}, {"L  R", "Tabs", false}};
        float hx2 = x0 + 48 * s, cy = by0 + 29 * s;
        for (const H &h : hs) {
            ImVec2 bs = text_size(small, fsm, h.b);
            float bw = h.round ? fsm * 1.24f : bs.x + fsm * 1.2f;
            badge(dl, small, fsm, ImVec2(hx2 + bw / 2, cy), h.b, h.round, ease);
            text(dl, small, fsm, ImVec2(hx2 + bw + 8 * s, cy - fsm / 2 - 1), RGB3(CREAM, ease), h.t);
            hx2 += bw + 8 * s + text_size(small, fsm, h.t).x + 34 * s;
        }
        const char *brand = "Kimo's Lodestar";
        ImVec2 ts = text_size(small, fsm, brand);
        text(dl, small, fsm, ImVec2(x1 - 44 * s - ts.x, cy - fsm / 2 - 1), RGB3(CREAM, 0.6f * ease), brand);
    }
    ImGui::End();
}

// ---- present-path hooks
extern "C" int ovl_frame(const MenuTarget *t)
{
    static long long test_at = -2;
    static uint64_t frames;
    frames++;
    if (test_at == -2) { const char *e = getenv("SW_OVL_TEST"); test_at = e ? atoll(e) : -1; }
    if (test_at >= 0 && frames >= (uint64_t)test_at) {   // SW_OVL_ROW=<n>: test, the Graphics row chosen at open
        test_at = -1; sw_menu_overlay_set(1);
        const char *r = getenv("SW_OVL_ROW");
        const char *tb = getenv("SW_OVL_TAB");   /* the tab chosen at open (0 Graphics .. 4 Debug) */
        if (tb) gm::g_tab = atoi(tb) % gm::T_COUNT;
        if (r) gm::g_row[gm::g_tab] = atoi(r);
    }
    int open = sw_menu_overlay_open();
    int hud = sw_menu_fps_counter() == 2;               // the detailed performance overlay (sw_perfhud.cpp)
    g_have = 0;
    if (!open) g_was_open = 0;
    if (!open && !hud) return 0;                        // closed: nothing to lay out or draw
    if (g_failed || !t || !t->width || !t->height) { if (open) sw_menu_overlay_set(0); return 0; }
    if (!g_ctx) {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;
        io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendPlatformName = "sw_overlay";
        style_init(g_base);
        ImGui::GetStyle() = g_base;
        QueryPerformanceFrequency(&g_qpf);
        g_ctx = 1;
    }
    /* sizes are for 1080 lines; a smaller window keeps text readable (at least 0.85), a high-DPI display scales up */
    float scale = (float)t->height / 1080.0f;
    {
        HWND fg = GetForegroundWindow();
        UINT dpi = fg ? GetDpiForWindow(fg) : 96;
        float least = 0.85f * (float)(dpi ? dpi : 96) / 96.0f;
        if (scale < least) scale = least;
    }
    scale = (float)(int)(scale * 8 + 0.5f) / 8;         // steps of 1/8: the atlas is not rebuilt for a pixel of resizing
    if (!vk_up(t) || (g_font_scale != scale && !fonts_up(scale))) {
        fprintf(stderr, "[overlay] Dear ImGui Vulkan set-up failed: overlay off for this session\n");
        g_failed = 1;
        sw_menu_overlay_set(0);
        return 0;
    }
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    ImGuiIO &io = ImGui::GetIO();
    int opening = open && !g_was_open;
    if (open) g_was_open = 1;
    if (opening) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_last.QuadPart = 0;
        AcquireSRWLockExclusive(&g_ev_lock);
        g_nev = 0;
        ReleaseSRWLockExclusive(&g_ev_lock);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = g_last.QuadPart ? (double)(now.QuadPart - g_last.QuadPart) / (double)g_qpf.QuadPart : 1.0 / 60;
    g_last = now;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    if (open) {                                         // the panel alone takes no input
        drain(io);
        pad_events(io, opening);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (opening) gm::g_open_t = 0;
    if (!open) {
    } else if (sw_menu_overlay_page() == SW_OVL_MENU) {
        gm::g_mods_from_menu = 0;
        layout_menu((float)t->width, (float)t->height, scale);
    } else {
        bui_frame(t, frames);
        layout_mods((float)t->width, (float)t->height, scale);
    }
    if (hud) {
        ImFont *body = ImGui::GetFont();
        perfhud_layout((float)t->width, (float)t->height, scale, body, g_font_small ? g_font_small : body);
    }
    ImGui::Render();
    g_have = ImGui::GetDrawData()->TotalVtxCount > 0;
    return g_have;
}

extern "C" void ovl_draw(VkCommandBuffer cb)
{
    if (!g_have || !g_vk) return;                     // exactly what ovl_frame laid out this frame
    ImDrawData *d = ImGui::GetDrawData();
    if (d && d->TotalVtxCount) ImGui_ImplVulkan_RenderDrawData(d, cb);
}

extern "C" void ovl_release(void)
{
    if (g_vk) bui_release();                        // thumbnails, before the back end goes
    if (g_vk) {
        ImGui_ImplVulkan_Shutdown();
        g_vk = 0;
        g_font_scale = 0;
    }
}

static int gm_back_to_menu(void) { return gm::g_mods_from_menu; }
