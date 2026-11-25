// mb_browser.h - the mod browser's state and worker threads (no ImGui here: ui/sw_overlay.cpp draws it, the test
// drives it directly).  Every network, archive and image job runs on a worker; the render thread only takes
// snapshots (short lock) and posts requests.
//
// Threads: "api" (search, details, update checks), "img" (thumbnails: download + WIC decode), "job" (download ->
// verify -> install, one at a time).  start() is called the first time a browser tab is opened: nothing is created,
// read or sent before (zero cost while the browser is unused).
#pragma once
#include <stdint.h>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mb_install.h"
#include "mb_source.h"

namespace mb {

struct BrowserPaths {
    std::string settings_file;        // Config/modbrowser.json
    std::string mods_root;            // where mods are installed (the loader's mods folder)
    std::string cache_dir;            // thumbnails, API answers
    std::string mock_dir;             // SW_MB_MOCK: the offline mock source (tests, demo); "" = none
    Target target;
};

enum { ST_IDLE, ST_LOADING, ST_OK, ST_ERROR };

struct TabView {
    std::string id, title;
    bool available = false, verified = false;
    std::string why;                  // why it is not available
    int state = ST_IDLE;
    std::string error;
    SearchQuery query;
    std::vector<ModSummary> mods;
    std::vector<VersionInfo> compat;  // per mod (from the source's fields / text)
    int total = -1;
    bool more = false;
};

struct DetailView {
    int state = ST_IDLE;
    std::string error;
    int tab = -1;
    ModDetails mod;
    VersionInfo compat;
};

enum JobState { JOB_QUEUED, JOB_DOWNLOAD, JOB_VERIFY, JOB_INSTALL, JOB_DONE, JOB_FAILED, JOB_CANCELLED, JOB_BLOCKED, JOB_BROWSER };
const char *job_state_name(int s);

struct JobView {
    int id = 0;
    std::string title, message, browser_url;
    int state = JOB_QUEUED;
    uint64_t done = 0, total = 0;
    std::vector<std::string> refused, warnings, folders;
    VersionInfo compat;
    bool finished() const { return state >= JOB_DONE; }
};

struct InstalledView {
    InstalledMeta meta;
    bool disabled = false;            // .disabled marker
    std::string latest;               // a newer version the source offers ("" = none / not checked)
};

struct Image {
    int state = ST_IDLE;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;        // w * h * 4
};

// decode PNG / JPEG / WebP / GIF / BMP bytes (WIC) to RGBA, at most max_w wide
bool decode_image(const std::string &bytes, int max_w, Image *out, std::string *err);

class Browser {
public:
    Browser();
    ~Browser();
    bool start(const BrowserPaths &p);          // once; later calls only return true
    bool started() const { return started_; }
    void stop();

    // ---- tabs (one per source)
    int tab_count();
    TabView tab(int i);
    void search(int tab, const SearchQuery &q, bool append);   // append: the next page under the current results
    // ---- detail page
    void open_details(int tab, const std::string &mod_id);
    DetailView details();
    // ---- install queue
    int install(int tab, const ModDetails &mod, const ModFile &file, bool allow_wrong_version);
    int install_local(const std::string &path_or_url, bool allow_wrong_version);   // "Install from file or link"
    std::vector<JobView> jobs();
    void cancel(int job_id);
    int install_anyway(int job_id);             // a JOB_BLOCKED job again, with allow_wrong_version
    void clear_finished();
    // ---- installed mods (mod_meta.json under the mods folder)
    std::vector<InstalledView> installed();
    void refresh_installed();
    bool uninstall(const std::string &folder, std::string *msg);
    bool set_enabled(const std::string &folder, bool on);        // .disabled marker (mods not loaded yet)
    void check_updates();                       // in the background; results in installed()[i].latest
    int installed_state(const std::string &source, const std::string &mod_id, std::string *version);  // 0 no, 1 yes
    // ---- thumbnails
    Image *image(int tab, const std::string &url);   // requests on first call; the pointer stays valid until stop()
    // ---- settings
    BrowserSettings settings();
    void set_settings(const BrowserSettings &s);     // saved; sources are made again
    const BrowserPaths &paths() const { return paths_; }
    std::string log_tail();                     // recent messages (the UI's status line)

    void wait_idle();                           // tests: until no task is queued or running

    struct Impl;                                // (mb_browser.cpp)

private:
    std::unique_ptr<Impl> d_;
    BrowserPaths paths_;
    bool started_ = false;
};

}  // namespace mb
