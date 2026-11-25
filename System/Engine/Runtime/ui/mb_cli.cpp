// mb_cli.cpp - mb_cli.exe: the mod browser for the launcher (launcher/modbrowser_ui.py).  The same sources, install
// pipeline and version checks as the rest of ui/mb_*.cpp; one JSON object per line on stdout.
//
//   mb_cli <command> [--option value ...]
//   commands: sources | search | details | image | install | install-local | installed | uninstall | enable |
//             check-updates | settings | set-settings
//   options : --mods-root DIR  --settings FILE  --cache DIR  --mock DIR  --exefs DIR (build id from exefs/main)
//             --build-id HEX  --source ID  --q TEXT  --sort N  --page N  --per-page N  --category TEXT  --id MOD
//             --file-id FILE  --url URL  --out FILE  --path FILE_OR_LINK  --folder DIR  --on 0|1  --allow 1
//             --json TEXT (set-settings)
// install / install-local print {"type":"progress",...} lines, then {"type":"result",...}.  Cancel = end the
// process: leftovers in <mods>/.mb_tmp and <cache>/downloads are removed by the next install.
// Exit code 0 = the command did its job, 1 = it failed ({"type":"error","error":...}).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <map>

#include "mb_browser.h"
#include "mb_http.h"
#include "mb_install.h"
#include "mb_source.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace mb;

static std::map<std::string, std::string> g_opt;
static std::string opt(const char *k, const char *def = "")
{
    auto it = g_opt.find(k);
    return it == g_opt.end() ? def : it->second;
}

static void out(const std::string &line)
{
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static int fail(const std::string &e)
{
    out("{\"type\":\"error\",\"error\":" + json_quote(e) + "}");
    return 1;
}

static std::string exe_dir()
{
    wchar_t b[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, b, MAX_PATH);
    std::string p = utf8(std::wstring(b, n));
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "." : p.substr(0, s);
}

// the build id of the game's main program: the NSO header (offset 0x40, 32 bytes) of exefs/main
static std::string build_id_from_exefs(const std::string &exefs)
{
    std::string data;
    std::string mainp = join(exefs, "main");
    HANDLE h = CreateFileW(wide(mainp).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return "";
    unsigned char hd[0x60];
    DWORD got = 0;
    BOOL ok = ReadFile(h, hd, sizeof hd, &got, NULL);
    CloseHandle(h);
    if (!ok || got != sizeof hd || memcmp(hd, "NSO0", 4) != 0) return "";
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", hd[0x40 + i]);
    return hex;
}

struct Ctx {
    BrowserPaths p;
    BrowserSettings s;
    std::vector<std::shared_ptr<ModSource>> src;
};

static void make_sources(Ctx &c)
{
    if (!c.p.mock_dir.empty()) c.src.emplace_back(make_mock_source(c.p.mock_dir));
    c.src.emplace_back(make_gamebanana_source(c.s, c.p.cache_dir));
    c.src.emplace_back(make_nexus_source(c.s, c.p.cache_dir));
    if (c.s.modio && !c.s.modio_key.empty()) c.src.emplace_back(make_modio_source(c.s, c.p.cache_dir));   // optional
    c.src.emplace_back(make_github_source(c.s, c.p.cache_dir));
}

static std::shared_ptr<ModSource> source(Ctx &c, const std::string &id)
{
    for (auto &s : c.src) if (id == s->id()) return s;
    return nullptr;
}

// ---- JSON writers
static std::string jcompat(const VersionInfo &v)
{
    return "{\"compat\":" + json_quote(compat_name(v.compat)) + ",\"detected\":" + json_quote(v.detected) +
           ",\"why\":" + json_quote(v.why) + ",\"code_specific\":" + (v.code_specific ? "true" : "false") + "}";
}

static std::string jsummary(const ModSummary &m)
{
    return "\"source\":" + json_quote(m.source) + ",\"id\":" + json_quote(m.id) + ",\"name\":" + json_quote(m.name) +
           ",\"author\":" + json_quote(m.author) + ",\"version\":" + json_quote(m.version) + ",\"thumb_url\":" +
           json_quote(m.thumb_url) + ",\"page_url\":" + json_quote(m.page_url) + ",\"category\":" + json_quote(m.category) +
           ",\"summary\":" + json_quote(m.summary) + ",\"game_version\":" + json_quote(m.game_version) +
           ",\"downloads\":" + std::to_string(m.downloads) + ",\"likes\":" + std::to_string(m.likes) +
           ",\"updated\":" + std::to_string(m.updated);
}

static std::string jfile(const ModFile &f)
{
    return "{\"id\":" + json_quote(f.id) + ",\"name\":" + json_quote(f.name) + ",\"size\":" + std::to_string(f.size) +
           ",\"version\":" + json_quote(f.version) + ",\"description\":" + json_quote(f.description) +
           ",\"game_version\":" + json_quote(f.game_version) + ",\"has_hash\":" +
           (f.md5.size() == 32 || !f.sha256.empty() ? "true" : "false") + "}";
}

static std::string jlist(const std::vector<std::string> &v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + json_quote(v[i]);
    return o + "]";
}

static std::string jinstalled(const InstalledMeta &m)
{
    return "{\"folder\":" + json_quote(m.folder) + ",\"source\":" + json_quote(m.source) + ",\"mod_id\":" + json_quote(m.mod_id) +
           ",\"name\":" + json_quote(m.name) + ",\"author\":" + json_quote(m.author) + ",\"version\":" + json_quote(m.version) +
           ",\"file_name\":" + json_quote(m.file_name) + ",\"url\":" + json_quote(m.url) + ",\"installed_at\":" +
           json_quote(m.installed_at) + ",\"detected_version\":" + json_quote(m.detected_version) + ",\"compat\":" +
           json_quote(m.compat) + ",\"compat_why\":" + json_quote(m.compat_why) + ",\"disabled\":" +
           (is_file(join(m.folder, ".disabled")) ? "true" : "false") + "}";
}

// ---- PNG for Tk (PhotoImage reads PNG without extra libraries)
static bool write_png(const Image &im, const std::string &path)
{
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *f = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    bool ok = false;
    std::wstring wp = wide(path + ".tmp");
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) && SUCCEEDED(fr->Initialize(NULL)) &&
        SUCCEEDED(fr->SetSize((UINT)im.w, (UINT)im.h))) {
        // the PNG encoder may only take BGRA: it says so by changing fmt
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
        std::vector<uint8_t> px(im.rgba);
        if (SUCCEEDED(fr->SetPixelFormat(&fmt))) {
            bool usable = IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA) != 0;
            if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
                for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
                usable = true;
            }
            if (usable && SUCCEEDED(fr->WritePixels((UINT)im.h, (UINT)im.w * 4, (UINT)px.size(), px.data())) &&
                SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
                ok = true;
        }
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    if (ok) ok = MoveFileExW(wp.c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(wp.c_str());
    return ok;
}

static bool find_mod_file(Ctx &c, const std::string &src_id, const std::string &mod_id, const std::string &file_id, ModDetails *d,
                          ModFile *f, std::string *err)
{
    auto s = source(c, src_id);
    if (!s) { *err = "unknown source " + src_id; return false; }
    if (!s->details(mod_id, d, err)) return false;
    for (const auto &x : d->files)
        if (file_id.empty() || x.id == file_id) { *f = x; return true; }
    *err = "the mod has no such file";
    return false;
}

static void clean_downloads(const std::string &dir)
{
    std::vector<Entry> es;
    walk(dir, &es, 1000);
    for (const auto &e : es) if (!e.dir) DeleteFileW(wide(join(dir, e.rel)).c_str());
}

// download (with progress lines) -> verify -> install; prints the result line
static int do_install(Ctx &c, std::shared_ptr<ModSource> s, const ModDetails &mod, const ModFile &file, const std::string &local, bool allow)
{
    std::string dl_dir = join(c.p.cache_dir, "downloads");
    make_dirs(dl_dir);
    clean_downloads(dl_dir);                               // leftovers of a cancelled install
    std::string ext = file_ext(file.name);
    if (ext.empty()) ext = ".zip";
    std::string dest = local.empty() || !is_file(local) ? join(dl_dir, temp_name("dl-") + ext) : local;
    bool delete_after = dest != local;
    std::string err, burl;
    ULONGLONG last = 0;
    Progress prog = [&](uint64_t done, uint64_t total) {
        ULONGLONG now = GetTickCount64();
        if (now - last >= 150 || (total && done >= total)) {
            last = now;
            out("{\"type\":\"progress\",\"stage\":\"download\",\"done\":" + std::to_string(done) + ",\"total\":" + std::to_string(total) + "}");
        }
        return true;
    };
    out("{\"type\":\"progress\",\"stage\":\"download\",\"done\":0,\"total\":" + std::to_string(file.size) + "}");
    bool ok = true;
    if (!delete_after) ok = true;                          // a local file: no download
    else if (s) ok = s->download(file, dest, prog, &err, &burl);
    else if (lower(local).compare(0, 8, "https://") == 0) {
        HttpRequest rq;
        rq.url = local;
        rq.max_bytes = c.s.max_download;
        ok = http_download(rq, dest, prog, &err);
    } else { ok = false; err = "not a readable file, an https:// link or an nxm:// link"; }
    auto result = [&](const char *state, const std::string &msg, const InstallResult *r) {
        if (delete_after) DeleteFileW(wide(dest).c_str());
        std::string o = "{\"type\":\"result\",\"state\":" + json_quote(state) + ",\"message\":" + json_quote(msg);
        if (!burl.empty()) o += ",\"browser_url\":" + json_quote(burl);
        if (r) o += ",\"refused\":" + jlist(r->refused) + ",\"warnings\":" + jlist(r->warnings) + ",\"folders\":" +
                    jlist(r->folders) + ",\"version\":" + jcompat(r->version);
        out(o + "}");
        return strcmp(state, "done") == 0 ? 0 : 1;
    };
    if (!ok) return result(burl.empty() ? "failed" : "browser", err, nullptr);
    out("{\"type\":\"progress\",\"stage\":\"verify\"}");
    if (!verify_download(dest, file, &err)) return result("failed", err, nullptr);
    out("{\"type\":\"progress\",\"stage\":\"install\"}");
    InstallOptions o;
    o.mods_root = c.p.mods_root;
    o.target = c.p.target;
    o.allow_wrong_version = allow;
    o.max_unpacked = c.s.max_unpacked;
    o.max_files = c.s.max_files;
    InstallResult r;
    install_archive(dest, mod, file, o, &r);
    if (r.blocked_version) return result("blocked", r.error, &r);
    if (!r.ok) return result("failed", r.error, &r);
    std::string msg = "installed - it loads at the next start";
    if (!r.refused.empty()) msg += " (" + std::to_string(r.refused.size()) + " program / script file(s) refused)";
    return result("done", msg, &r);
}

int wmain(int argc, wchar_t **wargv)
{
    _setmode(_fileno(stdout), _O_BINARY);                  // UTF-8 bytes as they are
    if (argc < 2) { fprintf(stderr, "usage: mb_cli <command> [--option value ...] (see mb_cli.cpp)\n"); return 2; }
    std::string cmd = utf8(wargv[1]);
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = utf8(wargv[i]);
        if (k.compare(0, 2, "--") == 0) g_opt[k.substr(2)] = utf8(wargv[i + 1]);
    }
    std::string ed = exe_dir();
    Ctx c;
    c.p.mods_root = opt("mods-root", join(ed, "../../../Mods").c_str());              /* exe in System/build_work/out */
    c.p.settings_file = opt("settings", join(ed, "../../../Settings/modbrowser.json").c_str());
    c.p.cache_dir = opt("cache", join(ed, "mod_cache").c_str());
    c.p.mock_dir = opt("mock");
    c.p.target.build_id = opt("build-id");
    if (c.p.target.build_id.empty() && !opt("exefs").empty()) c.p.target.build_id = build_id_from_exefs(opt("exefs"));
    settings_load(c.p.settings_file, &c.s);
    c.p.target.version = c.s.target_version.empty() ? "1.0.0" : c.s.target_version;
    make_sources(c);

    if (cmd == "sources") {
        std::string o = "{\"type\":\"sources\",\"target\":{\"build_id\":" + json_quote(c.p.target.build_id) + ",\"version\":" +
                        json_quote(c.p.target.version) + "},\"mods_root\":" + json_quote(c.p.mods_root) + ",\"sources\":[";
        for (size_t i = 0; i < c.src.size(); i++) {
            std::string why;
            bool av = c.src[i]->available(&why);
            o += (i ? "," : "") + std::string("{\"id\":") + json_quote(c.src[i]->id()) + ",\"title\":" + json_quote(c.src[i]->title()) +
                 ",\"available\":" + (av ? "true" : "false") + ",\"why\":" + json_quote(why) + ",\"verified\":" +
                 (c.src[i]->verified() ? "true" : "false") + "}";
        }
        out(o + "]}");
        return 0;
    }
    if (cmd == "settings") {
        std::string t;
        out(read_text(c.p.settings_file, &t) && json_parse(t) ? t.substr(0, t.find_last_not_of(" \r\n") + 1) : std::string("{}"));
        return 0;
    }
    if (cmd == "set-settings") {                           // the launcher's settings dialog
        std::string tmp = c.p.settings_file + ".in";
        if (!write_text_atomic(tmp, opt("json", "{}"))) return fail("cannot write the settings");
        BrowserSettings s = c.s;
        bool ok = settings_load(tmp, &s);
        DeleteFileW(wide(tmp).c_str());
        if (!ok) return fail("the settings are not valid JSON");
        make_dirs(c.p.settings_file.substr(0, c.p.settings_file.find_last_of("\\/")));
        if (!settings_save(c.p.settings_file, s)) return fail("cannot save " + c.p.settings_file);
        out("{\"type\":\"ok\"}");
        return 0;
    }
    if (cmd == "search") {
        auto s = source(c, opt("source"));
        if (!s) return fail("unknown source " + opt("source"));
        std::string err;
        if (!s->available(&err)) return fail(err);
        SearchQuery q;
        q.text = opt("q");
        q.category = opt("category");
        q.sort = atoi(opt("sort", "0").c_str());
        q.page = std::max(1, atoi(opt("page", "1").c_str()));
        q.per_page = std::max(1, std::min(100, atoi(opt("per-page", "24").c_str())));
        SearchResult r;
        if (!s->search(q, &r, &err)) return fail(err);
        std::string o = "{\"type\":\"search\",\"total\":" + std::to_string(r.total) + ",\"more\":" + (r.more ? "true" : "false") + ",\"mods\":[";
        for (size_t i = 0; i < r.mods.size(); i++) {
            ModDetails md;
            static_cast<ModSummary &>(md) = r.mods[i];
            o += (i ? "," : "") + std::string("{") + jsummary(r.mods[i]) + ",\"compat\":" +
                 jcompat(detect_version_meta(md, nullptr, c.p.target)) + "}";
        }
        out(o + "]}");
        return 0;
    }
    if (cmd == "details") {
        auto s = source(c, opt("source"));
        if (!s) return fail("unknown source " + opt("source"));
        ModDetails d;
        std::string err;
        if (!s->details(opt("id"), &d, &err)) return fail(err);
        std::string o = "{\"type\":\"details\"," + jsummary(d) + ",\"description\":" + json_quote(d.description) +
                        ",\"changelog\":" + json_quote(d.changelog) + ",\"images\":" + jlist(d.images) + ",\"compat\":" +
                        jcompat(detect_version_meta(d, d.files.empty() ? nullptr : &d.files[0], c.p.target)) + ",\"files\":[";
        for (size_t i = 0; i < d.files.size(); i++) o += (i ? "," : "") + jfile(d.files[i]);
        out(o + "]}");
        return 0;
    }
    if (cmd == "image") {                                  // thumbnail -> PNG file (cached by URL)
        auto s = source(c, opt("source"));
        std::string url = opt("url"), dst = opt("out");
        if (!s || url.empty() || dst.empty()) return fail("image needs --source, --url and --out");
        std::string bytes, err;
        Image im;
        if (!s->image(url, &bytes, &err)) return fail(err);
        if (!decode_image(bytes, atoi(opt("max-width", "320").c_str()), &im, &err)) return fail(err);
        if (!write_png(im, dst)) return fail("cannot write " + dst);
        out("{\"type\":\"image\",\"path\":" + json_quote(dst) + ",\"w\":" + std::to_string(im.w) + ",\"h\":" + std::to_string(im.h) + "}");
        return 0;
    }
    if (cmd == "install") {
        ModDetails d;
        ModFile f;
        std::string err;
        if (!find_mod_file(c, opt("source"), opt("id"), opt("file-id"), &d, &f, &err)) return fail(err);
        return do_install(c, source(c, opt("source")), d, f, "", opt("allow") == "1");
    }
    if (cmd == "install-local") {
        std::string what = trim(opt("path"));
        if (what.size() >= 2 && what.front() == '"' && what.back() == '"') what = what.substr(1, what.size() - 2);
        ModDetails mod;
        ModFile file;
        file.name = base_name(what);
        std::string lw = lower(what);
        std::shared_ptr<ModSource> s;
        if (lw.compare(0, 6, "nxm://") == 0) {
            std::string rest = what.substr(6);
            size_t q = rest.find('?');
            std::string qs = q == std::string::npos ? "" : rest.substr(q);
            std::vector<std::string> parts;
            std::string cur;
            for (char ch : rest.substr(0, q)) { if (ch == '/') { parts.push_back(cur); cur.clear(); } else cur += ch; }
            parts.push_back(cur);
            if (parts.size() < 5 || parts[1] != "mods" || parts[3] != "files") return fail("not an nxm:// download link");
            s = source(c, "nexus");
            mod.source = "nexus";
            mod.id = parts[2];
            mod.name = "Nexus mod " + parts[2];
            mod.page_url = "https://www.nexusmods.com/" + parts[0] + "/mods/" + parts[2];
            file.id = parts[4];
            file.name = "nexus_" + parts[2] + "_" + parts[4] + ".zip";
            file.url = "nexus:" + parts[2] + ":" + parts[4];
            file.md5 = "nxm:" + qs;
            return do_install(c, s, mod, file, "", opt("allow") == "1");
        }
        if (lw.compare(0, 7, "http://") == 0) return fail("refused: only https:// links");
        if (lw.compare(0, 8, "https://") == 0) {
            size_t q = file.name.find('?');
            if (q != std::string::npos) file.name = file.name.substr(0, q);
            mod.source = "url";
            mod.id = what;
            mod.page_url = what;
        } else {
            if (!is_file(what)) return fail("no such file: " + what);
            mod.source = "file";
            mod.id = base_name(what);
        }
        mod.name = opt("name", file.name.substr(0, file.name.size() - file_ext(file.name).size()).c_str());
        if (mod.name.empty()) mod.name = "Downloaded mod";
        return do_install(c, nullptr, mod, file, what, opt("allow") == "1");
    }
    if (cmd == "installed") {
        std::string o = "{\"type\":\"installed\",\"mods\":[";
        std::vector<InstalledMeta> v = list_installed(c.p.mods_root);
        for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + jinstalled(v[i]);
        out(o + "]}");
        return 0;
    }
    if (cmd == "uninstall") {
        std::string err;
        bool deferred = false;
        if (!uninstall_mod(opt("folder"), c.p.mods_root, &err, &deferred)) return fail(err);
        out(std::string("{\"type\":\"ok\",\"deferred\":") + (deferred ? "true" : "false") + "}");
        return 0;
    }
    if (cmd == "enable") {
        std::string folder = opt("folder"), marker = join(folder, ".disabled");
        std::string cf = canonical(folder), cr = canonical(c.p.mods_root);
        if (cf.compare(0, cr.size(), cr) != 0 || !is_dir(folder)) return fail("not a mod folder inside the mods folder");
        bool on = opt("on", "1") == "1";
        bool ok = on ? (!is_file(marker) || DeleteFileW(wide(marker).c_str())) : write_text_atomic(marker, "");
        if (!ok) return fail("cannot change " + marker);
        out("{\"type\":\"ok\"}");
        return 0;
    }
    if (cmd == "check-updates") {
        std::string o = "{\"type\":\"updates\",\"mods\":[";
        bool first = true;
        for (const auto &m : list_installed(c.p.mods_root)) {
            auto s = source(c, m.source);
            std::string why, latest, err;
            if (!s || !s->available(&why) || !s->check_update(m, &latest, &err) || !version_newer(latest, m.version)) continue;
            o += (first ? "" : ",") + std::string("{\"folder\":") + json_quote(m.folder) + ",\"latest\":" + json_quote(latest) + "}";
            first = false;
        }
        out(o + "]}");
        return 0;
    }
    return fail("unknown command " + cmd);
}
