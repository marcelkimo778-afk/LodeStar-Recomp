// sw_browser_ui.cpp - the in-game mod browser's pages (Dear ImGui), drawn inside the overlay window of
// sw_overlay.cpp: source tabs with a card grid, the detail page, the download queue, settings, and the parts of the
// "Installed" tab that come from the browser (update / uninstall / compatibility).  State and all slow work live in
// mb::Browser (mb_browser.cpp, worker threads); this file only takes snapshots and posts requests, so the render
// thread never waits for the network, the disk or an archive.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "ui/menu.h"
#include "mb_browser.h"
#include "sw_browser_ui.h"
#include "sw_mods.h"

#pragma comment(lib, "shell32.lib")

extern "C" void vk_queue_lock(void);
extern "C" void vk_queue_unlock(void);

namespace {

using namespace mb;

const ImVec4 kGold(0.91f, 0.72f, 0.33f, 1), kMuted(0.60f, 0.58f, 0.54f, 1), kWarn(0.95f, 0.62f, 0.25f, 1),
    kBad(0.90f, 0.36f, 0.30f, 1), kGood(0.45f, 0.78f, 0.45f, 1), kText(0.93f, 0.91f, 0.86f, 1);

Browser g_b;
uint64_t g_frame;
const MenuTarget *g_t;

// ================================================================== textures (thumbnails)

struct Tex {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    int w = 0, h = 0;
    uint64_t used = 0;
};
std::map<const Image *, Tex> g_tex;
VkSampler g_sampler = VK_NULL_HANDLE;
VkCommandPool g_pool = VK_NULL_HANDLE;
int g_uploads;                                       // this frame

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_t->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

void tex_free(Tex &t)
{
    VkDevice dv = g_t ? g_t->device : VK_NULL_HANDLE;
    if (!dv) return;
    if (t.ds) ImGui_ImplVulkan_RemoveTexture(t.ds);
    if (t.view) vkDestroyImageView(dv, t.view, NULL);
    if (t.img) vkDestroyImage(dv, t.img, NULL);
    if (t.mem) vkFreeMemory(dv, t.mem, NULL);
    t = Tex();
}

// one small image: staging buffer -> image, waited for here (480 x 270 at most; at most 3 per frame)
bool tex_upload(Image *im, Tex *out)
{
    VkDevice dv = g_t->device;
    if (!g_sampler) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 1;
        if (vkCreateSampler(dv, &si, NULL, &g_sampler) != VK_SUCCESS) return false;
    }
    if (!g_pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = g_t->queue_family;
        if (vkCreateCommandPool(dv, &pi, NULL, &g_pool) != VK_SUCCESS) return false;
    }
    VkDeviceSize size = (VkDeviceSize)im->w * im->h * 4;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    Tex t;
    t.w = im->w;
    t.h = im->h;
    bool ok = false;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {(uint32_t)im->w, (uint32_t)im->h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkMemoryRequirements mr;
    do {
        if (vkCreateBuffer(dv, &bi, NULL, &buf) != VK_SUCCESS) break;
        vkGetBufferMemoryRequirements(dv, buf, &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dv, &ai, NULL, &bmem) != VK_SUCCESS) break;
        vkBindBufferMemory(dv, buf, bmem, 0);
        void *p = NULL;
        if (vkMapMemory(dv, bmem, 0, size, 0, &p) != VK_SUCCESS) break;
        memcpy(p, im->rgba.data(), (size_t)size);
        vkUnmapMemory(dv, bmem);
        if (vkCreateImage(dv, &ii, NULL, &t.img) != VK_SUCCESS) break;
        vkGetImageMemoryRequirements(dv, t.img, &mr);
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dv, &ai, NULL, &t.mem) != VK_SUCCESS) break;
        vkBindImageMemory(dv, t.img, t.mem, 0);
        VkCommandBufferAllocateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = g_pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        VkCommandBuffer cb;
        if (vkAllocateCommandBuffers(dv, &ci, &cb) != VK_SUCCESS) break;
        VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &cbi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy rg = {};
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = ii.extent;
        vkCmdCopyBufferToImage(cb, buf, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cb);
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        vkCreateFence(dv, &fi, NULL, &fence);
        VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cb;
        vk_queue_lock();
        VkResult sr = vkQueueSubmit(g_t->queue, 1, &sub, fence);
        vk_queue_unlock();
        if (sr == VK_SUCCESS) vkWaitForFences(dv, 1, &fence, VK_TRUE, 2000000000ull);
        vkDestroyFence(dv, fence, NULL);
        vkFreeCommandBuffers(dv, g_pool, 1, &cb);
        if (sr != VK_SUCCESS) break;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dv, &vi, NULL, &t.view) != VK_SUCCESS) break;
        t.ds = ImGui_ImplVulkan_AddTexture(g_sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ok = t.ds != VK_NULL_HANDLE;
    } while (0);
    if (buf) vkDestroyBuffer(dv, buf, NULL);
    if (bmem) vkFreeMemory(dv, bmem, NULL);
    if (!ok) tex_free(t);
    else *out = t;
    return ok;
}

// the texture of an image, uploading it when it has arrived; NULL while loading / on error
ImTextureID texture(Image *im)
{
    if (!im || im->state != ST_OK) return (ImTextureID)0;
    auto it = g_tex.find(im);
    if (it != g_tex.end()) { it->second.used = g_frame; return (ImTextureID)(uintptr_t)it->second.ds; }
    if (im->rgba.empty() || g_uploads >= 3) return (ImTextureID)0;
    g_uploads++;
    Tex t;
    if (!tex_upload(im, &t)) { im->state = ST_ERROR; return (ImTextureID)0; }
    t.used = g_frame;
    g_tex[im] = t;
    std::vector<uint8_t>().swap(im->rgba);       // the pixels live on the GPU now
    im->rgba.clear();
    return (ImTextureID)(uintptr_t)t.ds;
}

// textures not drawn for a while go (a few frames later than their last use: frames in flight)
void tex_collect()
{
    if (g_tex.size() < 120) return;
    for (auto it = g_tex.begin(); it != g_tex.end();) {
        if (g_frame - it->second.used > 240) {
            Image *im = const_cast<Image *>(it->first);
            tex_free(it->second);
            im->state = ST_IDLE;                       // drawn again: requested again (cache on disk)
            it = g_tex.erase(it);
        } else ++it;
    }
}

// ================================================================== helpers

void open_url(const std::string &u)
{
    if (lower(u).compare(0, 8, "https://") != 0) return;       // links to web pages only, never files or programs
    ShellExecuteW(NULL, L"open", wide(u).c_str(), NULL, NULL, SW_SHOWNORMAL);
}

std::string fmt_count(uint64_t n)
{
    char b[32];
    if (n >= 1000000) snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else if (n >= 10000) snprintf(b, sizeof b, "%.0fk", n / 1e3);
    else if (n >= 1000) snprintf(b, sizeof b, "%.1fk", n / 1e3);
    else snprintf(b, sizeof b, "%llu", (unsigned long long)n);
    return b;
}

std::string fmt_size(uint64_t n)
{
    char b[32];
    if (!n) return "?";
    if (n >= (1ull << 30)) snprintf(b, sizeof b, "%.1f GB", n / 1073741824.0);
    else if (n >= (1ull << 20)) snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    else snprintf(b, sizeof b, "%.0f KB", n / 1024.0 + 0.5);
    return b;
}

std::string target_version() { return g_b.paths().target.version.empty() ? std::string("1.0.0") : g_b.paths().target.version; }

// the compatibility badge (spec section 6)
void compat_badge(const VersionInfo &v, bool small_text = false)
{
    (void)small_text;
    if (v.compat == COMPAT_MATCH) ImGui::TextColored(kGood, "v%s", target_version().c_str());
    else if (v.compat == COMPAT_MISMATCH) ImGui::TextColored(kBad, "Wrong version (%s)", v.detected.empty() ? "?" : v.detected.c_str());
    else ImGui::TextColored(kWarn, "Unknown version");
    if (ImGui::IsItemHovered() && !v.why.empty()) ImGui::SetTooltip("%s", v.why.c_str());
}

void compat_badge_meta(const InstalledMeta &m)
{
    VersionInfo v;
    v.compat = m.compat == "match" ? COMPAT_MATCH : m.compat == "mismatch" ? COMPAT_MISMATCH : COMPAT_UNKNOWN;
    v.detected = m.detected_version;
    v.why = m.compat_why;
    compat_badge(v);
}

// ================================================================== page state

int g_tab = -1;                                     // selected source tab
int g_tab_request = -1;                             // L / R switches tabs
bool g_detail;                                      // the detail page is open
int g_detail_img;                                   // carousel position
char g_query[128], g_category[64], g_link[1024];
int g_sort;
bool g_settings_open;
std::string g_status;
int g_confirm_job;                                  // "install anyway?" for this job (0 = none)
bool g_notice_ack;

BrowserPaths make_paths()
{
    BrowserPaths p;
    std::string exe = sw_mods_host_exe_dir();
    std::string cfg = join(exe, "../../Config");
    p.settings_file = is_dir(cfg) ? join(cfg, "modbrowser.json") : join(exe, "modbrowser.json");
    const char *se = getenv("SW_MB_SETTINGS");          // tests: another settings file
    if (se && *se) p.settings_file = se;
    p.mods_root = sw_mods_ui_folder();
    make_dirs(p.mods_root);
    p.cache_dir = join(exe, "mod_cache");
    const char *mock = getenv("SW_MB_MOCK");
    if (mock && *mock) p.mock_dir = mock;
    SwModuleInfo mi;
    if (sw_mods_host_module(0, &mi) && mi.module_id) p.target.build_id = mi.module_id;   // from the recompiled NSO
    p.target.version = "1.0.0";                      // a setting (Config/modbrowser.json "target_version")
    return p;
}

void ensure_started()
{
    if (!g_b.started()) g_b.start(make_paths());
}

void do_search(int ti, bool append)
{
    TabView t = g_b.tab(ti);
    SearchQuery q;
    q.text = g_query;
    q.category = g_category;
    q.sort = g_sort;
    q.per_page = 24;
    q.page = append ? t.query.page + 1 : 1;
    g_b.search(ti, q, append);
}

// ================================================================== cards

void card(int ti, const TabView &t, size_t i, float cw, float scale)
{
    const ModSummary &m = t.mods[i];
    const VersionInfo &cv = t.compat[i];
    ImGui::PushID((int)i);
    const ImGuiStyle &st = ImGui::GetStyle();
    float iw = cw - st.WindowPadding.x * 2, ih = iw * 9 / 16;
    if (ih > 150 * scale) ih = 150 * scale;
    // height from the content: picture, name (up to 2 lines), author, numbers, badges, button
    float card_h = st.WindowPadding.y * 2 + ih + st.ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing() * 6 +
                   ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##card", ImVec2(cw, card_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImTextureID tx = texture(g_b.image(ti, m.thumb_url));
    if (tx) ImGui::Image(tx, ImVec2(iw, ih));
    else {
        ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + iw, p0.y + ih), IM_COL32(30, 26, 20, 255), 6);
        ImGui::Dummy(ImVec2(iw, ih));
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + iw);
    ImGui::TextColored(kText, "%s", m.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::TextColored(kMuted, "by %s", m.author.empty() ? "?" : m.author.c_str());
    ImGui::TextColored(kMuted, "%s downloads  %s likes  %s", fmt_count(m.downloads).c_str(), fmt_count(m.likes).c_str(),
                       m.version.empty() ? "" : ("v" + m.version).c_str());
    compat_badge(cv);
    std::string iv;
    if (g_b.installed_state(m.source, m.id, &iv)) {
        ImGui::SameLine();
        if (!m.version.empty() && version_newer(m.version, iv)) ImGui::TextColored(kGold, "Update available");
        else ImGui::TextColored(kGood, "Installed");
    }
    // the whole card opens the detail page; A on it too
    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##open", ImVec2(iw, ih))) {
        g_b.open_details(ti, m.id);
        g_detail = true;
        g_detail_img = 0;
    }
    ImGui::SetCursorPosY(card_h - ImGui::GetFrameHeightWithSpacing() - st.WindowPadding.y + st.ItemSpacing.y);
    if (ImGui::Button("Details", ImVec2(iw, 0))) {
        g_b.open_details(ti, m.id);
        g_detail = true;
        g_detail_img = 0;
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void draw_grid(int ti, float scale)
{
    TabView t = g_b.tab(ti);
    if (t.state == ST_IDLE && t.available) { do_search(ti, false); t = g_b.tab(ti); }
    if (!t.available) {
        ImGui::Spacing();
        ImGui::TextColored(kWarn, "%s", t.why.c_str());
        ImGui::TextColored(kMuted, "Open Settings (top right) to set it up.");
        return;
    }
    if (!t.verified)
        ImGui::TextColored(kMuted, "This source is not verified against its live API yet (see MODS.md) - if it shows "
                                   "errors, the other sources keep working.");
    if (t.state == ST_ERROR) {
        ImGui::TextColored(kBad, "%s", t.error.c_str());
        if (ImGui::Button("Try again")) do_search(ti, false);
        return;
    }
    bool only = g_b.settings().only_compatible;
    float avail = ImGui::GetContentRegionAvail().x;
    float cw = 300 * scale, gap = ImGui::GetStyle().ItemSpacing.x;
    int cols = std::max(1, (int)((avail + gap) / (cw + gap)));
    cw = (avail - gap * (cols - 1)) / cols;
    int shown = 0;
    for (size_t i = 0; i < t.mods.size(); i++) {
        if (only && t.compat[i].compat == COMPAT_MISMATCH) continue;
        if (shown % cols) ImGui::SameLine();
        card(ti, t, i, cw, scale);
        shown++;
    }
    if (t.state == ST_LOADING) ImGui::TextColored(kMuted, "Loading...");
    else if (!shown) ImGui::TextColored(kMuted, only && !t.mods.empty() ? "Nothing compatible with v%s on this page - "
                                                                         "switch off the filter to see the rest." : "No mods found.",
                                        target_version().c_str());
    if (t.state == ST_OK && t.more && ImGui::Button("Load more")) do_search(ti, true);
}

// ================================================================== detail page

void draw_detail(float scale)
{
    DetailView d = g_b.details();
    if (ImGui::Button("< Back")) {                  // B goes back too (sw_overlay.cpp -> bui_back)
        g_detail = false;
        return;
    }
    if (d.state == ST_LOADING) { ImGui::TextColored(kMuted, "Loading..."); return; }
    if (d.state == ST_ERROR) { ImGui::TextColored(kBad, "%s", d.error.c_str()); return; }
    const ModDetails &m = d.mod;
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%s", m.name.c_str());
    float avail = ImGui::GetContentRegionAvail().x;
    float iw = std::min(avail * 0.5f, 720 * scale), ih = iw * 9 / 16;
    ImGui::BeginGroup();
    if (!m.images.empty()) {
        g_detail_img = (g_detail_img % (int)m.images.size() + (int)m.images.size()) % (int)m.images.size();
        ImTextureID tx = texture(g_b.image(d.tab, m.images[(size_t)g_detail_img]));
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (tx) ImGui::Image(tx, ImVec2(iw, ih));
        else {
            ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + iw, p0.y + ih), IM_COL32(30, 26, 20, 255), 6);
            ImGui::Dummy(ImVec2(iw, ih));
        }
        if (m.images.size() > 1) {
            if (ImGui::Button("<")) g_detail_img--;
            ImGui::SameLine();
            ImGui::Text("%d / %d", g_detail_img + 1, (int)m.images.size());
            ImGui::SameLine();
            if (ImGui::Button(">")) g_detail_img++;
        }
    } else ImGui::Dummy(ImVec2(iw, 1));
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("by %s", m.author.empty() ? "?" : m.author.c_str());
    ImGui::TextColored(kMuted, "%s downloads   %s likes   %s", fmt_count(m.downloads).c_str(), fmt_count(m.likes).c_str(),
                       m.version.empty() ? "" : ("version " + m.version).c_str());
    compat_badge(d.compat);
    if (!m.page_url.empty() && ImGui::Button("Open the mod's page in the browser")) open_url(m.page_url);
    ImGui::TextColored(kMuted, "Made by its author - all credit to them. Installed at your own risk.");
    std::string iv;
    bool inst = g_b.installed_state(m.source, m.id, &iv) != 0;
    if (inst) {
        ImGui::TextColored(kGood, "Installed (version %s)", iv.empty() ? "?" : iv.c_str());
        for (auto &i : g_b.installed())
            if (i.meta.source == m.source && i.meta.mod_id == m.id) {
                ImGui::SameLine();
                if (ImGui::Button("Uninstall")) g_b.uninstall(i.meta.folder, &g_status);
                break;
            }
    }
    ImGui::EndGroup();
    ImGui::Separator();
    // files: one install button each
    ImGui::TextColored(kGold, "Files");
    if (m.files.empty()) ImGui::TextColored(kMuted, "No files.");
    for (size_t k = 0; k < m.files.size(); k++) {
        const ModFile &f = m.files[k];
        ImGui::PushID((int)k);
        bool upd = inst && !f.version.empty() && version_newer(f.version, iv);
        if (ImGui::Button(upd ? "Update" : inst ? "Reinstall" : "Install", ImVec2(140 * scale, 0))) {
            g_b.install(d.tab, m, f, false);
            g_status = "Queued: " + m.name;
        }
        ImGui::SameLine();
        ImGui::Text("%s", f.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s%s%s", fmt_size(f.size).c_str(), f.version.empty() ? "" : "  v", f.version.c_str());
        if (!f.description.empty()) { ImGui::SameLine(); ImGui::TextColored(kMuted, "- %s", f.description.c_str()); }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::BeginChild("##desc", ImVec2(0, 0), 0);
    ImGui::TextWrapped("%s", m.description.empty() ? "(no description)" : m.description.c_str());
    if (!m.changelog.empty() && ImGui::CollapsingHeader("Changelog")) ImGui::TextWrapped("%s", m.changelog.c_str());
    ImGui::EndChild();
}

// ================================================================== settings

char g_nexus_key[128], g_modio_key[128], g_modio_game[32], g_nexus_domain[64], g_gb_game[32], g_repos[1024], g_target[16];
bool g_settings_loaded;

void draw_settings(float scale)
{
    BrowserSettings s = g_b.settings();
    if (!g_settings_loaded) {
        g_settings_loaded = true;
        snprintf(g_nexus_key, sizeof g_nexus_key, "%s", s.nexus_key.c_str());
        snprintf(g_modio_key, sizeof g_modio_key, "%s", s.modio_key.c_str());
        snprintf(g_modio_game, sizeof g_modio_game, "%s", s.modio_game_id.c_str());
        snprintf(g_nexus_domain, sizeof g_nexus_domain, "%s", s.nexus_game_domain.c_str());
        snprintf(g_gb_game, sizeof g_gb_game, "%s", s.gamebanana_game_id.c_str());
        snprintf(g_target, sizeof g_target, "%s", s.target_version.c_str());
        std::string r;
        for (auto &x : s.github_repos) r += x + "\n";
        snprintf(g_repos, sizeof g_repos, "%s", r.c_str());
    }
    if (ImGui::Button("< Back")) { g_settings_open = false; return; }
    ImGui::SameLine();
    ImGui::TextColored(kGold, "Mod browser settings");
    ImGui::TextColored(kMuted, "Stored in %s on this PC only. Keys are sent only to their own site and never logged.",
                       g_b.paths().settings_file.c_str());
    float w = 420 * scale;
    ImGui::SeparatorText("GameBanana");
    ImGui::Checkbox("Use GameBanana", &s.gamebanana);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("Game id (the number in gamebanana.com/games/<id>)", g_gb_game, sizeof g_gb_game, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SeparatorText("Nexus Mods");
    ImGui::Checkbox("Use Nexus Mods", &s.nexus);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("Personal API key (nexusmods.com > Site preferences > API)", g_nexus_key, sizeof g_nexus_key, ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("Game domain (the name in nexusmods.com/<domain>/mods)", g_nexus_domain, sizeof g_nexus_domain);
    ImGui::TextColored(kMuted, "Free accounts: Install opens the download page in your browser; then use Install from file / nxm link.");
    ImGui::SeparatorText("mod.io (optional)");
    ImGui::Checkbox("Use mod.io", &s.modio);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("API key", g_modio_key, sizeof g_modio_key, ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("Game id", g_modio_game, sizeof g_modio_game, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SeparatorText("GitHub releases");
    ImGui::Checkbox("Use GitHub", &s.github);
    ImGui::InputTextMultiline("Repositories (owner/repo, one per line)", g_repos, sizeof g_repos, ImVec2(w, 90 * scale));
    ImGui::SeparatorText("General");
    ImGui::Checkbox("Check installed mods for updates when the browser opens", &s.check_updates);
    ImGui::Checkbox("Only show mods compatible with the target version", &s.only_compatible);
    ImGui::SetNextItemWidth(160 * scale);
    ImGui::InputText("Target game version (the version you dumped)", g_target, sizeof g_target);
    if (ImGui::Button("Save settings", ImVec2(240 * scale, 0))) {
        s.nexus_key = trim(g_nexus_key);
        s.modio_key = trim(g_modio_key);
        s.modio_game_id = trim(g_modio_game);
        s.nexus_game_domain = trim(g_nexus_domain);
        s.gamebanana_game_id = trim(g_gb_game);
        s.target_version = normalize_game_version(g_target).empty() ? s.target_version : normalize_game_version(g_target);
        s.github_repos.clear();
        std::string cur;
        for (const char *p = g_repos;; p++) {
            if (!*p || *p == '\n' || *p == ',') {
                std::string r = trim(cur);
                if (r.find('/') != std::string::npos) s.github_repos.push_back(r);
                cur.clear();
                if (!*p) break;
            } else cur += *p;
        }
        g_b.set_settings(s);
        g_status = "Settings saved (the target version applies after a restart).";
        g_settings_open = false;
    } else {
        // checkboxes apply at once (without the text fields)
        BrowserSettings old = g_b.settings();
        if (old.gamebanana != s.gamebanana || old.nexus != s.nexus || old.modio != s.modio || old.github != s.github ||
            old.check_updates != s.check_updates || old.only_compatible != s.only_compatible)
            g_b.set_settings(s);
    }
}

// ================================================================== the download queue (bottom strip)

void draw_jobs(float scale)
{
    std::vector<JobView> js = g_b.jobs();
    if (js.empty()) return;
    ImGui::SeparatorText("Downloads");
    for (const auto &j : js) {
        ImGui::PushID(j.id);
        ImGui::TextColored(j.state == JOB_FAILED ? kBad : j.state == JOB_DONE ? kGood : kText, "%s", j.title.c_str());
        ImGui::SameLine();
        if (!j.finished()) {
            float frac = j.total ? (float)((double)j.done / (double)j.total) : 0.f;
            char ov[64];
            snprintf(ov, sizeof ov, "%s %s", job_state_name(j.state), j.total ? fmt_size(j.done).c_str() : "");
            ImGui::ProgressBar(j.state == JOB_DOWNLOAD ? frac : -1.0f * (float)ImGui::GetTime(), ImVec2(260 * scale, 0), ov);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) g_b.cancel(j.id);
        } else {
            ImGui::TextColored(j.state == JOB_DONE ? kGood : j.state == JOB_BLOCKED ? kWarn : kMuted, "%s", j.message.c_str());
            if (j.state == JOB_BLOCKED) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Install anyway...")) { g_confirm_job = j.id; ImGui::OpenPopup("##anyway"); }
            }
            if (j.state == JOB_BROWSER && !j.browser_url.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open download page")) open_url(j.browser_url);
            }
            if (!j.refused.empty()) {
                ImGui::TextColored(kWarn, "   Refused (programs / scripts are never installed):");
                for (const auto &r : j.refused) { ImGui::SameLine(); ImGui::TextColored(kWarn, "%s", r.c_str()); }
            }
            for (const auto &w : j.warnings) ImGui::TextColored(kWarn, "   %s", w.c_str());
        }
        if (ImGui::BeginPopupModal("##anyway", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(kWarn, "Install a mod made for another game version?");
            ImGui::TextWrapped("Its exefs patches or cheats name another build of the game. They will not match this "
                               "game: the mod loader skips them (with a note in mods.log), so usually only its romfs files "
                               "take effect, and those may not fit this version either. Install only if you know it works.");
            if (ImGui::Button("Install anyway")) {
                g_b.install_anyway(g_confirm_job);
                g_confirm_job = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { g_confirm_job = 0; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Clear finished")) g_b.clear_finished();
}

}  // namespace

// ================================================================== entry points (sw_overlay.cpp)

// test hooks (real-game checks without input): SW_MB_TEST=detail:<mod id>@<frame>,install@<frame>,uninstall@<frame>
// (frames counted from the overlay's first open frame)
static void test_hooks(uint64_t n)
{
    static std::string spec;
    static bool parsed;
    if (!parsed) { parsed = true; const char *e = getenv("SW_MB_TEST"); if (e) spec = e; }
    if (spec.empty()) return;
    size_t s0 = 0;
    while (s0 < spec.size()) {
        size_t e = spec.find(',', s0);
        if (e == std::string::npos) e = spec.size();
        std::string item = spec.substr(s0, e - s0);
        s0 = e + 1;
        size_t at = item.find('@');
        if (at == std::string::npos || (uint64_t)atoll(item.c_str() + at + 1) != n) continue;
        std::string act = item.substr(0, at);
        if (act.compare(0, 7, "detail:") == 0) { g_b.open_details(g_tab < 0 ? 0 : g_tab, act.substr(7)); g_detail = true; }
        else if (act == "install") {
            DetailView d = g_b.details();
            if (d.state == ST_OK && !d.mod.files.empty()) g_b.install(d.tab, d.mod, d.mod.files[0], false);
            fprintf(stderr, "[modbrowser] test: install %s\n", d.state == ST_OK ? d.mod.name.c_str() : "(no detail page)");
        } else if (act == "uninstall") {
            for (auto &i : g_b.installed()) { g_b.uninstall(i.meta.folder, &g_status); fprintf(stderr, "[modbrowser] test: uninstall %s: %s\n", i.meta.name.c_str(), g_status.c_str()); break; }
        } else if (act == "back") g_detail = false;
    }
}

void bui_frame(const MenuTarget *t, uint64_t frame)
{
    static uint64_t open_frames;
    g_t = t;
    g_frame = frame;
    g_uploads = 0;
    ensure_started();
    tex_collect();
    test_hooks(++open_frames);
}

bool bui_back()
{
    if (g_detail) { g_detail = false; return true; }
    if (g_settings_open) { g_settings_open = false; return true; }
    return false;
}

bool bui_subpage_open() { return g_detail || g_settings_open; }

void bui_draw_browse(float scale)
{
    // the one-time notice
    BrowserSettings s = g_b.settings();
    if (!s.notice_shown && !g_notice_ack) {
        ImGui::Spacing();
        ImGui::TextColored(kGold, "Before you browse");
        ImGui::TextWrapped("Mods are made by third parties. They are not checked by this project, and you install them at "
                           "your own risk. The browser only talks to the sources you switch on, shows each mod's author and "
                           "links back to its page, and never installs programs or scripts. Code patches (most \"60 FPS\" / "
                           "camera patches) cannot work in this recompiled game and are skipped.");
        if (ImGui::Button("I understand", ImVec2(220 * scale, 0))) {
            g_notice_ack = true;
            s.notice_shown = true;
            g_b.set_settings(s);
        }
        return;
    }
    if (g_settings_open) { draw_settings(scale); return; }
    int n = g_b.tab_count();
    if (g_tab < 0 || g_tab >= n) g_tab = 0;
    if (g_detail) { draw_detail(scale); draw_jobs(scale); return; }
    // top bar: source tabs, search, sort, category, filter, settings
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_tab_request = (g_tab + n - 1) % n;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_tab_request = (g_tab + 1) % n;
    if (ImGui::BeginTabBar("##sources")) {
        for (int i = 0; i < n; i++) {
            TabView t = g_b.tab(i);
            ImGuiTabItemFlags f = g_tab_request == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem((t.title + "###src" + std::to_string(i)).c_str(), NULL, f)) {
                if (g_tab != i) g_tab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    g_tab_request = -1;
    ImGui::SetNextItemWidth(320 * scale);
    bool go = ImGui::InputTextWithHint("##q", "Search mods", g_query, sizeof g_query, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    static const char *sorts[] = {"Trending", "Most popular", "Newest", "Recently updated"};
    ImGui::SetNextItemWidth(220 * scale);
    if (ImGui::Combo("##sort", &g_sort, sorts, 4)) go = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180 * scale);
    go |= ImGui::InputTextWithHint("##cat", "Category", g_category, sizeof g_category, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Search")) go = true;
    ImGui::SameLine();
    bool only = s.only_compatible;
    char lbl[64];
    snprintf(lbl, sizeof lbl, "Only compatible with v%s", target_version().c_str());
    if (ImGui::Checkbox(lbl, &only)) { s.only_compatible = only; g_b.set_settings(s); }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 120 * scale);
    if (ImGui::Button("Settings", ImVec2(120 * scale, 0))) { g_settings_open = true; g_settings_loaded = false; }
    if (go) do_search(g_tab, false);
    // install from a file or link (local zip / 7z / rar, https link, nxm:// link)
    ImGui::SetNextItemWidth(560 * scale);
    ImGui::InputTextWithHint("##link", "Install from file or link: C:\\path\\mod.zip, https://..., nxm://...", g_link, sizeof g_link);
    ImGui::SameLine();
    if (ImGui::Button("Install from file or link")) {
        int id = g_b.install_local(g_link, false);
        g_status = id ? "Queued." : "Not a readable file, an https:// link or an nxm:// link.";
        if (id) g_link[0] = 0;
    }
    if (!g_status.empty()) ImGui::TextColored(kMuted, "%s", g_status.c_str());
    float jobs_h = g_b.jobs().empty() ? 0 : std::min(240 * scale, (float)(g_b.jobs().size() * 2 + 2) * ImGui::GetTextLineHeightWithSpacing());
    ImGui::BeginChild("##grid", ImVec2(0, -jobs_h), 0);
    draw_grid(g_tab, scale);
    ImGui::EndChild();
    draw_jobs(scale);
}

// the Installed tab's browser part: mods installed by the browser, with update / uninstall / compatibility
void bui_draw_installed_extra(float scale)
{
    std::vector<InstalledView> iv = g_b.installed();
    if (iv.empty()) return;
    ImGui::SeparatorText("Installed from the browser");
    for (size_t i = 0; i < iv.size(); i++) {
        const InstalledView &m = iv[i];
        ImGui::PushID((int)i + 5000);
        bool on = !m.disabled;
        if (ImGui::Checkbox("##on", &on)) g_b.set_enabled(m.meta.folder, on);
        ImGui::SameLine();
        ImGui::Text("%s", m.meta.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s  v%s  from %s", m.meta.author.c_str(), m.meta.version.c_str(), m.meta.source.c_str());
        ImGui::SameLine();
        compat_badge_meta(m.meta);
        if (!m.latest.empty()) { ImGui::SameLine(); ImGui::TextColored(kGold, "Update available: %s", m.latest.c_str()); }
        float bw = 130 * scale;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 2 * bw - ImGui::GetStyle().ItemSpacing.x);
        if (!m.meta.url.empty()) { if (ImGui::Button("Page", ImVec2(bw, 0))) open_url(m.meta.url); }
        else ImGui::Dummy(ImVec2(bw, 0));
        ImGui::SameLine();
        if (ImGui::Button("Uninstall", ImVec2(bw, 0))) ImGui::OpenPopup("##uninst");
        if (ImGui::BeginPopupModal("##uninst", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Remove \"%s\"?", m.meta.name.c_str());
            ImGui::TextColored(kMuted, "Only its folder is deleted: %s", m.meta.folder.c_str());
            if (ImGui::Button("Remove")) { g_b.uninstall(m.meta.folder, &g_status); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Check for updates")) g_b.check_updates();
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "Changes here apply at the next start. %s", g_status.c_str());
}

void bui_release(void)
{
    for (auto &kv : g_tex) tex_free(kv.second);
    g_tex.clear();
    if (g_t && g_sampler) vkDestroySampler(g_t->device, g_sampler, NULL);
    if (g_t && g_pool) vkDestroyCommandPool(g_t->device, g_pool, NULL);
    g_sampler = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
}
