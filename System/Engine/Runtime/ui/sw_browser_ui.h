// sw_browser_ui.h - the mod browser's ImGui pages (sw_browser_ui.cpp), used by sw_overlay.cpp
#pragma once
#include <stdint.h>
#include "ui/menu.h"

void bui_frame(const MenuTarget *t, uint64_t frame);   // once per open frame, before the pages (starts the browser)
void bui_draw_browse(float scale);                       // the "Browse" tab: sources, cards, detail page, queue
void bui_draw_installed_extra(float scale);              // the "Installed" tab: mods installed by the browser
bool bui_back(void);                                     // B / Esc: closes a sub page (true) or nothing is open (false)
bool bui_subpage_open(void);
void bui_release(void);                                  // before the Vulkan device goes away
