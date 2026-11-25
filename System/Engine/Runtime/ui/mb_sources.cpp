// mb_sources.cpp - the mod browser's sources (mb_source.h) and its settings file.
//
// Verification state (also in AUTO_LOG.md, Phase 5):
//   mock        offline fixture provider, fully tested (Engine/Tests/test_modbrowser.bat)
//   mod.io      written against the official REST docs (docs.mod.io: introduction, get-mods, filtering, sorting,
//               pagination, rate limiting, mod object); NOT checked against the live API (no key in this session)
//   GitHub      checked live 2026-11-25: repository file lists (git/trees) + releases, see GithubSource
//   GameBanana  UNVERIFIED AGAINST THE LIVE API: its apiv11 documentation page was empty when read and the documented
//               legacy API has no field list; the apiv11 calls below follow the public site's API from memory and are
//               parsed defensively (every field optional).  Errors are shown in the tab, never fatal.
//   Nexus Mods  UNVERIFIED AGAINST THE LIVE API: the v1 REST reference was not reachable (HTTP 404); the endpoints
//               below follow the public v1 API from memory.  Personal API key only, as Nexus' acceptable-use policy
//               allows for personal use; downloads without premium open the mod's page in the browser.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <algorithm>
#include <thread>
#include <map>

#include "mb_http.h"
#include "mb_source.h"

namespace mb {

// ================================================================== settings (Config/modbrowser.json)

bool settings_load(const std::string &path, BrowserSettings *s)
{
    std::string text;
    if (!read_text(path, &text, 1u << 20)) return false;
    JsonP j = json_parse(text);
    if (!j || j->t != Json::Obj) return false;
    const Json &J = *j;
    const Json &src = J["sources"];
    if (src.has("gamebanana")) s->gamebanana = src["gamebanana"].truthy();
    if (src.has("nexus")) s->nexus = src["nexus"].truthy();
    if (src.has("modio")) s->modio = src["modio"].truthy();
    if (src.has("github")) s->github = src["github"].truthy();
    if (J.has("check_updates")) s->check_updates = J["check_updates"].truthy();
    if (J.has("notice_shown")) s->notice_shown = J["notice_shown"].truthy();
    if (J.has("only_compatible")) s->only_compatible = J["only_compatible"].truthy();
    s->nexus_key = J["nexus_api_key"].str();
    s->nexus_game_domain = J["nexus_game_domain"].str();
    if (trim(s->nexus_game_domain).empty()) s->nexus_game_domain = "pokemonlegendsarceus";   // this game on Nexus Mods
    s->modio_key = J["modio_api_key"].str();
    s->modio_game_id = J["modio_game_id"].str();
    // the number only: a pasted address ("https://gamebanana.com/games/15053") gives its last run of digits
    {
        std::string g = J["gamebanana_game_id"].str(), num;
        for (size_t i = g.size(); i-- > 0;) {
            if (isdigit((unsigned char)g[i])) num.insert(num.begin(), g[i]);
            else if (!num.empty()) break;
        }
        s->gamebanana_game_id = num;
    }
    s->github_repos.clear();
    for (const auto &r : J["github_repos"].a) if (r->t == Json::Str && !r->s.empty()) s->github_repos.push_back(r->s);
    s->github_names.clear();
    for (const auto &kv : J["github_names"].o) if (kv.second && kv.second->t == Json::Str) s->github_names.push_back({kv.first, kv.second->s});
    s->github_removed.clear();
    for (const auto &r : J["github_removed"].a) if (r->t == Json::Str && !r->s.empty()) s->github_removed.push_back(r->s);
    s->github_index = J["github_index"].str();
    if (J["max_download_mb"].num() > 0) s->max_download = (uint64_t)J["max_download_mb"].num() << 20;
    if (J["max_unpacked_mb"].num() > 0) s->max_unpacked = (uint64_t)J["max_unpacked_mb"].num() << 20;
    if (J["max_files"].num() > 0) s->max_files = (uint32_t)J["max_files"].num();
    if (!J["target_version"].str().empty()) s->target_version = J["target_version"].str();
    return true;
}

bool settings_save(const std::string &path, const BrowserSettings &s)
{
    std::string o = "{\n";
    o += "  \"sources\": {\"gamebanana\": " + std::string(s.gamebanana ? "true" : "false") +
         ", \"nexus\": " + (s.nexus ? "true" : "false") + ", \"modio\": " + (s.modio ? "true" : "false") +
         ", \"github\": " + (s.github ? "true" : "false") + "},\n";
    o += "  \"check_updates\": " + std::string(s.check_updates ? "true" : "false") + ",\n";
    o += "  \"notice_shown\": " + std::string(s.notice_shown ? "true" : "false") + ",\n";
    o += "  \"only_compatible\": " + std::string(s.only_compatible ? "true" : "false") + ",\n";
    o += "  \"target_version\": " + json_quote(s.target_version) + ",\n";
    o += "  \"gamebanana_game_id\": " + json_quote(s.gamebanana_game_id) + ",\n";
    o += "  \"nexus_api_key\": " + json_quote(s.nexus_key) + ",\n";
    o += "  \"nexus_game_domain\": " + json_quote(s.nexus_game_domain) + ",\n";
    o += "  \"modio_api_key\": " + json_quote(s.modio_key) + ",\n";
    o += "  \"modio_game_id\": " + json_quote(s.modio_game_id) + ",\n";
    o += "  \"github_names\": {";
    for (size_t i = 0; i < s.github_names.size(); i++)
        o += (i ? ", " : "") + json_quote(s.github_names[i].first) + ": " + json_quote(s.github_names[i].second);
    o += "},\n  \"github_removed\": [";
    for (size_t i = 0; i < s.github_removed.size(); i++) o += (i ? ", " : "") + json_quote(s.github_removed[i]);
    o += "],\n";
    if (!s.github_index.empty()) o += "  \"github_index\": " + json_quote(s.github_index) + ",\n";
    o += "  \"github_repos\": [";
    for (size_t i = 0; i < s.github_repos.size(); i++) o += (i ? ", " : "") + json_quote(s.github_repos[i]);
    o += "],\n";
    o += "  \"max_download_mb\": " + std::to_string(s.max_download >> 20) + ",\n";
    o += "  \"max_unpacked_mb\": " + std::to_string(s.max_unpacked >> 20) + ",\n";
    o += "  \"max_files\": " + std::to_string(s.max_files) + "\n}\n";
    return write_text_atomic(path, o);
}

static std::string url_enc(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

static bool icontains(const std::string &hay, const std::string &needle)
{
    return needle.empty() || lower(hay).find(lower(needle)) != std::string::npos;
}

// strip HTML to readable text (descriptions come as HTML from several sources)
static std::string strip_html(const std::string &h)
{
    std::string o;
    bool tag = false;
    for (size_t i = 0; i < h.size(); i++) {
        char c = h[i];
        if (c == '<') {
            tag = true;
            std::string t = lower(h.substr(i, 5));
            if (t.compare(0, 3, "<br") == 0 || t.compare(0, 3, "<p>") == 0 || t.compare(0, 4, "</p>") == 0 ||
                t.compare(0, 4, "<li>") == 0) o += '\n';
            continue;
        }
        if (c == '>') { tag = false; continue; }
        if (tag) continue;
        if (c == '&') {
            static const struct { const char *e, *r; } ents[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                                                                  {"&#39;", "'"}, {"&nbsp;", " "}};
            bool done = false;
            for (const auto &en : ents) {
                size_t l = strlen(en.e);
                if (h.compare(i, l, en.e) == 0) { o += en.r; i += l - 1; done = true; break; }
            }
            if (done) continue;
        }
        o += c;
    }
    return trim(o);
}

static bool api_error(const HttpResponse &r, const char *who, std::string *err)
{
    if (!r.error.empty()) { *err = std::string(who) + ": " + r.error; return true; }
    if (r.status == 429) {
        *err = std::string(who) + ": too many requests, try again " + (r.retry_after.empty() ? "later" : "in " + r.retry_after + " s");
        return true;
    }
    if (r.status == 401 || r.status == 403) { *err = std::string(who) + ": access denied (API key?) HTTP " + std::to_string(r.status); return true; }
    if (r.status < 200 || r.status >= 300) { *err = std::string(who) + ": HTTP " + std::to_string(r.status); return true; }
    return false;
}

static JsonP get_json(const HttpCache &cache, const HttpRequest &rq, int64_t max_age, const char *who, std::string *err)
{
    HttpResponse r;
    if (!http_get_cached(cache, rq, max_age, &r) && r.error.empty()) r.error = "request failed";
    if (api_error(r, who, err)) return nullptr;
    std::string jerr;
    JsonP j = json_parse(r.body, &jerr);
    if (!j) *err = std::string(who) + ": unexpected answer (" + jerr + ")";
    return j;
}

// ================================================================== mock (offline fixture)

class MockSource : public ModSource {
    std::string dir_;
    JsonP index_;
    std::string load_err_;

    bool offline(std::string *err) const
    {
        const char *e = getenv("SW_MB_MOCK_OFFLINE");
        if (e && *e == '1') { *err = "Mock: no network (offline test)"; return true; }
        if (!index_) { *err = "Mock: " + load_err_; return true; }
        return false;
    }
    static ModSummary summary(const Json &m)
    {
        ModSummary s;
        s.source = "mock";
        s.id = m["id"].str();
        s.name = m["name"].str();
        s.author = m["author"].str();
        s.version = m["version"].str();
        s.category = m["category"].str();
        s.summary = m["summary"].str();
        s.game_version = m["game_version"].str();
        s.downloads = (uint64_t)m["downloads"].num();
        s.likes = (uint64_t)m["likes"].num();
        s.updated = m["updated"].i64();
        s.thumb_url = m["thumb"].str().empty() ? "" : "mock:" + m["thumb"].str();
        s.page_url = "https://example.invalid/mock/" + s.id;
        return s;
    }

public:
    explicit MockSource(const std::string &dir) : dir_(dir)
    {
        std::string text;
        if (!read_text(join(dir, "index.json"), &text)) load_err_ = "fixture index.json missing in " + dir;
        else if (!(index_ = json_parse(text, &load_err_))) load_err_ = "fixture index.json: " + load_err_;
    }
    const char *id() const override { return "mock"; }
    const char *title() const override { return "Mock (offline)"; }
    bool verified() const override { return true; }

    bool search(const SearchQuery &q, SearchResult *out, std::string *err) override
    {
        if (offline(err)) return false;
        std::vector<const Json *> hits;
        for (const auto &m : (*index_)["mods"].a)
            if ((icontains((*m)["name"].str(), q.text) || icontains((*m)["summary"].str(), q.text) ||
                 icontains((*m)["author"].str(), q.text)) &&
                (q.category.empty() || lower((*m)["category"].str()) == lower(q.category)))
                hits.push_back(m.get());
        auto key = [&](const Json *m) -> double {
            switch (q.sort) {
            case SORT_POPULAR: return (*m)["downloads"].num();
            case SORT_NEWEST: return (*m)["added"].num();
            case SORT_UPDATED: return (*m)["updated"].num();
            default: return (*m)["likes"].num() * 10 + (*m)["downloads"].num() / 100;
            }
        };
        std::stable_sort(hits.begin(), hits.end(), [&](const Json *a, const Json *b) { return key(a) > key(b); });
        out->mods.clear();
        out->total = (int)hits.size();
        size_t from = (size_t)std::max(0, q.page - 1) * (size_t)q.per_page;
        for (size_t i = from; i < hits.size() && i < from + (size_t)q.per_page; i++) out->mods.push_back(summary(*hits[i]));
        out->more = from + (size_t)q.per_page < hits.size();
        return true;
    }

    bool details(const std::string &mod_id, ModDetails *out, std::string *err) override
    {
        if (offline(err)) return false;
        for (const auto &m : (*index_)["mods"].a) {
            if ((*m)["id"].str() != mod_id) continue;
            ModSummary s = summary(*m);
            static_cast<ModSummary &>(*out) = s;
            out->description = (*m)["description"].str();
            out->changelog = (*m)["changelog"].str();
            out->images.clear();
            for (const auto &im : (*m)["images"].a) out->images.push_back("mock:" + im->str());
            out->files.clear();
            for (const auto &f : (*m)["files"].a) {
                ModFile mf;
                mf.id = (*f)["id"].str();
                mf.name = (*f)["name"].str();
                mf.url = "mock:" + (*f)["file"].str();
                mf.version = (*f)["version"].str();
                mf.md5 = (*f)["md5"].str();
                mf.game_version = (*f)["game_version"].str();
                mf.description = (*f)["description"].str();
                mf.size = file_size(join(dir_, (*f)["file"].str()));
                out->files.push_back(mf);
            }
            return true;
        }
        *err = "Mock: no mod " + mod_id;
        return false;
    }

    bool download(const ModFile &f, const std::string &dest, const Progress &p, std::string *err, std::string *browser_url) override
    {
        (void)browser_url;
        if (offline(err)) return false;
        if (f.url.compare(0, 5, "mock:") != 0) { *err = "Mock: not a mock file"; return false; }
        std::string src = join(dir_, f.url.substr(5)), data;
        if (!read_text(src, &data, 1ull << 31)) { *err = "Mock: file missing: " + src; return false; }
        // in pieces, so progress and cancel behave like a real download
        HANDLE h = CreateFileW(wide(dest).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { *err = "cannot write " + dest; return false; }
        size_t done = 0, step = std::max<size_t>(data.size() / 8, 4096);
        bool ok = true;
        while (done < data.size()) {
            DWORD n = (DWORD)std::min(step, data.size() - done), put = 0;
            if (!WriteFile(h, data.data() + done, n, &put, NULL)) { ok = false; *err = "write failed"; break; }
            done += n;
            if (p && !p(done, data.size())) { ok = false; *err = "cancelled"; break; }
        }
        CloseHandle(h);
        return ok;
    }

    bool image(const std::string &url, std::string *bytes, std::string *err) override
    {
        if (offline(err)) return false;
        if (url.compare(0, 5, "mock:") != 0 || !read_text(join(dir_, url.substr(5)), bytes, 16u << 20)) {
            *err = "Mock: image missing";
            return false;
        }
        return true;
    }
};

std::unique_ptr<ModSource> make_mock_source(const std::string &dir) { return std::unique_ptr<ModSource>(new MockSource(dir)); }

// ================================================================== shared HTTP helpers for the web sources

static bool web_image(const HttpCache &cache, const std::string &url, std::string *bytes, std::string *err)
{
    HttpRequest rq;
    rq.url = url;
    rq.max_bytes = 16u << 20;
    rq.min_interval_s = 0.1;
    HttpResponse r;
    http_get_cached(cache, rq, 7 * 86400, &r);
    if (api_error(r, "image", err)) return false;
    bytes->swap(r.body);
    return true;
}

static bool web_download(const ModFile &f, const std::string &dest, const Progress &p, const BrowserSettings &s,
                         std::vector<std::pair<std::string, std::string>> headers, std::string *err)
{
    HttpRequest rq;
    rq.url = f.url;
    rq.headers = std::move(headers);
    rq.max_bytes = s.max_download;
    rq.timeout_ms = 60000;
    rq.min_interval_s = 0.2;
    return http_download(rq, dest, p, err);
}

// ================================================================== mod.io (docs.mod.io REST API)

class ModioSource : public ModSource {
    BrowserSettings s_;
    HttpCache cache_;
    std::string base() const { return "https://g-" + s_.modio_game_id + ".modapi.io/v1/games/" + s_.modio_game_id; }
    static ModSummary summary(const Json &m)
    {
        ModSummary s;
        s.source = "modio";
        s.id = m["id"].str();
        s.name = m["name"].str();
        s.author = m["submitted_by"]["username"].str();
        s.summary = m["summary"].str();
        s.version = m["modfile"]["version"].str();
        s.thumb_url = m["logo"]["thumb_320x180"].str();
        s.page_url = m["profile_url"].str();
        s.downloads = (uint64_t)m["stats"]["downloads_total"].num();
        s.likes = (uint64_t)m["stats"]["ratings_positive"].num();   // mod.io has ratings, not likes
        s.updated = m["date_updated"].i64();
        return s;
    }

public:
    ModioSource(const BrowserSettings &s, const std::string &cache) : s_(s) { cache_.dir = join(cache, "modio"); }
    const char *id() const override { return "modio"; }
    const char *title() const override { return "mod.io"; }
    bool verified() const override { return false; }           // docs yes, live no (see the top of this file)
    bool available(std::string *why) const override
    {
        if (!s_.modio) { *why = "mod.io is off (settings)"; return false; }
        if (s_.modio_key.empty() || s_.modio_game_id.empty()) { *why = "mod.io needs your API key and the game id (settings)"; return false; }
        return true;
    }
    bool list(const std::string &query, int64_t max_age, std::vector<JsonP> *mods, int *total, std::string *err)
    {
        HttpRequest rq;
        rq.url = base() + "/mods?api_key=" + url_enc(s_.modio_key) + query;
        JsonP j = get_json(cache_, rq, max_age, "mod.io", err);
        if (!j) return false;
        *mods = (*j)["data"].a;
        *total = (int)(*j)["result_total"].num(-1);
        return true;
    }
    bool search(const SearchQuery &q, SearchResult *out, std::string *err) override
    {
        if (!available(err)) return false;
        static const char *sorts[] = {"-downloads_today", "-downloads_total", "-date_live", "-date_updated"};
        std::string qs = "&_sort=" + std::string(sorts[q.sort & 3]) + "&_offset=" + std::to_string((q.page - 1) * q.per_page) +
                         "&_limit=" + std::to_string(q.per_page);
        if (!q.text.empty()) qs += "&_q=" + url_enc(q.text);
        std::vector<JsonP> mods;
        int total = -1;
        if (!list(qs, 600, &mods, &total, err)) return false;
        out->mods.clear();
        for (const auto &m : mods) out->mods.push_back(summary(*m));
        out->total = total;
        out->more = total > q.page * q.per_page;
        return true;
    }
    bool details(const std::string &mod_id, ModDetails *out, std::string *err) override
    {
        if (!available(err)) return false;
        std::vector<JsonP> mods;
        int total;
        if (!list("&id=" + url_enc(mod_id), 300, &mods, &total, err)) return false;   // documented field filter
        if (mods.empty()) { *err = "mod.io: mod not found"; return false; }
        const Json &m = *mods[0];
        static_cast<ModSummary &>(*out) = summary(m);
        out->description = strip_html(m["description"].str());
        out->changelog = m["modfile"]["changelog"].str();
        out->images.clear();
        if (!m["logo"]["thumb_640x360"].str().empty()) out->images.push_back(m["logo"]["thumb_640x360"].str());
        for (const auto &im : m["media"]["images"].a) if (!(*im)["thumb_320x180"].str().empty()) out->images.push_back((*im)["thumb_320x180"].str());
        out->files.clear();
        const Json &mf = m["modfile"];
        if (mf.t == Json::Obj) {
            ModFile f;
            f.id = mf["id"].str();
            f.name = mf["filename"].str();
            f.version = mf["version"].str();
            f.size = (uint64_t)mf["filesize"].num();
            f.md5 = lower(mf["filehash"]["md5"].str());
            f.url = mf["download"]["binary_url"].str();          // expires: fetched fresh for every download
            f.date = mf["date_added"].i64();
            out->files.push_back(f);
        }
        return true;
    }
    bool download(const ModFile &f, const std::string &dest, const Progress &p, std::string *err, std::string *browser_url) override
    {
        (void)browser_url;
        return web_download(f, dest, p, s_, {}, err);
    }
    bool image(const std::string &url, std::string *bytes, std::string *err) override { return web_image(cache_, url, bytes, err); }
};

std::unique_ptr<ModSource> make_modio_source(const BrowserSettings &s, const std::string &cache)
{
    return std::unique_ptr<ModSource>(new ModioSource(s, cache));
}

// ================================================================== GitHub (docs.github.com REST API)
// Checked live 2026-11-25.  Mod repositories hold their mods as files and folders, rarely as releases, e.g.
// "Mods/01001F5010DFA000/1.1.1/60FPS.zip" or "<game>/<mod>/exefs/<build id>.pchtxt".  This game's mods are:
//   - archives (.zip / .7z / .rar) whose path names the game (title id or "arceus"), installed as they are;
//   - folder mods: the outermost folder (naming the game) that holds romfs/ exefs/ cheats/; its files are fetched
//     one by one from raw.githubusercontent.com, packed into a zip (Windows' tar.exe) and installed like an archive.
// The built-in repositories come from launcher/assets/github_index.json (Engine/Tools/github_index.py, built with
// git before a release): listing them needs no request at all.  Repositories the player adds are listed live with
// one git/trees request each, all at once (threads), cached for 6 hours (60 requests an hour without a token);
// releases are asked for only when the file list has nothing for this game.  Settings may hold "owner/repo" or a
// pasted https://github.com/owner/repo[.git] link.

static const char *kGameTid = "01001f5010dfa000";

static std::string gh_repo(std::string r)
{
    r = trim(r);
    for (const char *p : {"https://", "http://", "www.", "github.com/"})
        if (lower(r).compare(0, strlen(p), p) == 0) r = r.substr(strlen(p));
    while (!r.empty() && r.back() == '/') r.pop_back();
    if (r.size() > 4 && lower(r.substr(r.size() - 4)) == ".git") r.resize(r.size() - 4);
    size_t a = r.find('/');
    if (a == std::string::npos || a == 0) return "";
    size_t b = r.find('/', a + 1);
    std::string out = b == std::string::npos ? r : r.substr(0, b);
    return out.size() > a + 1 ? out : "";
}

static std::string gh_path_enc(const std::string &path)   // each segment encoded, the slashes kept
{
    std::string o, seg;
    for (char c : path) {
        if (c == '/') { o += url_enc(seg) + "/"; seg.clear(); }
        else seg += c;
    }
    return o + url_enc(seg);
}

static bool gh_is_archive(const std::string &path)
{
    std::string e = lower(file_ext(path));
    return e == ".zip" || e == ".7z" || e == ".rar";
}

static bool gh_is_game(const std::string &text)
{
    std::string l = lower(text);
    // another game's title id in the path ("<other id>/[ Arceus]/cheats"): that game's, not ours
    for (size_t i = 0; i + 16 <= l.size(); i++) {
        if (l[i] != '0' || l[i + 1] != '1' || l.compare(i + 13, 3, "000") != 0) continue;
        if (i && isxdigit((unsigned char)l[i - 1])) continue;
        if (i + 16 < l.size() && isxdigit((unsigned char)l[i + 16])) continue;
        bool hex = true;
        for (size_t k = i; k < i + 16; k++) hex = hex && isxdigit((unsigned char)l[k]);
        if (hex && l.compare(i, 16, kGameTid) != 0) return false;
    }
    return l.find(kGameTid) != std::string::npos || l.find("arceus") != std::string::npos;
}

static bool gh_safe_rel(const std::string &rel)            // a path from a file list: never leaves the folder
{
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos) return false;
    std::string seg;
    for (size_t i = 0; i <= rel.size(); i++) {
        if (i == rel.size() || rel[i] == '/' || rel[i] == '\\') {
            if (seg == ".." || seg == ".") return false;
            seg.clear();
        } else seg += rel[i];
    }
    return true;
}

static std::string gh_version_in(const std::string &path)   // the first x.y.z in the path ("1.1.1/60FPS.zip")
{
    for (size_t i = 0; i < path.size(); i++) {
        if (!isdigit((unsigned char)path[i]) || (i && (isalnum((unsigned char)path[i - 1]) || path[i - 1] == '.'))) continue;
        size_t j = i, dots = 0;
        while (j < path.size() && (isdigit((unsigned char)path[j]) || (path[j] == '.' && j + 1 < path.size() &&
                                                                        isdigit((unsigned char)path[j + 1])))) {
            if (path[j] == '.') dots++;
            j++;
        }
        if (dots == 2 && (j == path.size() || !isalnum((unsigned char)path[j]))) return path.substr(i, j - i);
        i = j;
    }
    return "";
}

struct GhMod {
    std::string repo, kind, path, version, sha;             // kind "file" or "folder"; sha: git blob (forks share it)
    uint64_t size = 0;
    std::vector<std::pair<std::string, uint64_t>> files;   // folder: paths relative to it
};

// this game's mods in a repository's file list (the same rules as Engine/Tools/github_index.py)
static std::vector<GhMod> gh_mods_of(const std::string &repo, const std::vector<std::pair<std::string, uint64_t>> &files)
{
    std::vector<GhMod> out;
    for (const auto &f : files)
        if (gh_is_archive(f.first) && gh_is_game(f.first)) {
            GhMod m;
            m.repo = repo, m.kind = "file", m.path = f.first, m.size = f.second, m.version = gh_version_in(f.first);
            out.push_back(m);
        }
    std::vector<std::string> roots;
    for (const auto &f : files) {
        const std::string &p = f.first;
        size_t s = 0, prev = 0;
        while ((s = p.find('/', prev)) != std::string::npos) {
            size_t next = p.find('/', s + 1);
            if (next == std::string::npos) break;           // the last segment is the file itself
            std::string seg = lower(p.substr(s + 1, next - s - 1));
            if (seg == "romfs" || seg == "exefs" || seg == "cheats") {
                std::string root = p.substr(0, s);
                if (gh_is_game(p) && std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
                break;
            }
            prev = s + 1;
        }
    }
    for (const auto &root : roots) {
        bool nested = false;
        for (const auto &o : roots) if (o != root && root.compare(0, o.size() + 1, o + "/") == 0) nested = true;
        if (nested) continue;
        GhMod m;
        m.repo = repo, m.kind = "folder", m.path = root, m.version = gh_version_in(root);
        for (const auto &f : files)
            if (f.first.compare(0, root.size() + 1, root + "/") == 0) {
                m.files.push_back({f.first.substr(root.size() + 1), f.second});
                m.size += f.second;
            }
        if (!m.files.empty() && m.files.size() <= 400 && m.size <= (200ull << 20)) out.push_back(m);
    }
    return out;
}

static std::string gh_exe_dir()
{
    wchar_t b[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, b, MAX_PATH);
    std::string p = n && n < MAX_PATH ? utf8(std::wstring(b, n)) : std::string(".");
    size_t sl = p.find_last_of("\\/");
    return sl == std::string::npos ? "." : p.substr(0, sl);
}

class GithubSource : public ModSource {
    BrowserSettings s_;
    HttpCache cache_;
    bool index_loaded_ = false;
    std::vector<std::string> indexed_;                      // repos the built-in index covers
    std::vector<GhMod> index_mods_;
    std::vector<std::pair<std::string, std::string>> index_names_;

    std::vector<std::pair<std::string, std::string>> headers() const
    {
        return {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    }
    JsonP get(const std::string &url, int64_t max_age, std::string *err)
    {
        HttpRequest rq;
        rq.url = url;
        rq.headers = headers();
        rq.min_interval_s = 0.0;                            // a handful of parallel requests; the cache keeps it rare
        return get_json(cache_, rq, max_age, "GitHub", err);
    }
    void load_index()
    {
        if (index_loaded_) return;
        index_loaded_ = true;
        std::string path = !s_.github_index.empty() ? s_.github_index
                                                    : join(gh_exe_dir(), "../../launcher/assets/github_index.json");
        std::string text, jerr;
        if (!read_text(path, &text)) return;
        JsonP j = json_parse(text, &jerr);
        if (!j) return;
        for (const auto &r : (*j)["repos"].a)
            if ((*r)["ok"].truthy()) {
                indexed_.push_back(lower((*r)["repo"].str()));
                index_names_.push_back({lower((*r)["repo"].str()), (*r)["name"].str()});
            }
        for (const auto &m : (*j)["mods"].a) {
            GhMod g;
            g.repo = (*m)["repo"].str(), g.kind = (*m)["kind"].str(), g.path = (*m)["path"].str();
            g.version = (*m)["version"].str(), g.size = (uint64_t)(*m)["size"].num(), g.sha = (*m)["sha"].str();
            for (const auto &f : (*m)["files"].a) g.files.push_back({(*f)[(size_t)0].str(), (uint64_t)(*f)[(size_t)1].num()});
            if (!g.repo.empty() && !g.path.empty()) index_mods_.push_back(g);
        }
    }
    bool is_indexed(const std::string &repo) const
    {
        return std::find(indexed_.begin(), indexed_.end(), lower(repo)) != indexed_.end();
    }
    std::string name_of(const std::string &repo) const
    {
        for (const auto &kv : s_.github_names) if (lower(gh_repo(kv.first)) == lower(repo) && !trim(kv.second).empty()) return kv.second;
        for (const auto &kv : index_names_) if (kv.first == lower(repo) && !kv.second.empty()) return kv.second;
        return repo;
    }
    std::vector<std::string> repos() const
    {
        std::vector<std::string> v;
        if (s_.github_repos.empty() && s_.github_removed.empty()) {   // never set up: every built-in repository
            for (const auto &kv : index_names_) v.push_back(kv.first);
            return v;
        }
        for (const auto &r : s_.github_repos) {
            std::string n = gh_repo(r);
            if (n.empty()) continue;
            bool dup = false;
            for (const auto &x : v) if (lower(x) == lower(n)) dup = true;
            if (!dup) v.push_back(n);
        }
        for (const auto &kv : index_names_) {                // built-ins the player has not deleted (newer index)
            bool have = false, gone = false;
            for (const auto &x : v) have = have || lower(x) == kv.first;
            for (const auto &x : s_.github_removed) gone = gone || lower(gh_repo(x)) == kv.first;
            if (!have && !gone) v.push_back(kv.first);
        }
        return v;
    }
    // a repository's mods: from the index, else its live file list (+ its newest release if the list has none)
    bool live_mods(const std::string &repo, std::vector<GhMod> *mods, JsonP *release, std::string *err)
    {
        JsonP t = get("https://api.github.com/repos/" + repo + "/git/trees/HEAD?recursive=1", 6 * 3600, err);
        if (t) {
            std::vector<std::pair<std::string, uint64_t>> files;
            std::map<std::string, std::string> shas;
            for (const auto &it : (*t)["tree"].a)
                if ((*it)["type"].str() == "blob") {
                    files.push_back({(*it)["path"].str(), (uint64_t)(*it)["size"].num()});
                    shas[(*it)["path"].str()] = (*it)["sha"].str();
                }
            *mods = gh_mods_of(repo, files);
            for (auto &m : *mods) if (m.kind == "file") m.sha = shas[m.path];
        }
        if (mods->empty()) {
            std::string e2;
            JsonP r = get("https://api.github.com/repos/" + repo + "/releases?per_page=10", 6 * 3600, &e2);
            if (r) *release = r;
            else if (!t && err->empty()) *err = e2;
            return t || r;
        }
        return true;
    }
    ModSummary mod_summary(const GhMod &m) const
    {
        ModSummary s;
        s.source = "github";
        s.id = m.repo + "|" + m.path + (m.kind == "folder" ? "/" : "");
        size_t sl = m.path.rfind('/');
        std::string leaf = sl == std::string::npos ? m.path : m.path.substr(sl + 1);
        std::string stem = m.kind == "file" ? leaf.substr(0, leaf.size() - file_ext(leaf).size()) : leaf;
        if (m.kind == "folder" && (lower(stem) == lower(kGameTid) || gh_version_in(stem) == stem)) {
            // ".../<mod name>/01001F5010DFA000": the mod's name is the folder above, unless that is only a layout
            // folder (titles/, contents/, load/ ...): then what the folder holds names it
            std::string parent = sl == std::string::npos ? "" : m.path.substr(0, sl);
            size_t s2 = parent.rfind('/');
            std::string up = s2 == std::string::npos ? parent : parent.substr(s2 + 1);
            std::string lu = lower(up);
            bool layout = lu.empty() || lu == "titles" || lu == "contents" || lu == "load" || lu == "mods" ||
                          lu == "atmosphere" || lu == "cheats" || lu == lower(kGameTid);
            if (!layout) stem = up;
            else {
                bool ch = false, ex = false, ro = false;
                for (const auto &f : m.files) {
                    std::string first = lower(f.first.substr(0, f.first.find('/')));
                    ch |= first == "cheats", ex |= first == "exefs", ro |= first == "romfs";
                }
                stem = ro ? (ex ? "Game files + code patches" : "Game files mod") : ex ? (ch ? "Code patches + cheats" : "Code patches")
                          : "Cheats";
                stem += " (" + m.repo.substr(0, m.repo.find('/')) + ")";   // several repos have one: tell them apart
            }
            if (gh_version_in(m.path.substr(sl == std::string::npos ? 0 : sl + 1)) == m.path.substr(sl == std::string::npos ? 0 : sl + 1))
                stem += " " + m.path.substr(sl + 1);                // ".../1.0.0": keep the version visible
        }
        s.game_version = m.version;
        s.name = stem + (m.version.empty() || stem.find(m.version) != std::string::npos ? "" : " (game " + m.version + ")");
        s.author = name_of(m.repo);
        s.summary = m.repo + ": " + m.path + (m.kind == "folder" ? "/  (" + std::to_string(m.files.size()) + " files" : "") +
                    (m.size ? std::string(m.kind == "folder" ? ", " : "  (") + std::to_string((m.size + 1023) / 1024) + " KB)"
                            : std::string(m.kind == "folder" ? ")" : ""));
        s.page_url = "https://github.com/" + m.repo + (m.kind == "folder" ? "/tree/HEAD/" : "/blob/HEAD/") + gh_path_enc(m.path);
        s.category = m.kind == "folder" ? "Folders" : "Files";
        return s;
    }
    ModSummary release_summary(const std::string &repo, const Json &rel) const
    {
        ModSummary s;
        s.source = "github";
        s.id = repo;
        s.name = repo.substr(repo.find('/') + 1);
        s.author = name_of(repo);
        s.version = rel["tag_name"].str();
        s.summary = rel["name"].str();
        s.page_url = rel["html_url"].str().empty() ? "https://github.com/" + repo : rel["html_url"].str();
        uint64_t dl = 0;
        for (const auto &a : rel["assets"].a) dl += (uint64_t)(*a)["download_count"].num();
        s.downloads = dl;
        s.category = "Releases";
        return s;
    }
    static bool release_is_game(const Json &rel)
    {
        if (gh_is_game(rel["name"].str() + " " + rel["tag_name"].str() + " " + rel["body"].str())) return true;
        for (const auto &a : rel["assets"].a) if (gh_is_game((*a)["name"].str())) return true;
        return false;
    }
    bool find_mod(const std::string &repo, const std::string &path, GhMod *out, std::string *err)
    {
        load_index();
        if (is_indexed(repo))
            for (const auto &m : index_mods_)
                if (lower(m.repo) == lower(repo) && m.path == path) { *out = m; return true; }
        std::vector<GhMod> mods;
        JsonP rel;
        if (!live_mods(repo, &mods, &rel, err)) return false;
        for (const auto &m : mods) if (m.path == path) { *out = m; return true; }
        *err = "GitHub: " + path + " is no longer in " + repo;
        return false;
    }

public:
    GithubSource(const BrowserSettings &s, const std::string &cache) : s_(s)
    {
        cache_.dir = join(cache, "github");
        load_index();
    }
    const char *id() const override { return "github"; }
    const char *title() const override { return "GitHub"; }
    bool verified() const override { return true; }   // checked live 2026-11-25 (index, file lists, releases)
    bool available(std::string *why) const override
    {
        if (!s_.github) { *why = "GitHub is off (settings)"; return false; }
        if (repos().empty()) { *why = "no GitHub repositories (settings > Repos)"; return false; }
        return true;
    }
    bool search(const SearchQuery &q, SearchResult *out, std::string *err) override
    {
        if (!available(err)) return false;
        load_index();
        out->mods.clear();
        std::vector<GhMod> all;
        std::vector<std::pair<std::string, JsonP>> releases;
        std::vector<std::string> live;
        for (const auto &repo : repos()) {
            if (is_indexed(repo)) {
                for (const auto &m : index_mods_) if (lower(m.repo) == lower(repo)) all.push_back(m);
            } else live.push_back(repo);
        }
        // the added repositories: all at once
        struct Job { std::string repo; std::vector<GhMod> mods; JsonP rel; std::string err; bool ok = false; };
        std::vector<Job> jobs(live.size());
        std::vector<std::thread> th;
        for (size_t i = 0; i < live.size(); i++) {
            jobs[i].repo = live[i];
            th.emplace_back([this, &jobs, i]() { jobs[i].ok = live_mods(jobs[i].repo, &jobs[i].mods, &jobs[i].rel, &jobs[i].err); });
        }
        for (auto &t : th) t.join();
        std::string first_err;
        for (auto &j : jobs) {
            all.insert(all.end(), j.mods.begin(), j.mods.end());
            if (j.rel) releases.push_back({j.repo, j.rel});
            if (!j.ok && first_err.empty()) first_err = j.err;
        }
        std::vector<std::string> seen;   // forks carry the same file: one entry per name + size
        for (const auto &m : all) {
            std::string leaf = m.path.substr(m.path.rfind('/') == std::string::npos ? 0 : m.path.rfind('/') + 1);
            std::string key = !m.sha.empty() ? "sha:" + m.sha : m.kind + "|" + lower(leaf) + "|" + std::to_string(m.size);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            ModSummary s = mod_summary(m);
            if (icontains(s.name + " " + s.summary + " " + s.author, q.text)) out->mods.push_back(s);
        }
        for (const auto &r : releases)
            for (const auto &rel : r.second->a) {
                if ((*rel)["draft"].truthy()) continue;
                if (release_is_game(*rel)) {
                    ModSummary s = release_summary(r.first, *rel);
                    if (icontains(s.name + " " + s.author + " " + s.summary, q.text)) out->mods.push_back(s);
                }
                break;                                      // the newest published release per repo
            }
        if (out->mods.empty() && !first_err.empty() && all.empty()) { *err = first_err; return false; }
        out->total = (int)out->mods.size();
        out->more = false;
        return true;
    }
    bool details(const std::string &id, ModDetails *out, std::string *err) override
    {
        size_t bar = id.find('|');
        if (bar != std::string::npos) {                       // a file or folder in the repository
            std::string repo = gh_repo(id.substr(0, bar)), path = id.substr(bar + 1);
            bool folder = !path.empty() && path.back() == '/';
            if (folder) path.pop_back();
            GhMod m;
            if (!find_mod(repo, path, &m, err)) return false;
            static_cast<ModSummary &>(*out) = mod_summary(m);
            out->description = std::string(m.kind == "folder" ? "A mod folder" : "A file") + " in the GitHub repository " +
                               repo + ":\n" + path + (m.kind == "folder" ? "/" : "") +
                               "\n\nCollections hold mods from many authors: the credits are in the repository.";
            if (m.kind == "folder") {
                out->description += "\n\nFiles:";
                for (size_t i = 0; i < m.files.size() && i < 40; i++) out->description += "\n  " + m.files[i].first;
                if (m.files.size() > 40) out->description += "\n  ... " + std::to_string(m.files.size() - 40) + " more";
            }
            out->files.clear();
            ModFile f;
            size_t sl = path.rfind('/');
            std::string leaf = sl == std::string::npos ? path : path.substr(sl + 1);
            f.id = path;
            f.size = m.size;
            f.game_version = m.version;
            if (m.kind == "folder") {
                f.name = safe_folder_name(out->name) + ".zip";      // packed after the download (download())
                f.url = "ghfolder:" + repo + "|" + path;
            } else {
                f.name = leaf;
                f.url = "https://raw.githubusercontent.com/" + repo + "/HEAD/" + gh_path_enc(path);
            }
            out->files.push_back(f);
            return true;
        }
        std::string repo = gh_repo(id);
        JsonP j = get("https://api.github.com/repos/" + repo + "/releases?per_page=10", 6 * 3600, err);
        if (!j) return false;
        for (const auto &rel : j->a) {
            if ((*rel)["draft"].truthy()) continue;
            static_cast<ModSummary &>(*out) = release_summary(repo, *rel);
            out->description = (*rel)["body"].str();
            out->changelog.clear();
            for (const auto &r2 : j->a) out->changelog += (*r2)["tag_name"].str() + ": " + (*r2)["name"].str() + "\n";
            out->files.clear();
            for (const auto &a : (*rel)["assets"].a) {
                ModFile f;
                f.id = (*a)["id"].str();
                f.name = (*a)["name"].str();
                f.size = (uint64_t)(*a)["size"].num();
                f.url = (*a)["browser_download_url"].str();
                f.version = out->version;
                std::string dg = (*a)["digest"].str();          // "sha256:<hex>" when present
                if (lower(dg).compare(0, 7, "sha256:") == 0) f.sha256 = lower(dg.substr(7));
                out->files.push_back(f);
            }
            return true;
        }
        *err = "GitHub: no published release";
        return false;
    }
    bool download(const ModFile &f, const std::string &dest, const Progress &p, std::string *err, std::string *browser_url) override
    {
        (void)browser_url;
        if (f.url.compare(0, 9, "ghfolder:") != 0) return web_download(f, dest, p, s_, {}, err);
        // a folder mod: every file from raw.githubusercontent.com into a folder, then one zip of it (tar.exe)
        std::string spec = f.url.substr(9);
        size_t bar = spec.find('|');
        if (bar == std::string::npos) { *err = "GitHub: bad folder link"; return false; }
        std::string repo = spec.substr(0, bar), path = spec.substr(bar + 1);
        GhMod m;
        if (!find_mod(repo, path, &m, err)) return false;
        std::string dir = dest + ".d";
        remove_tree(dir);
        if (!make_dirs(dir)) { *err = "cannot create " + dir; return false; }
        uint64_t done = 0;
        for (const auto &file : m.files) {
            if (!gh_safe_rel(file.first)) { remove_tree(dir); *err = "GitHub: unsafe path in the folder: " + file.first; return false; }
            std::string out_path = join(dir, file.first);
            size_t sl = out_path.find_last_of("\\/");
            if (sl != std::string::npos) make_dirs(out_path.substr(0, sl));
            ModFile one;
            one.name = base_name(file.first);
            one.size = file.second;
            one.url = "https://raw.githubusercontent.com/" + repo + "/HEAD/" + gh_path_enc(path + "/" + file.first);
            uint64_t base = done;
            Progress sub = [&](uint64_t d, uint64_t) { return !p || p(base + d, m.size); };
            if (!web_download(one, out_path, sub, s_, {}, err)) { remove_tree(dir); return false; }
            done += file.second;
        }
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        std::string tar = n && n < MAX_PATH ? utf8(sys) + "\\tar.exe" : std::string("C:\\Windows\\System32\\tar.exe");
        std::string out;
        DeleteFileW(wide(dest).c_str());
        int rc = run_process(tar, {"-a", "-c", "-f", dest, "-C", dir, "."}, &out, 1u << 20, 10 * 60 * 1000, nullptr);
        remove_tree(dir);
        if (rc != 0 || !is_file(dest)) { *err = "GitHub: packing the folder failed: " + trim(out).substr(0, 200); return false; }
        return true;
    }
    bool image(const std::string &url, std::string *bytes, std::string *err) override { return web_image(cache_, url, bytes, err); }
};

std::unique_ptr<ModSource> make_github_source(const BrowserSettings &s, const std::string &cache)
{
    return std::unique_ptr<ModSource>(new GithubSource(s, cache));
}

// ================================================================== GameBanana (UNVERIFIED, see the top)

class GamebananaSource : public ModSource {
    BrowserSettings s_;
    HttpCache cache_;
    static std::string image_url(const Json &media, bool big)
    {
        const Json &im = media["_aImages"][(size_t)0];
        if (im.t != Json::Obj) return "";
        // the full picture (decoded down to the card size by the browser); _sFile220 is too small for the cards
        std::string f = im["_sFile"].str();
        if (f.empty()) f = im[big ? "_sFile" : "_sFile220"].str();
        return f.empty() ? "" : im["_sBaseUrl"].str() + "/" + f;
    }
    static ModSummary summary(const Json &m)
    {
        ModSummary s;
        s.source = "gamebanana";
        s.id = m["_idRow"].str();
        s.name = m["_sName"].str();
        s.author = m["_aSubmitter"]["_sName"].str();
        s.version = m["_sVersion"].str();
        s.category = m["_aRootCategory"]["_sName"].str();
        s.thumb_url = image_url(m["_aPreviewMedia"], false);
        s.page_url = m["_sProfileUrl"].str().empty() ? "https://gamebanana.com/mods/" + s.id : m["_sProfileUrl"].str();
        s.likes = (uint64_t)m["_nLikeCount"].num();
        s.downloads = (uint64_t)m["_nDownloadCount"].num();
        s.updated = m["_tsDateUpdated"].i64(m["_tsDateModified"].i64());
        return s;
    }

public:
    GamebananaSource(const BrowserSettings &s, const std::string &cache) : s_(s) { cache_.dir = join(cache, "gamebanana"); }
    const char *id() const override { return "gamebanana"; }
    const char *title() const override { return "GameBanana"; }
    bool verified() const override { return false; }
    bool available(std::string *why) const override
    {
        if (!s_.gamebanana) { *why = "GameBanana is off (settings)"; return false; }
        if (s_.gamebanana_game_id.empty()) {
            *why = "GameBanana: set the game's id in the settings (the number in its address, gamebanana.com/games/<id>)";
            return false;
        }
        return true;
    }
    bool search(const SearchQuery &q, SearchResult *out, std::string *err) override
    {
        if (!available(err)) return false;
        HttpRequest rq;
        if (!q.text.empty())
            rq.url = "https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=" +
                     url_enc(s_.gamebanana_game_id) + "&_sSearchString=" + url_enc(q.text) + "&_nPage=" + std::to_string(q.page);
        else {
            static const char *sorts[] = {"Generic_MostLiked", "Generic_MostDownloaded", "Generic_Newest", "Generic_LatestUpdated"};
            rq.url = "https://gamebanana.com/apiv11/Mod/Index?_nPage=" + std::to_string(q.page) + "&_nPerpage=" +
                     std::to_string(q.per_page) + "&_aFilters%5BGeneric_Game%5D=" + url_enc(s_.gamebanana_game_id) +
                     "&_sSort=" + sorts[q.sort & 3];
        }
        rq.min_interval_s = 1.0;
        JsonP j = get_json(cache_, rq, 600, "GameBanana", err);
        if (!j) return false;
        out->mods.clear();
        for (const auto &m : (*j)["_aRecords"].a) {
            if ((*m)["_sModelName"].str("Mod") != "Mod") continue;
            out->mods.push_back(summary(*m));
        }
        out->total = (int)(*j)["_aMetadata"]["_nRecordCount"].num(-1);
        out->more = !(*j)["_aMetadata"]["_bIsComplete"].truthy() && !out->mods.empty();
        return true;
    }
    bool details(const std::string &mod_id, ModDetails *out, std::string *err) override
    {
        if (!available(err)) return false;
        HttpRequest rq;
        rq.url = "https://gamebanana.com/apiv11/Mod/" + url_enc(mod_id) + "/ProfilePage";
        rq.min_interval_s = 1.0;
        JsonP j = get_json(cache_, rq, 300, "GameBanana", err);
        if (!j) return false;
        const Json &m = *j;
        // a mod for another game: never offered (the game filter of the listing could be wrong)
        std::string gid = m["_aGame"]["_idRow"].str();
        if (!gid.empty() && gid != s_.gamebanana_game_id) { *err = "GameBanana: this mod is for another game"; return false; }
        static_cast<ModSummary &>(*out) = summary(m);
        out->description = strip_html(m["_sText"].str());
        out->images.clear();
        for (const auto &im : m["_aPreviewMedia"]["_aImages"].a)
            if (!(*im)["_sFile"].str().empty()) out->images.push_back((*im)["_sBaseUrl"].str() + "/" + (*im)["_sFile"].str());
        out->files.clear();
        for (const auto &f : m["_aFiles"].a) {
            ModFile mf;
            mf.id = (*f)["_idRow"].str();
            mf.name = (*f)["_sFile"].str();
            mf.size = (uint64_t)(*f)["_nFilesize"].num();
            mf.url = (*f)["_sDownloadUrl"].str();
            mf.md5 = lower((*f)["_sMd5Checksum"].str());
            mf.version = (*f)["_sVersion"].str();
            mf.description = (*f)["_sDescription"].str();
            mf.date = (*f)["_tsDateAdded"].i64();
            out->files.push_back(mf);
        }
        return true;
    }
    bool download(const ModFile &f, const std::string &dest, const Progress &p, std::string *err, std::string *browser_url) override
    {
        (void)browser_url;
        return web_download(f, dest, p, s_, {}, err);
    }
    bool image(const std::string &url, std::string *bytes, std::string *err) override { return web_image(cache_, url, bytes, err); }
};

std::unique_ptr<ModSource> make_gamebanana_source(const BrowserSettings &s, const std::string &cache)
{
    return std::unique_ptr<ModSource>(new GamebananaSource(s, cache));
}

// ================================================================== Nexus Mods (UNVERIFIED, see the top)

class NexusSource : public ModSource {
    BrowserSettings s_;
    HttpCache cache_;
    std::vector<std::pair<std::string, std::string>> headers() const
    {
        return {{"apikey", s_.nexus_key}, {"Application-Name", "LodeStar"}, {"Application-Version", "1.0.0"},
                {"Accept", "application/json"}};
    }
    JsonP get(const std::string &path, int64_t max_age, std::string *err)
    {
        HttpRequest rq;
        rq.url = "https://api.nexusmods.com/v1/games/" + url_enc(s_.nexus_game_domain) + path;
        rq.headers = headers();
        rq.min_interval_s = 1.0;                             // the documented throttle refills one request per second
        return get_json(cache_, rq, max_age, "Nexus Mods", err);
    }
    static ModSummary summary(const Json &m, const std::string &domain)
    {
        ModSummary s;
        s.source = "nexus";
        s.id = m["mod_id"].str();
        s.name = m["name"].str();
        s.author = m["author"].str(m["uploaded_by"].str().c_str());
        s.summary = m["summary"].str();
        s.version = m["version"].str();
        s.thumb_url = m["picture_url"].str();
        s.page_url = "https://www.nexusmods.com/" + domain + "/mods/" + s.id;
        s.likes = (uint64_t)m["endorsement_count"].num();
        s.updated = m["updated_timestamp"].i64();
        return s;
    }

public:
    NexusSource(const BrowserSettings &s, const std::string &cache) : s_(s) { cache_.dir = join(cache, "nexus"); }
    const char *id() const override { return "nexus"; }
    const char *title() const override { return "Nexus Mods"; }
    bool verified() const override { return false; }
    bool available(std::string *why) const override
    {
        if (!s_.nexus) { *why = "Nexus Mods is off (settings)"; return false; }
        if (s_.nexus_key.empty()) {
            *why = "Nexus Mods needs your personal API key: nexusmods.com > your account > Site preferences > API Keys "
                   "(Personal API Key), then paste it in the mod browser settings";
            return false;
        }
        if (s_.nexus_game_domain.empty()) { *why = "Nexus Mods: set the game's domain name in the settings"; return false; }
        return true;
    }
    bool search(const SearchQuery &q, SearchResult *out, std::string *err) override
    {
        if (!available(err)) return false;
        // v1 has lists, not search: trending / latest added / latest updated, filtered here
        const char *list = q.sort == SORT_NEWEST ? "/mods/latest_added.json" : q.sort == SORT_UPDATED ? "/mods/latest_updated.json"
                                                                                                       : "/mods/trending.json";
        JsonP j = get(list, 900, err);
        if (!j) return false;
        out->mods.clear();
        for (const auto &m : j->a)
            if ((*m)["available"].t == Json::Null || (*m)["available"].truthy()) {
                ModSummary s = summary(*m, s_.nexus_game_domain);
                if (icontains(s.name + " " + s.summary + " " + s.author, q.text)) out->mods.push_back(s);
            }
        out->total = (int)out->mods.size();
        out->more = false;
        return true;
    }
    bool details(const std::string &mod_id, ModDetails *out, std::string *err) override
    {
        if (!available(err)) return false;
        JsonP m = get("/mods/" + url_enc(mod_id) + ".json", 600, err);
        if (!m) return false;
        static_cast<ModSummary &>(*out) = summary(*m, s_.nexus_game_domain);
        out->description = strip_html((*m)["description"].str());
        out->images.clear();
        if (!out->thumb_url.empty()) out->images.push_back(out->thumb_url);
        JsonP fl = get("/mods/" + url_enc(mod_id) + "/files.json", 600, err);
        out->files.clear();
        if (fl)
            for (const auto &f : (*fl)["files"].a) {
                ModFile mf;
                mf.id = (*f)["file_id"].str();
                mf.name = (*f)["file_name"].str();
                mf.version = (*f)["version"].str();
                mf.size = (uint64_t)(*f)["size_in_bytes"].num((*f)["size_kb"].num() * 1024);
                mf.md5 = lower((*f)["md5"].str());
                mf.description = (*f)["description"].str();
                mf.date = (*f)["uploaded_timestamp"].i64();
                mf.url = "nexus:" + mod_id + ":" + mf.id;
                out->files.push_back(mf);
            }
        return true;
    }
    // premium: a direct link; free accounts: the API refuses, the mod's files page opens in the browser
    bool download(const ModFile &f, const std::string &dest, const Progress &p, std::string *err, std::string *browser_url) override
    {
        size_t a = f.url.find(':'), b = f.url.find(':', a + 1);
        if (f.url.compare(0, 6, "nexus:") != 0 || b == std::string::npos) return web_download(f, dest, p, s_, {}, err);
        std::string mod_id = f.url.substr(a + 1, b - a - 1), file_id = f.url.substr(b + 1);
        std::string extra;
        if (!f.md5.empty() && f.md5.compare(0, 4, "nxm:") == 0) extra = f.md5.substr(4);   // "?key=..&expires=.." of an nxm link
        HttpRequest rq;
        rq.url = "https://api.nexusmods.com/v1/games/" + url_enc(s_.nexus_game_domain) + "/mods/" + mod_id + "/files/" +
                 file_id + "/download_link.json" + extra;
        rq.headers = headers();
        HttpResponse r;
        http_get(rq, &r);
        if (r.status == 403) {
            *browser_url = "https://www.nexusmods.com/" + s_.nexus_game_domain + "/mods/" + mod_id + "?tab=files&file_id=" + file_id;
            *err = "Nexus Mods: direct downloads need a premium account - the download page opens in your browser; "
                   "then use 'Install from file'";
            return false;
        }
        if (api_error(r, "Nexus Mods", err)) return false;
        JsonP j = json_parse(r.body);
        std::string url = j ? (*j)[(size_t)0]["URI"].str() : "";
        if (url.empty()) { *err = "Nexus Mods: no download link in the answer"; return false; }
        ModFile g = f;
        g.url = url;
        return web_download(g, dest, p, s_, {}, err);
    }
    bool image(const std::string &url, std::string *bytes, std::string *err) override { return web_image(cache_, url, bytes, err); }
};

std::unique_ptr<ModSource> make_nexus_source(const BrowserSettings &s, const std::string &cache)
{
    return std::unique_ptr<ModSource>(new NexusSource(s, cache));
}

}  // namespace mb
