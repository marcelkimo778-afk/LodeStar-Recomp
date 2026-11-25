// mb_source.h - the mod browser's source plugins (MODS.md "Mod browser").
//
// A ModSource is one website (or the offline mock): search, details, file list, download, update check.  Every call
// runs on a browser worker thread (ui/mb_browser.cpp), never on the render thread, and reports errors as text instead
// of failing hard: a source that is down shows its error in its tab while the others keep working.
#pragma once
#include <stdint.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mb_util.h"

namespace mb {

struct ModFile {
    std::string id, name, url, md5, sha256, version, description, game_version;
    uint64_t size = 0;
    int64_t date = 0;
};

struct ModSummary {
    std::string source, id, name, author, version, thumb_url, page_url, category, summary;
    std::string game_version;     // the "game version" field of the source, if it has one ("" = none)
    uint64_t downloads = 0, likes = 0;
    int64_t updated = 0;
};

struct ModDetails : ModSummary {
    std::string description, changelog, license;
    std::vector<std::string> images;
    std::vector<ModFile> files;
};

enum Sort { SORT_TRENDING, SORT_POPULAR, SORT_NEWEST, SORT_UPDATED };

struct SearchQuery {
    std::string text, category;
    int sort = SORT_TRENDING;
    int page = 1;                 // 1-based
    int per_page = 24;
};

struct SearchResult {
    std::vector<ModSummary> mods;
    int total = -1;               // -1: the source does not say
    bool more = false;            // another page exists
};

// what an installed mod remembers (mod_meta.json)
struct InstalledMeta {
    std::string folder;           // absolute path of the mod folder
    std::string source, mod_id, name, author, version, file_id, file_name, url, page_url, installed_at;
    std::string detected_version, compat, compat_why;   // version compatibility (mb_install.cpp)
};

// return false to cancel; total 0 = unknown
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

class ModSource {
public:
    virtual ~ModSource() {}
    virtual const char *id() const = 0;            // "gamebanana", "mock", ...
    virtual const char *title() const = 0;         // tab title
    virtual bool verified() const = 0;             // written against the official docs and checked live
    // usable now (enabled in the config, key present ...); *why says what is missing
    virtual bool available(std::string *why) const { (void)why; return true; }
    virtual bool search(const SearchQuery &q, SearchResult *out, std::string *err) = 0;
    virtual bool details(const std::string &mod_id, ModDetails *out, std::string *err) = 0;
    virtual bool list_files(const std::string &mod_id, std::vector<ModFile> *out, std::string *err)
    {
        ModDetails d;
        if (!details(mod_id, &d, err)) return false;
        *out = d.files;
        return true;
    }
    // download f to dest (a file path); a source that cannot hand out the file (Nexus Mods without premium) returns
    // false with *browser_url set: the UI opens that page in the system browser instead
    virtual bool download(const ModFile &f, const std::string &dest, const Progress &p, std::string *err,
                          std::string *browser_url) = 0;
    // bytes of an image (thumbnail / screenshot) this source named; the browser decodes them on a worker thread
    virtual bool image(const std::string &url, std::string *bytes, std::string *err) = 0;
    // a newer version than the installed one?  *latest = its version
    virtual bool check_update(const InstalledMeta &m, std::string *latest, std::string *err)
    {
        ModDetails d;
        if (!details(m.mod_id, &d, err)) return false;
        *latest = d.version;
        return true;
    }
};

// ---- settings (Config/modbrowser.json): which sources are on, API keys (local only, never logged), GitHub repos
struct BrowserSettings {
    bool gamebanana = true, nexus = false, modio = false, github = true;   // on: the built-in repository index needs no setup
    bool check_updates = false;         // background update check when the browser opens / at start
    bool notice_shown = false;          // the one-time third-party notice
    bool only_compatible = true;        // filter "Only compatible with the target version"
    std::string nexus_key, modio_key, modio_game_id, nexus_game_domain, gamebanana_game_id;
    std::string target_version = "1.0.0";   // the game version this recomp was built from (no control.nacp in the dump)
    std::vector<std::string> github_repos;   // "owner/repo": the ones searched (built-in + added, the launcher's Repos window)
    std::vector<std::pair<std::string, std::string>> github_names;   // repo -> the name the player gave it
    std::vector<std::string> github_removed;  // built-in repos the player deleted (not added back)
    std::string github_index;                 // the built-in index (launcher/assets/github_index.json; "" = beside the launcher)
    uint64_t max_download = 4ull << 30, max_unpacked = 8ull << 30;
    uint32_t max_files = 50000;
};
bool settings_load(const std::string &path, BrowserSettings *s);
bool settings_save(const std::string &path, const BrowserSettings &s);

// ---- the sources (mb_sources.cpp); the mock source reads a fixture folder (offline tests / demo)
std::unique_ptr<ModSource> make_mock_source(const std::string &fixture_dir);
std::unique_ptr<ModSource> make_gamebanana_source(const BrowserSettings &s, const std::string &cache_dir);
std::unique_ptr<ModSource> make_nexus_source(const BrowserSettings &s, const std::string &cache_dir);
std::unique_ptr<ModSource> make_modio_source(const BrowserSettings &s, const std::string &cache_dir);
std::unique_ptr<ModSource> make_github_source(const BrowserSettings &s, const std::string &cache_dir);

}  // namespace mb
