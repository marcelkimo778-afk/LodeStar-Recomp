// mb_install.cpp - see mb_install.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#include "mb_install.h"

namespace mb {

const char *const kTitleId = "01001F5010DFA000";

// ================================================================== version compatibility

const char *compat_name(Compat c) { return c == COMPAT_MATCH ? "match" : c == COMPAT_MISMATCH ? "mismatch" : "unknown"; }

std::string normalize_game_version(const std::string &in)
{
    std::string s = lower(trim(in));
    if (!s.empty() && s[0] == 'v') s.erase(0, 1);
    int part[3] = {0, 0, 0}, n = 0;
    size_t i = 0;
    while (n < 3 && i < s.size() && isdigit((unsigned char)s[i])) {
        int v = 0, d = 0;
        while (i < s.size() && isdigit((unsigned char)s[i]) && d < 4) { v = v * 10 + (s[i] - '0'); i++; d++; }
        part[n++] = v;
        if (i < s.size() && s[i] == '.') i++; else break;
    }
    if (n < 2) return "";
    char b[32];
    snprintf(b, sizeof b, "%d.%d.%d", part[0], part[1], part[2]);
    return b;
}

// game versions Legends: Arceus was released in (keyword search accepts only these: a mod's own "v2.3" is not a
// game version)
static const char *kGameVersions[] = {"1.0.0", "1.0.1", "1.0.2", "1.1.0", "1.1.1"};

static bool known_game_version(const std::string &v)
{
    for (const char *k : kGameVersions) if (v == k) return true;
    return false;
}

// "v1.0.0", "1.1.1", "ver 1.0.2", "version 1.0", "(1.0.0)": game versions named in free text
static std::vector<std::string> keyword_versions(const std::string &text)
{
    std::vector<std::string> out;
    std::string t = lower(text);
    for (size_t i = 0; i < t.size(); i++) {
        if (!isdigit((unsigned char)t[i]) || (i && (isdigit((unsigned char)t[i - 1]) || t[i - 1] == '.'))) continue;
        size_t j = i;
        while (j < t.size() && (isdigit((unsigned char)t[j]) || t[j] == '.')) j++;
        std::string tok = t.substr(i, j - i);
        while (!tok.empty() && tok.back() == '.') tok.pop_back();
        if (std::count(tok.begin(), tok.end(), '.') < 1) continue;
        std::string v = normalize_game_version(tok);
        // "1.0" alone is too common in prose; 3-part versions count anywhere, 2-part ones only after v / ver / version
        bool three = std::count(tok.begin(), tok.end(), '.') >= 2;
        size_t k = i;
        while (k && t[k - 1] == ' ') k--;
        bool prefixed = (k && t[k - 1] == 'v') || (k >= 3 && t.compare(k - 3, 3, "ver") == 0) ||
                        (k >= 7 && t.compare(k - 7, 7, "version") == 0);
        if (known_game_version(v) && (three || prefixed) &&
            std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
        i = j;
    }
    return out;
}

static VersionInfo from_versions(const std::vector<std::string> &vs, const Target &t, const char *where)
{
    VersionInfo r;
    if (vs.empty()) return r;
    bool hit = std::find(vs.begin(), vs.end(), normalize_game_version(t.version)) != vs.end();
    r.compat = hit ? COMPAT_MATCH : COMPAT_MISMATCH;
    r.detected = hit ? normalize_game_version(t.version) : vs[0];
    r.why = std::string(where) + (vs.size() > 1 ? " (names " + std::to_string(vs.size()) + " versions)" : "");
    return r;
}

VersionInfo detect_version_meta(const ModDetails &mod, const ModFile *file, const Target &t)
{
    // (1) the source's own field
    std::string gv = file && !file->game_version.empty() ? file->game_version : mod.game_version;
    if (!gv.empty()) {
        std::vector<std::string> vs;
        std::string cur;
        for (char c : gv + ",") {                              // "1.0.0, 1.1.1" lists
            if (c == ',' || c == ';' || c == '/' || c == '|') {
                std::string v = normalize_game_version(cur);
                if (!v.empty()) vs.push_back(v);
                cur.clear();
            } else cur += c;
        }
        if (!vs.empty()) return from_versions(vs, t, "the source's game version field");
    }
    // (3) keywords in the name, description and file names (never the mod's own version field)
    std::string text = mod.name + "\n" + mod.summary + "\n" + mod.description;
    if (file) text += "\n" + file->name + "\n" + file->description;
    for (const auto &f : mod.files) text += "\n" + f.name;
    return from_versions(keyword_versions(text), t, "a version named in the mod's text");
}

// build ids of the exefs patches / cheats in an extracted tree: (id, where)
static void tree_build_ids(const std::string &dir, std::vector<std::pair<std::string, std::string>> *ids)
{
    std::vector<Entry> es;
    walk(dir, &es, 200000);
    for (const auto &e : es) {
        if (e.dir || e.link) continue;
        std::string name = base_name(e.rel), ext = file_ext(name);
        std::string stem = name.substr(0, name.size() - ext.size());
        bool hex = stem.size() >= 16 && stem.size() <= 64 &&
                   std::all_of(stem.begin(), stem.end(), [](char c) { return isxdigit((unsigned char)c) != 0; });
        if ((ext == ".ips" || ext == ".ips32") && hex) ids->push_back({lower(stem), e.rel});
        else if (ext == ".txt" && hex && lower(e.rel).find("cheats/") != std::string::npos) ids->push_back({lower(stem), e.rel});
        else if (ext == ".pchtxt") {
            std::string text;
            if (!read_text(join(dir, e.rel), &text, 4u << 20)) continue;
            size_t p = lower(text).find("@nsobid-");
            if (p == std::string::npos) continue;
            size_t q = p + 8;
            while (q < text.size() && isxdigit((unsigned char)text[q])) q++;
            if (q - p - 8 >= 16) ids->push_back({lower(text.substr(p + 8, q - p - 8)), e.rel});
        }
    }
}

VersionInfo detect_version_tree(const std::string &dir, const ModDetails &mod, const ModFile *file, const Target &t)
{
    // (2) build ids are exact: they decide when present
    std::vector<std::pair<std::string, std::string>> ids;
    tree_build_ids(dir, &ids);
    std::string target = lower(t.build_id);
    if (!ids.empty() && !target.empty()) {
        VersionInfo r;
        r.code_specific = true;
        std::string bad;
        for (const auto &id : ids)
            if (target.compare(0, id.first.size(), id.first) != 0) { bad = id.first + " in " + id.second; break; }
        if (bad.empty()) {
            r.compat = COMPAT_MATCH;
            r.detected = normalize_game_version(t.version).empty() ? "build " + t.build_id.substr(0, 16)
                                                                   : normalize_game_version(t.version);
            r.why = "the build id of its exefs patches / cheats";
        } else {
            r.compat = COMPAT_MISMATCH;
            std::string other = bad.substr(0, bad.find(' '));
            for (auto &c : other) c = (char)toupper((unsigned char)c);
            r.detected = "build " + other.substr(0, 16);
            r.why = "exefs patch / cheat for another build: " + bad;
        }
        return r;
    }
    VersionInfo m = detect_version_meta(mod, file, t);
    if (m.compat != COMPAT_UNKNOWN) return m;
    // (3) also file and folder names inside the archive
    std::vector<Entry> es;
    walk(dir, &es, 200000);
    std::string names;
    for (const auto &e : es) names += e.rel + "\n";
    return from_versions(keyword_versions(names), t, "a version in the archive's file names");
}

bool version_newer(const std::string &a, const std::string &b)
{
    auto parts = [](const std::string &s) {
        std::vector<long> v;
        long cur = -1;
        for (char c : s) {
            if (isdigit((unsigned char)c)) cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            else if (cur >= 0) { v.push_back(cur); cur = -1; }
        }
        if (cur >= 0) v.push_back(cur);
        return v;
    };
    std::vector<long> x = parts(a), y = parts(b);
    if (x.empty() || y.empty()) return false;        // not comparable: never claim an update
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p > q;
    }
    return false;
}

// ================================================================== archive checks

static bool bad_member(const std::string &name0, std::string *why)
{
    std::string n = name0;
    for (auto &c : n) if (c == '\\') c = '/';
    if (n.empty()) return false;
    if (n[0] == '/' || (n.size() > 1 && n[1] == ':')) { *why = "absolute path \"" + name0 + "\""; return true; }
    size_t s = 0;
    while (s <= n.size()) {
        size_t e = n.find('/', s);
        if (e == std::string::npos) e = n.size();
        if (n.compare(s, e - s, "..") == 0 && e - s == 2) { *why = "path leaves the archive (\"" + name0 + "\")"; return true; }
        s = e + 1;
    }
    for (unsigned char c : n) if (c < 0x20) { *why = "control character in a file name"; return true; }
    return false;
}

static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const unsigned char *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

// a zip's central directory: names, declared sizes, symlinks (before anything is written to disk)
static bool zip_precheck(const std::string &path, const InstallOptions &o, std::string *err)
{
    HANDLE f = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { *err = "cannot open the archive"; return false; }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    auto readat = [&](uint64_t off, void *buf, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        DWORD got = 0;
        return SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
    };
    uint64_t fsize = (uint64_t)sz.QuadPart;
    DWORD tail = (DWORD)std::min<uint64_t>(fsize, 65557);
    std::vector<unsigned char> t(tail);
    bool ok = tail >= 22 && readat(fsize - tail, t.data(), tail);
    long eocd = -1;
    for (long i = (long)tail - 22; ok && i >= 0; i--) if (rd32(&t[(size_t)i]) == 0x06054b50) { eocd = i; break; }
    if (!ok || eocd < 0) { CloseHandle(f); *err = "not a valid zip file (no central directory)"; return false; }
    uint64_t count = rd16(&t[(size_t)eocd + 10]), cd_size = rd32(&t[(size_t)eocd + 12]), cd_off = rd32(&t[(size_t)eocd + 16]);
    if ((count == 0xFFFF || cd_off == 0xFFFFFFFFu) && eocd >= 20 && rd32(&t[(size_t)eocd - 20]) == 0x07064b50) {
        unsigned char z[56];
        uint64_t z64 = rd64(&t[(size_t)eocd - 20 + 8]);
        if (readat(z64, z, 56) && rd32(z) == 0x06064b50) { count = rd64(z + 32); cd_size = rd64(z + 40); cd_off = rd64(z + 48); }
    }
    if (count > o.max_files) { CloseHandle(f); *err = "too many files in the archive (" + std::to_string(count) + ")"; return false; }
    if (cd_size > (64u << 20) || cd_off + cd_size > fsize) { CloseHandle(f); *err = "damaged zip file"; return false; }
    std::vector<unsigned char> cd((size_t)cd_size);
    if (cd_size && !readat(cd_off, cd.data(), (DWORD)cd_size)) { CloseHandle(f); *err = "cannot read the archive"; return false; }
    CloseHandle(f);
    uint64_t total = 0, n = 0;
    for (size_t p = 0; p + 46 <= cd.size() && n < count; n++) {
        if (rd32(&cd[p]) != 0x02014b50) { *err = "damaged zip directory"; return false; }
        uint32_t made = rd16(&cd[p + 4]);
        uint64_t usize = rd32(&cd[p + 24]);
        uint32_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
        uint32_t attr = rd32(&cd[p + 38]);
        if (p + 46 + nl + xl + cl > cd.size()) { *err = "damaged zip directory"; return false; }
        std::string name((const char *)&cd[p + 46], nl);
        if (usize == 0xFFFFFFFFu) {                               // zip64 extra field
            for (size_t x = p + 46 + nl; x + 4 <= p + 46 + nl + xl;) {
                uint32_t id = rd16(&cd[x]), len = rd16(&cd[x + 2]);
                if (id == 1 && len >= 8) { usize = rd64(&cd[x + 4]); break; }
                x += 4 + len;
            }
        }
        std::string why;
        if (bad_member(name, &why)) { *err = "unsafe archive: " + why; return false; }
        if ((made >> 8) == 3 && ((attr >> 16) & 0xF000) == 0xA000) { *err = "unsafe archive: symbolic link \"" + name + "\""; return false; }
        total += usize;
        if (total > o.max_unpacked) { *err = "the archive unpacks to more than the size limit (zip bomb?)"; return false; }
        p += 46 + nl + xl + cl;
    }
    return true;
}

static std::string tar_path(const InstallOptions &o)
{
    if (!o.tar_exe.empty()) return o.tar_exe;
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return n && n < MAX_PATH ? utf8(sys) + "\\tar.exe" : std::string("C:\\Windows\\System32\\tar.exe");
}

// executables and scripts are never installed
static bool blocked_ext(const std::string &ext)
{
    static const char *k[] = {".exe", ".dll", ".so", ".dylib", ".bat", ".cmd", ".sh", ".ps1", ".psm1", ".psd1", ".msi",
                              ".msp", ".com", ".scr", ".vbs", ".vbe", ".js", ".jse", ".wsf", ".wsh", ".hta", ".cpl",
                              ".jar", ".py", ".pyw", ".lnk", ".reg", ".app", ".nro", ".nso", ".kip", ".sys", ".drv",
                              ".ocx", ".appx", ".msix", ".pif", ".url", ".inf", ".elf", ".apk", ".deb", ".rpm"};
    for (const char *b : k) if (ext == b) return true;
    return false;
}
static bool doc_ext(const std::string &e)
{
    return e == ".txt" || e == ".md" || e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".webp" || e == ".gif" ||
           e == ".pdf" || e == ".rtf";
}

static std::string child_i(const std::string &dir, const char *name, bool want_dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(join(dir, "*")).c_str(), &fd);
    std::string out;
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d == want_dir && lower(utf8(fd.cFileName)) == lower(name)) { out = join(dir, utf8(fd.cFileName)); break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static bool has_mod_dirs(const std::string &d)
{
    return !child_i(d, "romfs", true).empty() || !child_i(d, "exefs", true).empty() || !child_i(d, "cheats", true).empty();
}

// mod roots inside the extracted tree: folders holding romfs/ exefs/ cheats/ (outermost first); else a bare RomFS
// (a folder holding bin/ or system_resource/) or loose patch files
struct Root { std::string dir; int kind; };   // kind 0: romfs/exefs/cheats inside, 1: dir is a bare romfs, 2: loose patches
static void find_roots(const std::string &top, std::vector<Root> *out)
{
    std::vector<std::string> level = {top};
    for (int depth = 0; depth < 5 && out->empty() && !level.empty(); depth++) {
        std::vector<std::string> next;
        for (const auto &d : level) {
            if (has_mod_dirs(d)) { out->push_back({d, 0}); continue; }
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(wide(join(d, "*")).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                    continue;
                next.push_back(join(d, utf8(fd.cFileName)));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        level.swap(next);
    }
    if (!out->empty()) return;
    // bare RomFS: the outermost folder with bin/ or system_resource/
    level = {top};
    for (int depth = 0; depth < 5 && out->empty() && !level.empty(); depth++) {
        std::vector<std::string> next;
        for (const auto &d : level) {
            if (!child_i(d, "bin", true).empty() || !child_i(d, "system_resource", true).empty()) { out->push_back({d, 1}); continue; }
            std::vector<Entry> es;
            walk(d, &es, 4096);
            for (const auto &e : es) if (e.dir && e.rel.find('/') == std::string::npos) next.push_back(join(d, e.rel));
        }
        level.swap(next);
    }
    if (!out->empty()) return;
    std::vector<Entry> es;
    walk(top, &es);
    for (const auto &e : es)
        if (!e.dir && (ends_with_i(e.rel, ".pchtxt") || ends_with_i(e.rel, ".ips") || ends_with_i(e.rel, ".ips32"))) {
            out->push_back({top, 2});
            return;
        }
}

// copy the allowed content of one root into stage (romfs/, exefs/, cheats/, docs)
static bool stage_root(const Root &rt, const std::string &stage, InstallResult *r)
{
    if (!make_dirs(stage)) { r->error = "cannot create " + stage; return false; }
    auto copy_rel = [&](const std::string &from_dir, const std::string &to_dir, const std::function<bool(const std::string &)> &keep,
                        const std::string &label) {
        std::vector<Entry> es;
        walk(from_dir, &es);
        for (const auto &e : es) {
            if (e.dir) continue;
            if (!keep(e.rel)) { r->dropped.push_back(label + e.rel); continue; }
            std::string dst = join(to_dir, e.rel);
            size_t sl = dst.find_last_of("/\\");
            if (sl != std::string::npos) make_dirs(dst.substr(0, sl));
            if (!move_path(join(from_dir, e.rel), dst)) { r->error = "cannot copy " + e.rel; return false; }
        }
        return true;
    };
    auto any = [](const std::string &) { return true; };
    auto patch = [](const std::string &rel) {
        std::string e = file_ext(rel);
        return rel.find('/') == std::string::npos && (e == ".ips" || e == ".ips32" || e == ".pchtxt");
    };
    auto cheat = [](const std::string &rel) { return file_ext(rel) == ".txt"; };
    if (rt.kind == 1) return copy_rel(rt.dir, join(stage, "romfs"), any, "");
    if (rt.kind == 2) return copy_rel(rt.dir, join(stage, "exefs"), [&](const std::string &rel) {
        std::string e = file_ext(rel);
        return e == ".ips" || e == ".ips32" || e == ".pchtxt";
    }, "");
    std::string d;
    if (!(d = child_i(rt.dir, "romfs", true)).empty() && !copy_rel(d, join(stage, "romfs"), any, "romfs/")) return false;
    if (!(d = child_i(rt.dir, "exefs", true)).empty() && !copy_rel(d, join(stage, "exefs"), patch, "exefs/")) return false;
    if (!(d = child_i(rt.dir, "cheats", true)).empty() && !copy_rel(d, join(stage, "cheats"), cheat, "cheats/")) return false;
    // readme / pictures next to romfs/ ...; everything else stays out
    std::vector<Entry> es;
    walk(rt.dir, &es, 4096);
    for (const auto &e : es) {
        if (e.rel.find('/') != std::string::npos) continue;
        std::string l = lower(e.rel);
        if (e.dir) {
            if (l != "romfs" && l != "exefs" && l != "cheats") r->dropped.push_back(e.rel + "/");
            continue;
        }
        if (doc_ext(file_ext(e.rel)) && l != "mod_meta.json") {
            if (!move_path(join(rt.dir, e.rel), join(stage, e.rel))) { r->error = "cannot copy " + e.rel; return false; }
        } else r->dropped.push_back(e.rel);
    }
    return true;
}

static std::string meta_json(const ModDetails &m, const ModFile &f, const VersionInfo &v, const std::string &variant)
{
    std::string s = "{\n";
    auto kv = [&](const char *k, const std::string &val, bool last = false) {
        s += "  " + json_quote(k) + ": " + json_quote(val) + (last ? "\n" : ",\n");
    };
    kv("source", m.source);
    kv("mod_id", m.id);
    kv("name", m.name);
    kv("author", m.author);
    kv("version", !f.version.empty() ? f.version : m.version);
    kv("file_id", f.id);
    kv("file_name", f.name);
    kv("url", m.page_url);
    kv("installed_at", iso_time(now_unix()));
    kv("variant", variant);
    kv("detected_version", v.detected);
    kv("compat", compat_name(v.compat));
    kv("compat_why", v.why, true);
    return s + "}\n";
}

bool read_meta(const std::string &folder, InstalledMeta *m)
{
    std::string text;
    if (!read_text(join(folder, "mod_meta.json"), &text, 1u << 20)) return false;
    JsonP j = json_parse(text);
    if (!j || j->t != Json::Obj) return false;
    const Json &J = *j;
    m->folder = folder;
    m->source = J["source"].str();
    m->mod_id = J["mod_id"].str();
    m->name = J["name"].str();
    m->author = J["author"].str();
    m->version = J["version"].str();
    m->file_id = J["file_id"].str();
    m->file_name = J["file_name"].str();
    m->url = m->page_url = J["url"].str();
    m->installed_at = J["installed_at"].str();
    m->detected_version = J["detected_version"].str();
    m->compat = J["compat"].str("unknown");
    m->compat_why = J["compat_why"].str();
    return true;
}

std::vector<InstalledMeta> list_installed(const std::string &mods_root)
{
    std::vector<InstalledMeta> out;
    std::string tid = join(mods_root, kTitleId);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(join(tid, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        InstalledMeta m;
        if (read_meta(join(tid, utf8(fd.cFileName)), &m)) out.push_back(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const InstalledMeta &a, const InstalledMeta &b) { return lower(a.name) < lower(b.name); });
    return out;
}

static std::string tmp_root(const std::string &mods_root) { return join(mods_root, ".mb_tmp"); }

void cleanup_tmp(const std::string &mods_root)
{
    std::string t = tmp_root(mods_root);
    if (is_dir(t)) remove_tree(t);
}

// the folder for a new install: <tid>/<Name>; an existing folder of the same source + mod id is an update
static std::string target_folder(const std::string &tid, const std::string &name, const ModDetails &m, bool *update)
{
    std::string base = safe_folder_name(name);
    *update = false;
    for (int k = 1; k < 1000; k++) {
        std::string cand = join(tid, k == 1 ? base : base + " (" + std::to_string(k) + ")");
        if (!is_dir(cand)) return cand;
        InstalledMeta old;
        if (read_meta(cand, &old) && old.source == m.source && old.mod_id == m.id) { *update = true; return cand; }
    }
    return "";
}

bool install_archive(const std::string &archive, const ModDetails &mod, const ModFile &file, const InstallOptions &o,
                     InstallResult *r)
{
    *r = InstallResult();
    if (o.mods_root.empty()) { r->error = "no mods folder"; return false; }
    std::string job = join(tmp_root(o.mods_root), temp_name("job-"));
    std::string ex = join(job, "x"), st = join(job, "s");
    auto fail = [&](const std::string &e) {
        if (!e.empty()) r->error = e;
        remove_tree(job);                                     // nothing half-installed stays behind
        r->ok = false;
        return false;
    };
    if (!make_dirs(ex)) return fail("cannot create a temporary folder in " + o.mods_root);
    std::string ext = file_ext(archive.size() > 4 ? archive : file.name);
    // 1. before anything is written: the archive's own directory
    std::string err;
    std::string tar = tar_path(o);
    if (ext == ".zip" || (file_ext(file.name) == ".zip")) {
        if (!zip_precheck(archive, o, &err)) return fail(err);
    } else {
        if (!is_file(tar)) return fail("7z / rar need Windows' tar.exe (Windows 10 1803 or newer): " + tar + " is missing");
        std::string list;
        int rc = run_process(tar, {"-tf", archive}, &list, 64u << 20, 120000, nullptr);
        if (rc != 0) return fail("cannot read the archive (not a zip / 7z / rar, or damaged)");
        size_t count = 0, s = 0;
        while (s < list.size()) {
            size_t e = list.find('\n', s);
            if (e == std::string::npos) e = list.size();
            std::string name = list.substr(s, e - s);
            if (!name.empty() && name.back() == '\r') name.pop_back();
            std::string why;
            if (!name.empty() && bad_member(name, &why)) return fail("unsafe archive: " + why);
            if (!name.empty() && ++count > o.max_files) return fail("too many files in the archive");
            s = e + 1;
        }
    }
    // 2. extract; a watcher stops tar when the folder grows past the limit (archives that lie about their size)
    if (!is_file(tar)) return fail("Windows' tar.exe is missing: " + tar);
    ULONGLONG last_check = 0;
    bool too_big = false;
    auto watch = [&]() {
        ULONGLONG now = GetTickCount64();
        if (now - last_check < 300) return false;
        last_check = now;
        std::vector<Entry> es;
        walk(ex, &es, (size_t)o.max_files + 2);
        uint64_t sum = 0;
        for (const auto &e : es) sum += e.size;
        too_big = sum > o.max_unpacked || es.size() > o.max_files;
        return too_big;
    };
    std::string out;
    int rc = run_process(tar, {"-xf", archive, "-C", ex}, &out, 1u << 20, 30 * 60 * 1000, watch);
    if (too_big || rc == -2) return fail("the archive unpacks to more than the size or file limit (zip bomb?): refused");
    if (rc != 0) return fail("extracting failed: " + trim(out).substr(0, 300));
    // 3. what is on disk now: links refused, sizes checked again, executables removed
    std::vector<Entry> es;
    walk(ex, &es, (size_t)o.max_files + 2);
    uint64_t sum = 0;
    for (const auto &e : es) {
        if (e.link) return fail("unsafe archive: it contains a link (" + e.rel + ")");
        sum += e.size;
    }
    if (sum > o.max_unpacked || es.size() > o.max_files) return fail("the archive unpacks to more than the size or file limit");
    std::string cex = canonical(ex);
    for (const auto &e : es) {
        if (canonical(join(ex, e.rel)).compare(0, cex.size(), cex) != 0) return fail("unsafe archive: a path leaves the folder");
        if (!e.dir && blocked_ext(file_ext(e.rel))) {
            r->refused.push_back(e.rel);
            DeleteFileW(wide(join(ex, e.rel)).c_str());
        }
    }
    // 4. version compatibility from the content (build ids decide)
    r->version = detect_version_tree(ex, mod, &file, o.target);
    if (r->version.compat == COMPAT_MISMATCH && r->version.code_specific && !o.allow_wrong_version) {
        r->blocked_version = true;
        return fail("made for another game version (" + r->version.why + "). Its exefs patches / cheats would not "
                    "match this game: install anyway only if you know it works.");
    }
    if (r->version.compat == COMPAT_MISMATCH)
        r->warnings.push_back("made for another game version (" + r->version.detected + ", " + r->version.why +
                              "): romfs files may not fit this version");
    // 5. layout
    std::vector<Root> roots;
    find_roots(ex, &roots);
    if (roots.empty()) return fail("no mod content found (no romfs/, exefs/ or cheats/ folder, no bin/ folder, no patches)");
    std::string tid = join(o.mods_root, kTitleId);
    if (!make_dirs(tid)) return fail("cannot create " + tid);
    struct Plan { std::string stage, final_dir, variant; bool update; };
    std::vector<Plan> plans;
    for (size_t i = 0; i < roots.size(); i++) {
        std::string variant = roots.size() > 1 ? base_name(roots[i].dir) : "";
        std::string name = variant.empty() ? mod.name : mod.name + " - " + variant;
        Plan p;
        p.stage = join(st, std::to_string(i));
        p.variant = variant;
        if (!stage_root(roots[i], p.stage, r)) return fail("");
        write_text_atomic(join(p.stage, "mod_meta.json"), meta_json(mod, file, r->version, variant));
        if (i > 0) write_text_atomic(join(p.stage, ".disabled"), "");   // variants: the first one is on
        p.final_dir = target_folder(tid, name, mod, &p.update);
        if (p.final_dir.empty()) return fail("cannot choose a folder name");
        if (p.update && is_file(join(p.final_dir, ".disabled"))) write_text_atomic(join(p.stage, ".disabled"), "");
        plans.push_back(p);
    }
    if (roots.size() > 1)
        r->warnings.push_back("the archive has " + std::to_string(roots.size()) + " variants: each is its own mod, only "
                              "the first is switched on");
    // 6. move into place; an update swaps the old folder out first and puts it back if the move fails
    std::vector<std::pair<std::string, std::string>> done;    // (final, old copy or "")
    for (size_t i = 0; i < plans.size(); i++) {
        const Plan &p = plans[i];
        std::string old;
        if (p.update) {
            old = join(job, "old" + std::to_string(i));
            if (!move_path(p.final_dir, old)) {
                for (auto &d : done) { remove_tree(d.first); if (!d.second.empty()) move_path(d.second, d.first); }
                return fail("cannot replace the installed version (files in use? close the game's mod files)");
            }
        }
        if (!move_path(p.stage, p.final_dir)) {
            if (!old.empty()) move_path(old, p.final_dir);
            for (auto &d : done) { remove_tree(d.first); if (!d.second.empty()) move_path(d.second, d.first); }
            return fail("cannot move the mod into " + p.final_dir);
        }
        done.push_back({p.final_dir, old});
        r->folders.push_back(p.final_dir);
    }
    remove_tree(job);
    r->ok = true;
    return true;
}

bool verify_download(const std::string &path, const ModFile &f, std::string *err)
{
    std::string h;
    if (!f.sha256.empty()) {
        if (!file_sha256(path, &h)) { *err = "cannot read the download"; return false; }
        if (h != lower(f.sha256)) { *err = "the download is damaged (SHA-256 does not match the source)"; return false; }
    } else if (f.md5.size() == 32) {
        if (!file_md5(path, &h)) { *err = "cannot read the download"; return false; }
        if (h != lower(f.md5)) { *err = "the download is damaged (MD5 does not match the source)"; return false; }
    }
    return true;
}

bool uninstall_mod(const std::string &folder, const std::string &mods_root, std::string *err, bool *deferred)
{
    if (deferred) *deferred = false;
    std::string cf = canonical(folder), cr = canonical(mods_root);
    if (cf.size() <= cr.size() + 1 || cf.compare(0, cr.size(), cr) != 0 || cf[cr.size()] != '/') {
        *err = "refused: the folder is not inside the mods folder";
        return false;
    }
    if (!is_dir(folder)) { *err = "already removed"; return false; }
    std::string trash = join(tmp_root(mods_root), temp_name("del-"));
    make_dirs(tmp_root(mods_root));
    if (!move_path(folder, trash)) { *err = "cannot remove it (files in use?)"; return false; }
    if (!remove_tree(trash) && deferred) *deferred = true;   // the rest goes with cleanup_tmp at the next start
    return true;
}

}  // namespace mb
