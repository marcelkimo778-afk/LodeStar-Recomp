// sw_perfhud.cpp - the detailed performance overlay ("Performance overlay: Detailed", F10).
//
// Modes (src/sw_menu.c keeps the mode: sw_menu_fps_counter): 0 off, 1 the small FPS line (gfx/vk/sw_menu_vk.c draws
// it), 2 this panel.  F3 switches between off and the FPS line, F10 goes off -> FPS -> detailed -> off.
//
// What it shows and where it comes from:
//   frame rate, average / 1% low / 0.1% low / worst over the last 10 s, a frame-time graph - the presents
//       (perfhud_present, called for every present by sw_menu_vk.c fps_measure; 1% low = the 99th percentile frame
//       time as fps, the way bench.py reports it);
//   GPU ms per frame and GPU busy % - the renderer's own timestamps (gfx/vk/vk_sync.c), switched on only while this
//       panel is shown (perf_gpu_timing in gfx/sw_gfx_glue.c asks sw_perfhud_gpu_wanted);
//   CPU % of the machine and of the game, the busiest thread (the game's bottleneck when it is near 100 %),
//       RAM, VRAM (vk_mem_vram) - a sampler thread, twice a second, running only while the panel is shown;
//   shaders compiled (perf_count PERF_CNT_PIPE_BUILT) - a new pipeline is the usual cause of a one-off hitch.
//   "Limited by": a guess from those numbers (GPU busy, the frame cap, one thread at its limit).
//
// Costs: off or FPS only - one array write per present.  Detailed - a timestamp pair per command buffer and a thread
// snapshot twice a second (on its own thread).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <unordered_map>

#include "imgui.h"
#include "sw_menu.h"

extern "C" int sw_fps_get(void);                 // src/sw_hooks.c: the frame-rate target (0 = 30)
extern "C" int sw_fg_effective(void);            // gfx/vk/sw_framegen.c: frame generation multiplier in use
extern "C" uint64_t perf_gpu_busy_total(void);   // gfx/sw_gfx_glue.c: GPU busy ns so far (timestamps)
extern "C" int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact);
extern "C" uint64_t sw_perf_count_get(int kind); // gfx/sw_gfx_glue.c
extern "C" double sw_host_wait_ms(unsigned long tid);
extern "C" int sw_res_get(void);                 // src/sw_menu.c: resolution scale
extern "C" int sw_dlaa_state(void);              // src/sw_menu.c: 1 = DLAA running   // src/sw_thread.c: guest waits of a host thread (spin + sleep)
enum { PERF_CNT_PIPE_ASYNC_ = 0, PERF_CNT_PIPE_BUILT_ = 7 };   // gfx/compat/host.h order

static int hud_mode(void) { return sw_menu_fps_counter(); }
extern "C" int sw_perfhud_gpu_wanted(void) { return hud_mode() == 2; }

// ---- presents (present thread only)
enum { RING = 4096, GRAPH = 240 };
static float g_ms[RING];          // frame times, ms
static uint32_t g_n;              // presents so far
static LARGE_INTEGER g_qpf;

extern "C" void perfhud_present(double ms)
{
    g_ms[g_n % RING] = (float)ms;
    g_n++;
}

// ---- the machine (sampler thread)
struct Machine {
    float cpu_total = -1, cpu_game = -1;   // % of all logical processors
    float top_thread = -1;                 // % of one core
    float top_wait = -1;                   // % of wall time the busiest thread spent waiting for other guest threads
    wchar_t top_name[48] = L"";
    float ram_game_mb = -1, ram_used_mb = -1, ram_total_mb = -1;
    float vram_used_mb = -1, vram_budget_mb = -1;
    int threads = 0, cores = 1;
};
static SRWLOCK g_mlock = SRWLOCK_INIT;
static Machine g_mach;
static volatile LONG g_sampler;           // 1 while the sampler thread runs

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

typedef HRESULT(WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);

static DWORD WINAPI sampler(void *)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = (int)si.dwNumberOfProcessors;
    GetThreadDescription_t gtd = (GetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                                 "GetThreadDescription");
    uint64_t last_idle = 0, last_kern = 0, last_user = 0, last_proc = 0;
    LARGE_INTEGER last_t = {};
    std::unordered_map<DWORD, uint64_t> last_thread;
    std::unordered_map<DWORD, double> last_wait;
    DWORD pid = GetCurrentProcessId();
    while (hud_mode() == 2) {
        Machine m;
        m.cores = cores;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double wall = last_t.QuadPart ? (double)(now.QuadPart - last_t.QuadPart) / (double)g_qpf.QuadPart : 0;
        last_t = now;
        FILETIME idle, kern, user, cr, ex, pk, pu;
        if (GetSystemTimes(&idle, &kern, &user) && GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) {
            uint64_t i = ft64(idle), k = ft64(kern), u = ft64(user), p = ft64(pk) + ft64(pu);
            uint64_t total = (k - last_kern) + (u - last_user);   // kernel time includes idle time
            if (last_kern && total) {
                m.cpu_total = 100.0f * (float)(total - (i - last_idle)) / (float)total;
                m.cpu_game = 100.0f * (float)(p - last_proc) / (float)total;
            }
            last_idle = i, last_kern = k, last_user = u, last_proc = p;
        }
        // the busiest thread of this process: run time since the last sample over the wall time
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            std::unordered_map<DWORD, uint64_t> seen;
            DWORD best_tid = 0;
            uint64_t best = 0;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                FILETIME a, b, tk, tu;
                if (GetThreadTimes(h, &a, &b, &tk, &tu)) {
                    uint64_t t = ft64(tk) + ft64(tu);
                    seen[te.th32ThreadID] = t;
                    auto it = last_thread.find(te.th32ThreadID);
                    if (it != last_thread.end() && t - it->second > best) { best = t - it->second; best_tid = te.th32ThreadID; }
                }
                CloseHandle(h);
                m.threads++;
            }
            CloseHandle(snap);
            last_thread.swap(seen);
            if (wall > 0 && best_tid) {
                m.top_thread = (float)(best / 1e7 / wall * 100.0);
                if (m.top_thread > 100) m.top_thread = 100;
                double w = sw_host_wait_ms(best_tid);
                auto lw = last_wait.find(best_tid);
                if (lw != last_wait.end() && w >= lw->second) {
                    m.top_wait = (float)((w - lw->second) / (wall * 1000.0) * 100.0);
                    if (m.top_wait > 100) m.top_wait = 100;
                }
            }
            for (auto &kv : last_thread) last_wait[kv.first] = sw_host_wait_ms(kv.first);
            if (wall > 0 && best_tid) {
                HANDLE h = gtd ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, best_tid) : NULL;
                PWSTR name = NULL;
                if (h && SUCCEEDED(gtd(h, &name)) && name && name[0]) {
                    wcsncpy(m.top_name, name, 47);
                    m.top_name[47] = 0;
                } else {
                    swprintf(m.top_name, 48, L"thread %lu", (unsigned long)best_tid);
                }
                if (name) LocalFree(name);
                if (h) CloseHandle(h);
            }
        }
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
            m.ram_game_mb = (float)(pmc.WorkingSetSize >> 20);
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            m.ram_total_mb = (float)(ms.ullTotalPhys >> 20);
            m.ram_used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20);
        }
        uint64_t vu = 0, vb = 0;
        int exact = 0;
        if (vk_mem_vram(&vu, &vb, &exact)) {
            m.vram_used_mb = (float)(vu >> 20);
            m.vram_budget_mb = (float)(vb >> 20);
        }
        AcquireSRWLockExclusive(&g_mlock);
        g_mach = m;
        ReleaseSRWLockExclusive(&g_mlock);
        Sleep(500);
    }
    InterlockedExchange(&g_sampler, 0);
    return 0;
}

// ---- the numbers, refreshed twice a second on the present thread
struct Stats {
    float fps = 0, avg_ms = 0, low1 = 0, low01 = 0, worst_ms = 0;
    int stutters = 0;                    // frames over 2.5x the average in the window
    float gpu_ms = -1, gpu_busy = -1;    // per presented frame, % of wall time
    uint64_t shaders_total = 0;
    int shaders_recent = 0;              // built in the last 10 s
    Machine m;
};
static Stats g_st;

static void refresh(void)
{
    static LARGE_INTEGER last;
    static uint64_t last_busy, last_shaders;
    static uint32_t last_n;
    static int shader_hist[20], hist_i;   // pipelines per half second, last 10 s
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double span = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / (double)g_qpf.QuadPart : 0;
    if (last.QuadPart && span < 0.5) return;
    Stats s = g_st;
    // frames of the last 10 s (at most RING)
    static float sorted[RING];
    uint32_t have = g_n < RING ? g_n : RING;
    double sum = 0;
    uint32_t k = 0;
    while (k < have && sum < 10000.0) { sorted[k] = g_ms[(g_n - 1 - k) % RING]; sum += sorted[k]; k++; }
    if (k) {
        s.avg_ms = (float)(sum / k);
        s.fps = (float)(1000.0 * k / sum);
        s.stutters = 0;
        for (uint32_t i = 0; i < k; i++) if (sorted[i] > 2.5f * s.avg_ms && sorted[i] > 20.0f) s.stutters++;
        std::sort(sorted, sorted + k);
        float p99 = sorted[(uint32_t)(0.99 * (k - 1))], p999 = sorted[(uint32_t)(0.999 * (k - 1))];
        s.low1 = p99 > 0 ? 1000.0f / p99 : 0;
        s.low01 = p999 > 0 ? 1000.0f / p999 : 0;
        s.worst_ms = sorted[k - 1];
    }
    uint64_t busy = perf_gpu_busy_total();
    if (last.QuadPart && span > 0 && busy >= last_busy && busy != 0) {
        double d = (double)(busy - last_busy) / 1e6;   // ms
        uint32_t frames = g_n - last_n;
        s.gpu_busy = (float)(d / (span * 1000.0) * 100.0);
        if (s.gpu_busy > 100) s.gpu_busy = 100;
        s.gpu_ms = frames ? (float)(d / frames) : -1;
    }
    uint64_t sh = sw_perf_count_get(PERF_CNT_PIPE_BUILT_) + sw_perf_count_get(PERF_CNT_PIPE_ASYNC_);
    shader_hist[hist_i++ % 20] = last.QuadPart ? (int)(sh - last_shaders) : 0;
    s.shaders_total = sh;
    s.shaders_recent = 0;
    for (int i = 0; i < 20; i++) s.shaders_recent += shader_hist[i];
    last_busy = busy, last_n = g_n, last_shaders = sh, last = now;
    AcquireSRWLockShared(&g_mlock);
    s.m = g_mach;
    ReleaseSRWLockShared(&g_mlock);
    g_st = s;
}

// ---- drawing: a dark glass panel at the top left, Hisui gold accents
static ImU32 col(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
static const ImU32 kGood = IM_COL32(110, 220, 120, 255), kWarn = IM_COL32(240, 200, 70, 255),
                   kBad = IM_COL32(240, 90, 70, 255), kText = IM_COL32(246, 238, 218, 255),
                   kDim = IM_COL32(190, 178, 150, 255), kGold = IM_COL32(176, 134, 47, 255);

static ImU32 fps_col(float fps, float target)
{
    return fps >= target * 0.95f ? kGood : fps >= target * 0.75f ? kWarn : kBad;
}

// One line of label + value pairs: "label value   label value".  Returns the line height.
struct Line {
    ImDrawList *dl; ImFont *f; float size, x, y;
    void put(const char *t, ImU32 c) { dl->AddText(f, size, ImVec2(x, y), c, t); x += f->CalcTextSizeA(size, FLT_MAX, 0, t).x; }
    void gap(float g) { x += g; }
};

extern "C" void perfhud_layout(float W, float H, float s, ImFont *body, ImFont *small)
{
    (void)H;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (hud_mode() != 2) return;
    if (!InterlockedCompareExchange(&g_sampler, 1, 0)) {
        HANDLE t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(t);
        } else InterlockedExchange(&g_sampler, 0);
    }
    refresh();
    const Stats &st = g_st;
    int target = sw_fps_get() ? sw_fps_get() : 30;
    int fg = sw_fg_effective();
    float shown_target = (float)target * (fg > 1 ? fg : 1);

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float fb = body->FontSize * 0.82f, fsm = small->FontSize;
    float pad = 12 * s, x0 = 14 * s, y0 = 14 * s, pw = 430 * s;
    if (pw > W - 2 * x0) pw = W - 2 * x0;
    float lh = fsm + 6 * s, gh = 64 * s;
    float ph = pad + fb + 6 * s + lh + gh + 8 * s + 5 * lh + pad;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), col(14, 18, 30, 205), 10 * s);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + pw, y0 + ph), col(176, 134, 47, 170), 10 * s, 0, 1.5f * s);
    char b[160];
    float x = x0 + pad, y = y0 + pad;

    // big line: fps and frame time
    {
        Line L{dl, body, fb, x, y};
        snprintf(b, sizeof b, "%.0f FPS", st.fps);
        L.put(b, fps_col(st.fps, shown_target));
        L.gap(12 * s);
        snprintf(b, sizeof b, "%.2f ms", st.avg_ms);
        L.put(b, kText);
        L.gap(12 * s);
        int n = fg > 1 ? snprintf(b, sizeof b, "target %d x%d", target, fg) : snprintf(b, sizeof b, "target %d", target);
        if (sw_res_get() > 1 && n > 0 && n < (int)sizeof b) n += snprintf(b + n, sizeof b - n, "  %dx", sw_res_get());
        if (sw_dlaa_state() == 1 && n > 0 && n < (int)sizeof b) snprintf(b + n, sizeof b - n, "  DLAA");
        Line T{dl, small, fsm, 0, y + (fb - fsm) * 0.7f};
        T.x = x0 + pw - pad - small->CalcTextSizeA(fsm, FLT_MAX, 0, b).x;
        T.put(b, kDim);
        y += fb + 6 * s;
    }
    {
        Line L{dl, small, fsm, x, y};
        L.put("1% low ", kDim);
        snprintf(b, sizeof b, "%.0f", st.low1);
        L.put(b, fps_col(st.low1, shown_target));
        L.gap(14 * s);
        L.put("0.1% low ", kDim);
        snprintf(b, sizeof b, "%.0f", st.low01);
        L.put(b, fps_col(st.low01, shown_target));
        L.gap(14 * s);
        L.put("worst ", kDim);
        snprintf(b, sizeof b, "%.1f ms", st.worst_ms);
        L.put(b, st.worst_ms > 2.5f * 1000.0f / shown_target ? kBad : kText);
        L.gap(14 * s);
        L.put("hitches ", kDim);
        snprintf(b, sizeof b, "%d", st.stutters);
        L.put(b, st.stutters ? kWarn : kText);
        y += lh;
    }
    // graph: the last GRAPH frames, a gold line at the target frame time, red bars above twice of it
    {
        float gx0 = x, gx1 = x0 + pw - pad, gy0 = y + 2 * s, gy1 = gy0 + gh;
        dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(gx1, gy1), col(0, 0, 0, 90), 4 * s);
        float tgt = 1000.0f / shown_target;
        float top = tgt * 3.0f;
        if (top < 20) top = 20;
        int n = (int)(g_n < GRAPH ? g_n : GRAPH);
        float bw = (gx1 - gx0) / GRAPH;
        for (int i = 0; i < n; i++) {
            float ms = g_ms[(g_n - n + i) % RING];
            float h = ms / top * (gy1 - gy0);
            if (h > gy1 - gy0) h = gy1 - gy0;
            ImU32 c = ms > tgt * 2.0f ? kBad : ms > tgt * 1.25f ? kWarn : col(110, 200, 150, 220);
            float bx = gx1 - (n - i) * bw;
            dl->AddRectFilled(ImVec2(bx, gy1 - h), ImVec2(bx + (bw > 1.5f ? bw - 0.5f : bw), gy1), c);
        }
        float ty = gy1 - tgt / top * (gy1 - gy0);
        dl->AddLine(ImVec2(gx0, ty), ImVec2(gx1, ty), kGold, 1.2f * s);
        snprintf(b, sizeof b, "%.1f ms", tgt);
        dl->AddText(small, fsm * 0.8f, ImVec2(gx0 + 4 * s, ty - fsm * 0.8f - 1 * s), kGold, b);
        snprintf(b, sizeof b, "%.0f ms", top);
        dl->AddText(small, fsm * 0.8f, ImVec2(gx0 + 4 * s, gy0 + 1 * s), kDim, b);
        y = gy1 + 8 * s;
    }
    const Machine &m = st.m;
    {   // CPU
        Line L{dl, small, fsm, x, y};
        L.put("CPU ", kDim);
        if (m.cpu_total >= 0) snprintf(b, sizeof b, "%.0f%%", m.cpu_total); else snprintf(b, sizeof b, "--");
        L.put(b, kText);
        L.put("  game ", kDim);
        if (m.cpu_game >= 0) snprintf(b, sizeof b, "%.0f%% (%.1f cores)", m.cpu_game, m.cpu_game * m.cores / 100.0f);
        else snprintf(b, sizeof b, "--");
        L.put(b, kText);
        y += lh;
    }
    {   // busiest thread
        Line L{dl, small, fsm, x, y};
        L.put("Busiest thread ", kDim);
        if (m.top_thread >= 0) {
            char nm[48];
            WideCharToMultiByte(CP_UTF8, 0, m.top_name, -1, nm, sizeof nm, NULL, NULL);
            snprintf(b, sizeof b, "%.0f%%", m.top_thread);
            L.put(b, m.top_thread > 90 ? kBad : m.top_thread > 75 ? kWarn : kText);
            snprintf(b, sizeof b, "  %s", nm);
            L.put(b, kDim);
            if (m.top_wait > 1) {
                snprintf(b, sizeof b, "  (%.0f%% waiting)", m.top_wait);
                L.put(b, m.top_wait > 25 ? kWarn : kDim);
            }
        } else L.put("--", kText);
        y += lh;
    }
    {   // GPU
        Line L{dl, small, fsm, x, y};
        L.put("GPU ", kDim);
        if (st.gpu_ms >= 0) {
            snprintf(b, sizeof b, "%.2f ms/frame  busy %.0f%%", st.gpu_ms, st.gpu_busy);
            L.put(b, st.gpu_busy > 90 ? kBad : kText);
        } else L.put("measuring...", kDim);
        y += lh;
    }
    {   // memory
        Line L{dl, small, fsm, x, y};
        L.put("VRAM ", kDim);
        if (m.vram_used_mb >= 0) snprintf(b, sizeof b, "%.1f / %.1f GB", m.vram_used_mb / 1024, m.vram_budget_mb / 1024);
        else snprintf(b, sizeof b, "--");
        L.put(b, m.vram_budget_mb > 0 && m.vram_used_mb > 0.9f * m.vram_budget_mb ? kBad : kText);
        L.put("   RAM ", kDim);
        if (m.ram_game_mb >= 0) snprintf(b, sizeof b, "%.1f GB (%.0f / %.0f GB)", m.ram_game_mb / 1024, m.ram_used_mb / 1024,
                                         m.ram_total_mb / 1024);
        else snprintf(b, sizeof b, "--");
        L.put(b, kText);
        y += lh;
    }
    {   // shaders + the bottleneck guess
        Line L{dl, small, fsm, x, y};
        L.put("New shaders (10 s) ", kDim);
        snprintf(b, sizeof b, "+%d", st.shaders_recent);
        L.put(b, st.shaders_recent ? kWarn : kText);
        L.gap(16 * s);
        const char *why;
        ImU32 wc = kText;
        float frame_ms = st.avg_ms;
        if (st.fps >= shown_target * 0.97f) { why = "frame cap"; wc = kGood; }
        else if (st.gpu_ms >= 0 && frame_ms > 0 && st.gpu_ms > 0.85f * frame_ms) { why = "GPU"; wc = kWarn; }
        else if (m.top_thread > 85 && m.top_wait > 25) { why = "CPU (thread hand-offs)"; wc = kWarn; }
        else if (m.top_thread > 85) { why = "CPU (one thread)"; wc = kWarn; }
        else if (m.cpu_total > 90) { why = "CPU (all cores)"; wc = kWarn; }
        else if (st.gpu_ms < 0 || m.top_thread < 0) { why = "measuring..."; wc = kDim; }
        else { why = "CPU (thread hand-offs)"; wc = kWarn; }
        L.put("Limited by ", kDim);
        L.put(why, wc);
        y += lh;
    }
}
