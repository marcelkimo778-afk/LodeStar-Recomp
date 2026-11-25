// mb_browser.cpp - see mb_browser.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "mb_browser.h"
#include "mb_http.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace mb {

const char *job_state_name(int s)
{
    static const char *n[] = {"queued", "downloading", "checking", "installing", "installed", "failed", "cancelled",
                              "needs confirmation", "opened in the browser"};
    return s >= 0 && s <= JOB_BROWSER ? n[s] : "?";
}

// ================================================================== images (WIC)

bool decode_image(const std::string &bytes, int max_w, Image *out, std::string *err)
{
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *f = NULL;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *cv = NULL;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromMemory((BYTE *)bytes.data(), (DWORD)bytes.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, NULL, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&w, &h)) && w && h && w < 16384 && h < 16384) {
        IWICBitmapSource *srcb = fr;
        UINT ow = w, oh = h;
        if ((int)w > max_w) {
            ow = (UINT)max_w;
            oh = std::max<UINT>(1, (UINT)((uint64_t)h * (UINT)max_w / w));
            if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, ow, oh, WICBitmapInterpolationModeFant))) srcb = sc;
            else { ow = w; oh = h; }
        }
        if (SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(srcb, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
            out->rgba.assign((size_t)ow * oh * 4, 0);
            ok = SUCCEEDED(cv->CopyPixels(NULL, ow * 4, (UINT)out->rgba.size(), out->rgba.data()));
            out->w = (int)ow;
            out->h = (int)oh;
        }
    }
    if (!ok && err) *err = "cannot decode the image";
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

// ================================================================== workers

class Worker {
    std::thread t_;
    std::mutex m_;
    std::condition_variable cv_, idle_cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
    int busy_ = 0;

public:
    void start()
    {
        t_ = std::thread([this] {
            for (;;) {
                std::function<void()> f;
                {
                    std::unique_lock<std::mutex> l(m_);
                    cv_.wait(l, [this] { return stop_ || !q_.empty(); });
                    if (stop_ && q_.empty()) return;
                    f = std::move(q_.front());
                    q_.pop_front();
                    busy_++;
                }
                try { f(); } catch (...) { fprintf(stderr, "[modbrowser] a task failed with an exception\n"); }
                std::lock_guard<std::mutex> l(m_);
                busy_--;
                idle_cv_.notify_all();
            }
        });
    }
    void post(std::function<void()> f)
    {
        std::lock_guard<std::mutex> l(m_);
        q_.push_back(std::move(f));
        cv_.notify_one();
    }
    void wait_idle()
    {
        std::unique_lock<std::mutex> l(m_);
        idle_cv_.wait(l, [this] { return q_.empty() && busy_ == 0; });
    }
    void stop()
    {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
            q_.clear();
            cv_.notify_all();
        }
        if (t_.joinable()) t_.join();
    }
};

// ================================================================== state

struct JobData {
    JobView v;
    int tab = -1;
    ModDetails mod;
    ModFile file;
    bool allow = false;
    std::string local;                          // install_local: a file path or https / nxm link
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
};

struct Browser::Impl {
    std::mutex m;
    BrowserSettings s;
    std::vector<std::shared_ptr<ModSource>> src;
    std::vector<TabView> tabs;
    std::vector<uint64_t> gen;                  // per tab: newest search (older answers are dropped)
    DetailView det;
    uint64_t det_gen = 0;
    std::vector<JobData> jobs;
    int next_job = 1;
    std::vector<InstalledView> inst;
    std::map<std::string, std::unique_ptr<Image>> images;
    std::deque<std::string> log;
    Worker api, img, job;

    void note(const std::string &msg)
    {
        fprintf(stderr, "[modbrowser] %s\n", msg.c_str());
        std::lock_guard<std::mutex> l(m);
        log.push_back(msg);
        while (log.size() > 40) log.pop_front();
    }
};

Browser::Browser() : d_(new Impl) {}
Browser::~Browser() { stop(); }

static void build_sources(Browser::Impl *d, const BrowserPaths &p)
{
    std::vector<std::shared_ptr<ModSource>> src;
    if (!p.mock_dir.empty()) src.emplace_back(make_mock_source(p.mock_dir));
    src.emplace_back(make_gamebanana_source(d->s, p.cache_dir));
    src.emplace_back(make_nexus_source(d->s, p.cache_dir));
    if (d->s.modio && !d->s.modio_key.empty()) src.emplace_back(make_modio_source(d->s, p.cache_dir));   // optional: only when set up
    src.emplace_back(make_github_source(d->s, p.cache_dir));
    std::vector<TabView> tabs;
    for (auto &s : src) {
        TabView t;
        t.id = s->id();
        t.title = s->title();
        t.verified = s->verified();
        t.available = s->available(&t.why);
        tabs.push_back(t);
    }
    d->src = src;
    d->tabs = tabs;
    d->gen.assign(tabs.size(), 0);
}

bool Browser::start(const BrowserPaths &p)
{
    if (started_) return true;
    paths_ = p;
    make_dirs(p.cache_dir);
    settings_load(p.settings_file, &d_->s);
    if (!d_->s.target_version.empty()) paths_.target.version = d_->s.target_version;
    build_sources(d_.get(), paths_);
    cleanup_tmp(p.mods_root);                   // leftovers of an interrupted install / uninstall
    d_->api.start();
    d_->img.start();
    d_->job.start();
    started_ = true;
    d_->note("target: build " + paths_.target.build_id.substr(0, 16) + ", version " + paths_.target.version +
             " (the version is a setting: the game dump carries no version file)");
    refresh_installed();
    if (d_->s.check_updates) check_updates();
    return true;
}

void Browser::stop()
{
    if (!started_) return;
    {
        std::lock_guard<std::mutex> l(d_->m);
        for (auto &j : d_->jobs) j.cancel->store(true);
    }
    d_->job.stop();
    d_->api.stop();
    d_->img.stop();
    started_ = false;
}

void Browser::wait_idle()
{
    for (int k = 0; k < 3; k++) { d_->api.wait_idle(); d_->img.wait_idle(); d_->job.wait_idle(); }
}

int Browser::tab_count()
{
    std::lock_guard<std::mutex> l(d_->m);
    return (int)d_->tabs.size();
}

TabView Browser::tab(int i)
{
    std::lock_guard<std::mutex> l(d_->m);
    return i >= 0 && i < (int)d_->tabs.size() ? d_->tabs[(size_t)i] : TabView();
}

void Browser::search(int ti, const SearchQuery &q, bool append)
{
    std::shared_ptr<ModSource> s;
    uint64_t g;
    {
        std::lock_guard<std::mutex> l(d_->m);
        if (ti < 0 || ti >= (int)d_->tabs.size()) return;
        TabView &t = d_->tabs[(size_t)ti];
        t.available = d_->src[(size_t)ti]->available(&t.why);
        if (!t.available) { t.state = ST_ERROR; t.error = t.why; return; }
        t.query = q;
        t.state = ST_LOADING;
        t.error.clear();
        if (!append) { t.mods.clear(); t.compat.clear(); }
        s = d_->src[(size_t)ti];
        g = ++d_->gen[(size_t)ti];
    }
    Impl *d = d_.get();
    Target target = paths_.target;
    d->api.post([d, s, q, ti, g, append, target] {
        SearchResult r;
        std::string err;
        bool ok = s->search(q, &r, &err);
        std::vector<VersionInfo> cv;
        for (const auto &m : r.mods) {
            ModDetails md;
            static_cast<ModSummary &>(md) = m;
            cv.push_back(detect_version_meta(md, nullptr, target));
        }
        std::lock_guard<std::mutex> l(d->m);
        if (ti >= (int)d->tabs.size() || d->gen[(size_t)ti] != g) return;   // a newer search replaced this one
        TabView &t = d->tabs[(size_t)ti];
        if (!ok) { t.state = ST_ERROR; t.error = err; return; }
        if (!append) { t.mods.clear(); t.compat.clear(); }
        t.mods.insert(t.mods.end(), r.mods.begin(), r.mods.end());
        t.compat.insert(t.compat.end(), cv.begin(), cv.end());
        t.total = r.total;
        t.more = r.more;
        t.state = ST_OK;
    });
}

void Browser::open_details(int ti, const std::string &mod_id)
{
    std::shared_ptr<ModSource> s;
    uint64_t g;
    {
        std::lock_guard<std::mutex> l(d_->m);
        if (ti < 0 || ti >= (int)d_->src.size()) return;
        s = d_->src[(size_t)ti];
        d_->det = DetailView();
        d_->det.state = ST_LOADING;
        d_->det.tab = ti;
        g = ++d_->det_gen;
    }
    Impl *d = d_.get();
    Target target = paths_.target;
    d->api.post([d, s, ti, mod_id, g, target] {
        DetailView v;
        v.tab = ti;
        std::string err;
        if (s->details(mod_id, &v.mod, &err)) {
            v.state = ST_OK;
            v.compat = detect_version_meta(v.mod, v.mod.files.empty() ? nullptr : &v.mod.files[0], target);
        } else { v.state = ST_ERROR; v.error = err; }
        std::lock_guard<std::mutex> l(d->m);
        if (d->det_gen == g) d->det = v;
    });
}

DetailView Browser::details()
{
    std::lock_guard<std::mutex> l(d_->m);
    return d_->det;
}

// ---- jobs

static void set_job(Browser::Impl *d, int id, const std::function<void(JobView &)> &f)
{
    std::lock_guard<std::mutex> l(d->m);
    for (auto &j : d->jobs) if (j.v.id == id) { f(j.v); return; }
}

static void run_job(Browser *b, Browser::Impl *d, int id)
{
    JobData j;
    std::shared_ptr<ModSource> s;
    {
        std::lock_guard<std::mutex> l(d->m);
        for (auto &x : d->jobs) if (x.v.id == id) j = x;
        if (!j.v.id || j.cancel->load()) return;
        if (j.tab >= 0 && j.tab < (int)d->src.size()) s = d->src[(size_t)j.tab];
    }
    const BrowserPaths &p = b->paths();
    BrowserSettings st = b->settings();
    std::string ext = file_ext(j.file.name);
    if (ext.empty()) ext = ".zip";
    std::string dl_dir = join(p.cache_dir, "downloads");
    make_dirs(dl_dir);
    std::string dest = join(dl_dir, temp_name("dl-") + ext);
    auto cancel = j.cancel;
    Progress prog = [d, id, cancel](uint64_t done, uint64_t total) {
        set_job(d, id, [&](JobView &v) { v.done = done; v.total = total; });
        return !cancel->load();
    };
    set_job(d, id, [](JobView &v) { v.state = JOB_DOWNLOAD; v.message = "downloading"; });
    std::string err, burl;
    bool ok;
    bool is_local = !j.local.empty() && is_file(j.local);
    if (is_local) {
        dest = j.local;                                  // install straight from the file (never deleted)
        ok = true;
    } else if (!j.local.empty() && !s) {                 // a https link
        HttpRequest rq;
        rq.url = j.local;
        rq.max_bytes = st.max_download;
        ok = http_download(rq, dest, prog, &err);
    } else if (s) {
        ok = s->download(j.file, dest, prog, &err, &burl);
    } else { ok = false; err = "no source for this download"; }
    auto finish = [&](int state, const std::string &msg) {
        set_job(d, id, [&](JobView &v) { v.state = state; v.message = msg; });
        if (!is_local) DeleteFileW(wide(dest).c_str());
        d->note(j.v.title + ": " + msg);
    };
    if (!ok) {
        if (!burl.empty()) {
            set_job(d, id, [&](JobView &v) { v.browser_url = burl; });
            return finish(JOB_BROWSER, err);
        }
        return finish(cancel->load() ? JOB_CANCELLED : JOB_FAILED, cancel->load() ? "cancelled" : err);
    }
    set_job(d, id, [](JobView &v) { v.state = JOB_VERIFY; v.message = "checking"; });
    if (!verify_download(dest, j.file, &err)) return finish(JOB_FAILED, err);
    if (cancel->load()) return finish(JOB_CANCELLED, "cancelled");
    set_job(d, id, [](JobView &v) { v.state = JOB_INSTALL; v.message = "installing"; });
    InstallOptions o;
    o.mods_root = p.mods_root;
    o.target = p.target;
    o.allow_wrong_version = j.allow;
    o.max_unpacked = st.max_unpacked;
    o.max_files = st.max_files;
    InstallResult r;
    install_archive(dest, j.mod, j.file, o, &r);
    set_job(d, id, [&](JobView &v) { v.refused = r.refused; v.warnings = r.warnings; v.folders = r.folders; v.compat = r.version; });
    if (r.blocked_version) return finish(JOB_BLOCKED, r.error);
    if (!r.ok) return finish(JOB_FAILED, r.error);
    std::string msg = "installed - it loads at the next start";
    if (!r.refused.empty()) msg += " (" + std::to_string(r.refused.size()) + " program / script file(s) refused)";
    finish(JOB_DONE, msg);
    b->refresh_installed();
}

int Browser::install(int ti, const ModDetails &mod, const ModFile &file, bool allow)
{
    JobData j;
    {
        std::lock_guard<std::mutex> l(d_->m);
        j.v.id = d_->next_job++;
        j.v.title = mod.name + (file.name.empty() ? "" : " (" + file.name + ")");
        j.v.total = file.size;
        j.tab = ti;
        j.mod = mod;
        j.file = file;
        j.allow = allow;
        d_->jobs.push_back(j);
    }
    Browser *b = this;
    Impl *d = d_.get();
    int id = j.v.id;
    d_->job.post([b, d, id] { run_job(b, d, id); });
    return id;
}

int Browser::install_local(const std::string &in, bool allow)
{
    std::string what = trim(in);
    if (what.size() >= 2 && what.front() == '"' && what.back() == '"') what = what.substr(1, what.size() - 2);
    ModDetails mod;
    ModFile file;
    int ti = -1;
    file.name = base_name(what);
    std::string lw = lower(what);
    if (lw.compare(0, 6, "nxm://") == 0) {
        // nxm://<game>/mods/<mod>/files/<file>?key=..&expires=..&user_id=..  (Nexus Mods "Mod Manager Download")
        std::string rest = what.substr(6);
        size_t q = rest.find('?');
        std::string qs = q == std::string::npos ? "" : rest.substr(q);
        std::vector<std::string> parts;
        std::string cur;
        for (char c : rest.substr(0, q)) { if (c == '/') { parts.push_back(cur); cur.clear(); } else cur += c; }
        parts.push_back(cur);
        if (parts.size() < 5 || parts[1] != "mods" || parts[3] != "files") return 0;
        std::lock_guard<std::mutex> l(d_->m);
        for (size_t i = 0; i < d_->src.size(); i++) if (std::string(d_->src[i]->id()) == "nexus") ti = (int)i;
        if (ti < 0) return 0;
        mod.source = "nexus";
        mod.id = parts[2];
        mod.name = "Nexus mod " + parts[2];
        mod.page_url = "https://www.nexusmods.com/" + parts[0] + "/mods/" + parts[2];
        file.id = parts[4];
        file.name = "nexus_" + parts[2] + "_" + parts[4] + ".zip";
        file.url = "nexus:" + parts[2] + ":" + parts[4];
        file.md5 = "nxm:" + qs;                           // the key + expiry of the link (see NexusSource)
    } else if (lw.compare(0, 8, "https://") == 0) {
        mod.source = "url";
        mod.id = what;
        mod.page_url = what;
        size_t q = file.name.find('?');
        if (q != std::string::npos) file.name = file.name.substr(0, q);
        mod.name = file.name.empty() ? "Downloaded mod" : file.name.substr(0, file.name.size() - file_ext(file.name).size());
    } else if (lw.compare(0, 7, "http://") == 0) {
        return 0;                                         // https only
    } else {
        if (!is_file(what)) return 0;
        mod.source = "file";
        mod.id = base_name(what);
        mod.name = file.name.substr(0, file.name.size() - file_ext(file.name).size());
    }
    JobData j;
    {
        std::lock_guard<std::mutex> l(d_->m);
        j.v.id = d_->next_job++;
        j.v.title = mod.name;
        j.tab = ti;
        j.mod = mod;
        j.file = file;
        j.allow = allow;
        j.local = mod.source == "nexus" ? "" : what;
        d_->jobs.push_back(j);
    }
    Browser *b = this;
    Impl *d = d_.get();
    int id = j.v.id;
    d_->job.post([b, d, id] { run_job(b, d, id); });
    return id;
}

std::vector<JobView> Browser::jobs()
{
    std::lock_guard<std::mutex> l(d_->m);
    std::vector<JobView> v;
    for (const auto &j : d_->jobs) v.push_back(j.v);
    return v;
}

void Browser::cancel(int id)
{
    std::lock_guard<std::mutex> l(d_->m);
    for (auto &j : d_->jobs)
        if (j.v.id == id) {
            j.cancel->store(true);
            if (j.v.state == JOB_QUEUED) { j.v.state = JOB_CANCELLED; j.v.message = "cancelled"; }
        }
}

int Browser::install_anyway(int id)
{
    int tab = -1;
    ModDetails mod;
    ModFile file;
    std::string local;
    {
        std::lock_guard<std::mutex> l(d_->m);
        bool found = false;
        for (auto &j : d_->jobs)
            if (j.v.id == id && j.v.state == JOB_BLOCKED) { tab = j.tab; mod = j.mod; file = j.file; local = j.local; found = true; }
        if (!found) return 0;
    }
    if (!local.empty()) return install_local(local, true);
    return install(tab, mod, file, true);
}

void Browser::clear_finished()
{
    std::lock_guard<std::mutex> l(d_->m);
    d_->jobs.erase(std::remove_if(d_->jobs.begin(), d_->jobs.end(), [](const JobData &j) { return j.v.finished(); }),
                   d_->jobs.end());
}

// ---- installed

void Browser::refresh_installed()
{
    std::vector<InstalledMeta> ms = list_installed(paths_.mods_root);
    std::lock_guard<std::mutex> l(d_->m);
    std::vector<InstalledView> v;
    for (const auto &m : ms) {
        InstalledView iv;
        iv.meta = m;
        iv.disabled = is_file(join(m.folder, ".disabled"));
        for (const auto &old : d_->inst) if (old.meta.folder == m.folder && version_newer(old.latest, m.version)) iv.latest = old.latest;
        v.push_back(iv);
    }
    d_->inst = v;
}

std::vector<InstalledView> Browser::installed()
{
    std::lock_guard<std::mutex> l(d_->m);
    return d_->inst;
}

int Browser::installed_state(const std::string &source, const std::string &mod_id, std::string *version)
{
    std::lock_guard<std::mutex> l(d_->m);
    for (const auto &i : d_->inst)
        if (i.meta.source == source && i.meta.mod_id == mod_id) {
            if (version) *version = i.meta.version;
            return 1;
        }
    return 0;
}

bool Browser::uninstall(const std::string &folder, std::string *msg)
{
    bool deferred = false;
    std::string err;
    bool ok = uninstall_mod(folder, paths_.mods_root, &err, &deferred);
    *msg = ok ? (deferred ? "removed (some files were still open: they go at the next start)" : "removed") : err;
    d_->note(base_name(folder) + ": " + *msg);
    refresh_installed();
    return ok;
}

bool Browser::set_enabled(const std::string &folder, bool on)
{
    std::string marker = join(folder, ".disabled");
    bool ok = on ? (!is_file(marker) || DeleteFileW(wide(marker).c_str()) != 0) : write_text_atomic(marker, "");
    refresh_installed();
    return ok;
}

void Browser::check_updates()
{
    std::vector<std::pair<InstalledMeta, std::shared_ptr<ModSource>>> todo;
    {
        std::lock_guard<std::mutex> l(d_->m);
        for (const auto &i : d_->inst)
            for (size_t k = 0; k < d_->src.size(); k++)
                if (i.meta.source == d_->src[k]->id() && d_->tabs[k].available) todo.push_back({i.meta, d_->src[k]});
    }
    Impl *d = d_.get();
    for (auto &t : todo) {
        InstalledMeta m = t.first;
        std::shared_ptr<ModSource> s = t.second;
        d->api.post([d, m, s] {
            std::string latest, err;
            if (!s->check_update(m, &latest, &err)) return;
            if (!version_newer(latest, m.version)) return;
            std::lock_guard<std::mutex> l(d->m);
            for (auto &i : d->inst) if (i.meta.folder == m.folder) i.latest = latest;
        });
    }
}

// ---- images

Image *Browser::image(int ti, const std::string &url)
{
    if (url.empty()) return nullptr;
    std::shared_ptr<ModSource> s;
    Image *img;
    {
        std::lock_guard<std::mutex> l(d_->m);
        auto &slot = d_->images[url];
        if (slot && slot->state != ST_IDLE) return slot.get();   // idle: the UI dropped its texture, load it again
        if (!slot) slot.reset(new Image());
        slot->state = ST_LOADING;
        img = slot.get();
        if (ti < 0 || ti >= (int)d_->src.size()) { img->state = ST_ERROR; return img; }
        s = d_->src[(size_t)ti];
    }
    Impl *d = d_.get();
    d->img.post([d, s, url, img] {
        std::string bytes, err;
        Image tmp;
        bool ok = s->image(url, &bytes, &err) && decode_image(bytes, 480, &tmp, &err);
        std::lock_guard<std::mutex> l(d->m);
        if (ok) { img->w = tmp.w; img->h = tmp.h; img->rgba.swap(tmp.rgba); img->state = ST_OK; }
        else img->state = ST_ERROR;
    });
    return img;
}

// ---- settings

BrowserSettings Browser::settings()
{
    std::lock_guard<std::mutex> l(d_->m);
    return d_->s;
}

void Browser::set_settings(const BrowserSettings &s)
{
    {
        std::lock_guard<std::mutex> l(d_->m);
        d_->s = s;
        build_sources(d_.get(), paths_);             // tasks keep their own shared_ptr to the old sources
    }
    if (!settings_save(paths_.settings_file, s)) d_->note("cannot save " + paths_.settings_file);
}

std::string Browser::log_tail()
{
    std::lock_guard<std::mutex> l(d_->m);
    return d_->log.empty() ? std::string() : d_->log.back();
}

}  // namespace mb
