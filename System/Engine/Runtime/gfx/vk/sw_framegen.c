/* Frame generation (Arceus Recomp, built in): 2x to 10x the game's frame rate on screen.
 *
 * The game's present (vk_backend.c present_image, [arceus] hunk, on the game's thread) hands every finished picture
 * to sw_fg_capture: it is copied into a history image (rgba16f, our own: the queued present job no longer reads the
 * game's image, so the game's next submissions need not wait for it), a luma pyramid (1/4, 1/8, 1/16 size) is built
 * and the motion from the previous picture is found by block matching, coarse to fine (fg_search.comp), each level
 * median filtered.  The back end's present thread then calls sw_fg_present: N-1 generated pictures between the
 * previous and this one (fg_interp.comp: both warped along the motion to the in-between time and blended; pixels
 * that fit standing still stay, so the HUD stays sharp; where the two disagree the nearer real picture is used),
 * evenly spaced over the game's frame interval, then this picture at the time the pacer asked for.  The cost: the
 * real picture is shown (N-1)/N of a game frame later.
 *
 * Three history slots and two final motion fields: while the present thread still shows the pair (A, B), the game
 * may already capture C (the back end lets it once the job of B was taken, so A is free again by the time D comes).
 * Each thread has its own command buffers.
 *
 * N is the chosen multiplier (SW_FRAMEGEN at start, then the in-game menu; 1 = off) limited to what the monitor can
 * show: at 60 fps on a 144 Hz screen, 2x.  A pause between pictures (loading) or a first picture shows the real one
 * only. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "sw_framegen.h"
#include "sw_framegen_spv.h"

#define FMT VK_FORMAT_R16G16B16A16_SFLOAT
#define NCB 6

int sw_ingame_get(const char *key);            /* src/sw_romfs.c: Config/ingame_settings.json */
void sw_ingame_set(const char *key, int value);

typedef struct { VkImage img; VmaAllocation a; VkImageView v; int w, h; } Img;
typedef struct { int32_t size[2], full[2]; float t, radius, prior, pad; } Push;
typedef struct { VkCommandPool pool; VkCommandBuffer cb[NCB]; VkFence fence[NCB]; unsigned slot; } Ring;

static volatile int g_level = -1;              /* chosen multiplier 1..10 */
static volatile int g_eff = 1;                 /* the last one used */
static PlatMutex g_res = PLAT_MUTEX_INIT;      /* the images: a size change waits for a present sequence */

static struct {
    int ok, failed, w, h, inited, cur, have;   /* cur: history slot of the last picture; have: pictures (0..2) */
    Img hist[3], lum[3][3], flow[3], flowm[2], fin[2], out[2];   /* flowm: filtered 1/16, 1/8; fin: final per pair */
    int fin_next;
    VkSampler smp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline p_down, p_search, p_filter, p_interp;
    PFN_vkCmdPushDescriptorSetKHR push;
    Ring cap, pres;                            /* the game's thread, the present thread */
    uint64_t last_at, last_cap, T;             /* T: the game's frame interval, ns */
} G;

/* ---- settings */
int sw_fg_level(void)
{
    if (g_level < 0) {
        const char *e = getenv("SW_FRAMEGEN");
        int v = e && *e ? atoi(e) : sw_ingame_get("frame_gen");
        g_level = v < 1 ? 1 : v > 10 ? 10 : v;
    }
    return g_level;
}
void sw_fg_set(int n)
{
    g_level = n < 1 ? 1 : n > 10 ? 10 : n;
    sw_ingame_set("frame_gen", g_level);
    rt_log("framegen: %dx", g_level);
}
int sw_fg_effective(void) { return sw_fg_level() > 1 ? g_eff : 1; }

/* ---- resources */
static void img_free(Img *m)
{
    VkCtx *c = vk_ctx();
    if (m->v) vkDestroyImageView(c->device, m->v, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->a);
    memset(m, 0, sizeof *m);
}
static int img_make(Img *m, int w, int h)
{
    VkCtx *c = vk_ctx();
    m->w = w < 1 ? 1 : w, m->h = h < 1 ? 1 : h;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = FMT,
                            .extent = {(uint32_t)m->w, (uint32_t)m->h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &m->img, &m->a, NULL) != VK_SUCCESS) { m->img = VK_NULL_HANDLE; return 0; }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = FMT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &m->v) == VK_SUCCESS;
}
static VkPipeline pipe(const uint32_t *code, size_t size)
{
    VkCtx *c = vk_ctx();
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                       .layout = G.pl};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}
static int ring_make(Ring *r)
{
    VkCtx *c = vk_ctx();
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = c->gfx_family};
    if (vkCreateCommandPool(c->device, &pci, NULL, &r->pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = r->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NCB};
    if (vkAllocateCommandBuffers(c->device, &ai, r->cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < NCB; i++)
        if (vkCreateFence(c->device, &fi, NULL, &r->fence[i]) != VK_SUCCESS) return 0;
    return 1;
}
static int setup_once(void)
{
    if (G.ok) return 1;
    if (G.failed) return 0;
    VkCtx *c = vk_ctx();
    G.failed = 1;
    if (!c) return 0;
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1,
                                              .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    if (!G.push || vkCreateSampler(c->device, &si, NULL, &G.smp) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) != VK_SUCCESS)
        return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &G.pl) != VK_SUCCESS) return 0;
    G.p_down = pipe(fg_down_spv, sizeof fg_down_spv);
    G.p_search = pipe(fg_search_spv, sizeof fg_search_spv);
    G.p_filter = pipe(fg_filter_spv, sizeof fg_filter_spv);
    G.p_interp = pipe(fg_interp_spv, sizeof fg_interp_spv);
    if (!G.p_down || !G.p_search || !G.p_filter || !G.p_interp || !ring_make(&G.cap) || !ring_make(&G.pres)) return 0;
    G.failed = 0;
    G.ok = 1;
    rt_log("framegen: ready");
    return 1;
}
static void images_free(void)
{
    for (int i = 0; i < 3; i++) {
        img_free(&G.hist[i]), img_free(&G.flow[i]);
        for (int l = 0; l < 3; l++) img_free(&G.lum[i][l]);
    }
    for (int i = 0; i < 2; i++) img_free(&G.flowm[i]), img_free(&G.fin[i]), img_free(&G.out[i]);
    G.w = G.h = 0, G.inited = 0, G.have = 0;
}
/* Called on the game's thread with no present job waiting; the present thread is between pictures at most, so the
 * size change waits for the device first. */
static int images_for(int w, int h)
{
    if (G.w == w && G.h == h) return 1;
    VkCtx *c = vk_ctx();
    plat_mutex_lock(&g_res);
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    images_free();
    int ok = 1, lw[3], lh[3];
    lw[0] = (w + 3) / 4, lh[0] = (h + 3) / 4;
    for (int l = 1; l < 3; l++) lw[l] = (lw[l - 1] + 1) / 2, lh[l] = (lh[l - 1] + 1) / 2;   /* 1/4, 1/8, 1/16 */
    for (int i = 0; i < 3; i++) {
        ok &= img_make(&G.hist[i], w, h) & img_make(&G.flow[i], lw[i], lh[i]);
        for (int l = 0; l < 3; l++) ok &= img_make(&G.lum[i][l], lw[l], lh[l]);
    }
    for (int i = 0; i < 2; i++)
        ok &= img_make(&G.flowm[i], lw[i + 1], lh[i + 1]) & img_make(&G.fin[i], lw[0], lh[0]) & img_make(&G.out[i], w, h);
    if (!ok) images_free(), G.failed = 1;
    else G.w = w, G.h = h;
    plat_mutex_unlock(&g_res);
    if (!ok) { rt_log("framegen: no images for %dx%d - off", w, h); return 0; }
    rt_log("framegen: %dx%d, motion at %dx%d", w, h, lw[0], lh[0]);
    return 1;
}

/* ---- recording */
static void barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}
static void to_general(VkCommandBuffer cb, Img *m)
{
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = m->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
static void run(VkCommandBuffer cb, VkPipeline p, const Img *s0, const Img *s1, const Img *s2, const Img *dst,
                float t, float radius, float prior)
{
    VkDescriptorImageInfo ii[4] = {{G.smp, s0->v, VK_IMAGE_LAYOUT_GENERAL}, {G.smp, s1->v, VK_IMAGE_LAYOUT_GENERAL},
                                   {G.smp, s2->v, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, dst->v, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1,
                                      .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo = &ii[i]};
    Push pc = {{dst->w, dst->h}, {G.w, G.h}, t, radius, prior, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    G.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.pl, 0, 4, w);
    vkCmdPushConstants(cb, G.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (uint32_t)(dst->w + 7) / 8, (uint32_t)(dst->h + 7) / 8, 1);
    barrier_all(cb);
}
static VkCommandBuffer cb_begin(Ring *r)
{
    VkCtx *c = vk_ctx();
    r->slot = (r->slot + 1) % NCB;
    vkWaitForFences(c->device, 1, &r->fence[r->slot], VK_TRUE, 5000000000ull);
    vkResetFences(c->device, 1, &r->fence[r->slot]);
    VkCommandBuffer cb = r->cb[r->slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);   /* covers the earlier submissions on the queue (the game's writes, the last present's blit) */
    return cb;
}
static void cb_submit(Ring *r, VkCommandBuffer cb)
{
    VkCtx *c = vk_ctx();
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    vk_queue_lock();
    VkResult r2 = vkQueueSubmit2(c->gfx_queue, 1, &si, r->fence[r->slot]);
    vk_queue_unlock();
    if (r2 != VK_SUCCESS) rt_log("framegen: submit failed (%d)", (int)r2);
}

/* ---- pacing */
static void wait_until(uint64_t at)
{
    uint64_t now = kern_now_ns();
    if (at > now + 1500000) plat_wait_ns(at - now - 1000000);
    while (kern_now_ns() < at) plat_yield();
}

/* The game's thread, the picture just presented (its image in GENERAL layout, the crop {x, y, w, h} or the whole
 * w x h), due on screen at at_ns (0: no pacing), with no present job waiting.  0: frame generation is off (the
 * present goes as before); 1: *job is set for sw_fg_present and the game's image is no longer needed. */
int sw_fg_capture(VkImage src, VkFormat fmt, int w, int h, const int *crop, uint64_t at_ns, SwFgJob *job)
{
    (void)fmt;
    int level = sw_fg_level();
    if (level < 2 || !setup_once()) { G.have = 0, G.last_at = 0, g_eff = 1; return 0; }
    int cx = 0, cy = 0, cw = w, ch = h;
    if (crop && crop[2] > 0 && crop[3] > 0) cx = crop[0], cy = crop[1], cw = crop[2], ch = crop[3];
    if (!images_for(cw, ch)) return 0;
    uint64_t now = kern_now_ns(), t = at_ns ? at_ns : now, last = at_ns ? G.last_at : G.last_cap;
    uint64_t d = last && t > last ? t - last : 0;
    int gap = !d || d > 100000000ull;   /* a pause (loading, a hitch) or the first picture: no in-between pictures */
    if (!gap) G.T = G.T ? (G.T * 7 + d) / 8 : d;
    G.last_at = at_ns, G.last_cap = now;

    VkCommandBuffer cb = cb_begin(&G.cap);
    if (!G.inited) {
        for (int i = 0; i < 3; i++) {
            to_general(cb, &G.hist[i]), to_general(cb, &G.flow[i]);
            for (int l = 0; l < 3; l++) to_general(cb, &G.lum[i][l]);
        }
        for (int i = 0; i < 2; i++) to_general(cb, &G.flowm[i]), to_general(cb, &G.fin[i]), to_general(cb, &G.out[i]);
        G.inited = 1;
    }
    int prev = G.cur, cur = G.cur = (G.cur + 1) % 3;
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{cx, cy, 0}, {cx + cw, cy + ch, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{0, 0, 0}, {cw, ch, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, G.hist[cur].img, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
    barrier_all(cb);
    run(cb, G.p_down, &G.hist[cur], &G.hist[cur], &G.hist[cur], &G.lum[cur][0], 0, 1, 0);
    run(cb, G.p_down, &G.lum[cur][0], &G.lum[cur][0], &G.lum[cur][0], &G.lum[cur][1], 0, 0, 0);
    run(cb, G.p_down, &G.lum[cur][1], &G.lum[cur][1], &G.lum[cur][1], &G.lum[cur][2], 0, 0, 0);
    if (G.have < 2) G.have++;
    int n = 1;
    if (G.have == 2 && !gap && G.T) {
        double hz = gfx_window_refresh_hz(), fit = hz > 1 ? hz * (double)G.T / 1e9 + 0.15 : level;
        n = level < (int)fit ? level : (int)fit;
        if (n < 1) n = 1;
    }
    int fin = G.fin_next;
    if (n > 1) {
        G.fin_next ^= 1;
        for (int l = 2; l >= 0; l--) {   /* 1/16 (no prior, wider search), 1/8, 1/4; each median filtered */
            run(cb, G.p_search, &G.lum[cur][l], &G.lum[prev][l], l == 2 ? &G.lum[cur][l] : &G.flowm[l], &G.flow[l],
                0, l == 2 ? 6.0f : 2.0f, l == 2 ? 0.0f : 1.0f);
            run(cb, G.p_filter, &G.flow[l], &G.flow[l], &G.flow[l], l ? &G.flowm[l - 1] : &G.fin[fin], 0, 0, 0);
        }
    }
    cb_submit(&G.cap, cb);
    if (!gap && G.have == 2) {
        static int logged;
        if (n != logged)
            rt_log("framegen: %dx (chosen %dx, game %.1f fps, display %.0f Hz)", n, level,
                   G.T ? 1e9 / (double)G.T : 0.0, gfx_window_refresh_hz());
        logged = n;
        g_eff = n;
    }
    job->n = n, job->prev = prev, job->cur = cur, job->fin = fin;
    job->step = n > 1 ? G.T / (uint64_t)n : 0;
    /* the first in-between picture: one N-th of a frame after the previous real one, which was due at at - T */
    job->start = at_ns && G.T && at_ns > G.T ? at_ns - G.T + G.T / (uint64_t)n : 0;
    return 1;
}

/* Test switches: SW_FG_SHOT=<n> saves every picture shown for the n-th game picture (fg_shot_<k>.png, k = n for the
 * real one) in the working directory; SW_FG_TRACE=<n> logs the present times of 600 pictures from the n-th game picture on. */
static void shot(long n, int k)
{
    const uint8_t *px; int w, h; VkFormat f;
    if (!vk_present_captured(&px, &w, &h, &f)) return;
    uint8_t *o = malloc((size_t)w * h * 4);
    if (!o) return;
    int bgra = f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < (size_t)w * h * 4; i += 4)
        o[i] = px[i + (bgra ? 2 : 0)], o[i + 1] = px[i + 1], o[i + 2] = px[i + (bgra ? 0 : 2)], o[i + 3] = 255;
    char p[64];
    snprintf(p, sizeof p, "fg_shot_%ld_%d.png", n, k);
    rt_write_png(p, o, (uint32_t)w, (uint32_t)h);
    free(o);
}

/* The present thread: the in-between pictures, each at its time, then the real one. */
void sw_fg_present(const SwFgJob *j)
{
    static long caps, traced;
    static int trace = -1, nshots = -1;
    static long shots[16];   /* SW_FG_SHOT=<n>[,<n>...]: up to 16 game pictures */
    if (nshots < 0) {
        const char *e = getenv("SW_FG_SHOT");
        nshots = 0;
        while (e && *e && nshots < 16) {
            long v = atol(e);
            if (v > 0) shots[nshots++] = v;
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    if (trace < 0) { const char *e = getenv("SW_FG_TRACE"); trace = e ? atoi(e) : 0; }
    int sh = 0;
    caps++;
    for (int i = 0; i < nshots; i++) sh |= shots[i] == caps;
    /* a fixed timeline: a made picture whose time has passed (the game's picture came late), or that would follow the
     * last one too closely, is left out instead of pushing the rest later (bunched pictures look like stutter) */
    static uint64_t last_shown;
    uint64_t start = j->start ? j->start : kern_now_ns();
    /* Below the frame-rate cap the pacer has no slack: the game's picture is due "now" (nvn_api_obj.c pace), so the
     * in-between pictures planned before it were all in the past and every one was left out - frame generation
     * showed nothing as soon as the game missed its cap (user report 2026-11-25, high Simulation distance).  A late
     * picture starts its in-between pictures from now instead, evenly spaced (the real one then shows (N-1)/N of a
     * frame later, as it does on time).  SW_FG_REBASE=0: the old fixed timeline. */
    static int rebase = -1;
    if (rebase < 0) { const char *e = getenv("SW_FG_REBASE"); rebase = !(e && *e == '0'); }
    /* Exactly N pictures per game picture (user: "90 fps 2x must be 180"): the first in-between picture also keeps
     * one step after the last picture shown, instead of being left out when it would follow it too closely (that
     * dropped ~7% below the cap).  It moves by half a step at most: a full step made the sequence as long as a game
     * frame, the game then waited for the present thread, its frames got longer, the step with them - the game fell
     * to half its frame rate (measured 2026-11-25).  Ending (N - 1/2) steps after now leaves the game its slack. */
    if (rebase && j && j->n > 1 && j->step) {
        uint64_t now_s = kern_now_ns();
        if (start + j->step / 2 < now_s) start = now_s;
        if (last_shown && start < last_shown + j->step / 2) start = last_shown + j->step / 2;
        if (start > now_s + j->step / 2) start = now_s + j->step / 2;
    }
    plat_mutex_lock(&g_res);
    if (j->cur >= 0 && !G.hist[j->cur].img) j = NULL;   /* the images went away (a failed size change) */
    if (sh && j) vk_present_capture(1);
    for (int k = 1; j && k <= j->n; k++) {
        Img *o = k < j->n ? &G.out[k & 1] : &G.hist[j->cur];
        uint64_t due = start + (uint64_t)(k - 1) * j->step, now0 = kern_now_ns();
        if (k < j->n && (rebase ? now0 > due + j->step   /* only to catch up when a whole step behind */
                                : (now0 > due + j->step / 2 || due < last_shown + j->step / 2))) continue;
        if (k < j->n) {
            VkCommandBuffer cb = cb_begin(&G.pres);
            run(cb, G.p_interp, &G.hist[j->prev], &G.hist[j->cur], &G.fin[j->fin], o, (float)k / (float)j->n, 0, 0);
            cb_submit(&G.pres, cb);
        }
        wait_until(due);
        if (trace > 0 && caps >= trace && traced < 600) {
            static uint64_t last;
            uint64_t now = kern_now_ns();
            rt_log("framegen trace: %s %d/%d, %.2f ms after the last", k < j->n ? "made" : "real", k, j->n,
                   last ? (now - last) / 1e6 : 0.0);
            last = now, traced++;
        }
        last_shown = kern_now_ns();
        vk_present_image(o->img, VK_IMAGE_LAYOUT_GENERAL, FMT, o->w, o->h, NULL);
        if (sh) shot(caps, k);
    }
    if (sh && j) vk_present_capture(0);
    plat_mutex_unlock(&g_res);
}
