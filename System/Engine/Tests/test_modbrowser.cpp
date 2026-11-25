#define NOMINMAX
// Mod browser core test (Engine/Tests/test_modbrowser.bat): mock source, install pipeline, version compatibility,
// update, uninstall - offline, no game.  Fixture: modbrowser_fixture (make_modbrowser_fixture.py).
//   test_modbrowser <fixture> <tmp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../Runtime/ui/mb_browser.h"
#include "../Runtime/ui/mb_install.h"
#include "../Runtime/ui/mb_source.h"

using namespace mb;

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string g_fx, g_root;
static Target g_target = {"AABBCCDD11223344556677889900AABBCCDDEEFF000000000000000000000000", "1.0.0"};

struct Got { bool ok; InstallResult r; std::string err; };

static ModDetails details_of(ModSource &s, const std::string &id)
{
    ModDetails d;
    std::string e;
    if (!s.details(id, &d, &e)) printf("details(%s): %s\n", id.c_str(), e.c_str());
    return d;
}

// the browser's flow: download -> verify -> install
static Got install(ModSource &s, const std::string &id, bool allow = false, uint64_t max_unpacked = 32ull << 20)
{
    Got g{};
    ModDetails d = details_of(s, id);
    if (d.files.empty()) { g.err = "no files"; return g; }
    std::string dl = join(join(g_root, ".."), "dl_" + id + file_ext(d.files[0].name));
    std::string browser;
    if (!s.download(d.files[0], dl, nullptr, &g.err, &browser)) return g;
    if (!verify_download(dl, d.files[0], &g.err)) { DeleteFileW(wide(dl).c_str()); return g; }
    InstallOptions o;
    o.mods_root = g_root;
    o.target = g_target;
    o.allow_wrong_version = allow;
    o.max_unpacked = max_unpacked;
    g.ok = install_archive(dl, d, d.files[0], o, &g.r);
    g.err = g.r.error;
    DeleteFileW(wide(dl).c_str());
    return g;
}

static std::string mod_dir(const char *name) { return join(join(g_root, kTitleId), name); }
static std::string slurp(const std::string &p) { std::string s; read_text(p, &s); return s; }
static bool tmp_empty()
{
    std::vector<Entry> es;
    walk(join(g_root, ".mb_tmp"), &es);
    return es.empty();
}
static bool has(const std::vector<std::string> &v, const char *s)
{
    for (const auto &x : v) if (x.find(s) != std::string::npos) return true;
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_modbrowser <fixture> <tmp>\n"); return 2; }
    g_fx = argv[1];
    g_root = join(argv[2], "mods");
    remove_tree(argv[2]);
    make_dirs(g_root);
    auto mock = make_mock_source(g_fx);

    // ---- search, sort, pagination, details
    SearchQuery q;
    q.per_page = 10;
    SearchResult r;
    std::string e;
    CHECK(mock->search(q, &r, &e) && r.mods.size() == 10 && r.more && r.total >= 38, "page 1: %zu mods, total %d (%s)",
          r.mods.size(), r.total, e.c_str());
    q.page = 4;
    CHECK(mock->search(q, &r, &e) && !r.more && !r.mods.empty(), "last page");
    q.page = 1;
    q.text = "shiny";
    CHECK(mock->search(q, &r, &e) && r.mods.size() == 1 && r.mods[0].name == "Shiny Textures", "search 'shiny'");
    q.text = "";
    q.category = "Cheats";
    CHECK(mock->search(q, &r, &e) && r.mods.size() == 1 && r.mods[0].id == "10", "category filter");
    q.category = "";
    q.sort = SORT_POPULAR;
    CHECK(mock->search(q, &r, &e) && r.mods[0].id == "1", "popular first: %s", r.mods.empty() ? "-" : r.mods[0].id.c_str());
    ModDetails d = details_of(*mock, "1");
    CHECK(d.name == "Shiny Textures" && d.files.size() == 1 && d.images.size() == 2 && d.files[0].size > 0, "details");
    std::string img;
    CHECK(mock->image(d.thumb_url, &img, &e) && img.size() > 8 && !memcmp(img.data() + 1, "PNG", 3), "thumbnail bytes");
    _putenv("SW_MB_MOCK_OFFLINE=1");
    CHECK(!mock->search(q, &r, &e) && e.find("offline") != std::string::npos, "offline search must fail softly: %s", e.c_str());
    _putenv("SW_MB_MOCK_OFFLINE=");

    // ---- version detection (section 6)
    CHECK(normalize_game_version("v1.0") == "1.0.0" && normalize_game_version("1.1.1") == "1.1.1" &&
          normalize_game_version("abc").empty(), "normalize");
    {
        ModDetails m;
        m.name = "Thing";
        CHECK(detect_version_meta(m, nullptr, g_target).compat == COMPAT_UNKNOWN, "no info = unknown");
        m.description = "Works on v1.1.1";
        VersionInfo v = detect_version_meta(m, nullptr, g_target);
        CHECK(v.compat == COMPAT_MISMATCH && v.detected == "1.1.1", "keyword mismatch: %s", v.detected.c_str());
        m.description = "Requires game version 1.0.0 (tested on 1.0.0)";
        CHECK(detect_version_meta(m, nullptr, g_target).compat == COMPAT_MATCH, "keyword match");
        m.description = "My mod v2.3, see 1.0 notes";
        CHECK(detect_version_meta(m, nullptr, g_target).compat == COMPAT_UNKNOWN, "a mod's own version is not a game version");
        m.game_version = "1.0.0";
        m.description = "v1.1.1";
        CHECK(detect_version_meta(m, nullptr, g_target).compat == COMPAT_MATCH, "the source's field goes first");
    }
    CHECK(version_newer("1.1", "1.0") && version_newer("1.10", "1.9") && !version_newer("1.0", "1.0") &&
          !version_newer("", "1.0"), "version_newer");

    // ---- installs: every layout
    Got g = install(*mock, "1");
    CHECK(g.ok, "root layout: %s", g.err.c_str());
    CHECK(slurp(join(mod_dir("Shiny Textures"), "romfs/bin/field/mock_a.bin")) == "MOCK_A_V1", "root layout content");
    CHECK(is_file(join(mod_dir("Shiny Textures"), "readme.txt")), "readme kept");
    InstalledMeta m;
    CHECK(read_meta(mod_dir("Shiny Textures"), &m) && m.source == "mock" && m.mod_id == "1" && m.author == "Tester" &&
          m.compat == "match" && m.detected_version == "1.0.0" && !m.installed_at.empty(), "mod_meta.json: %s %s", m.compat.c_str(), m.detected_version.c_str());
    g = install(*mock, "2");
    CHECK(g.ok && is_file(join(mod_dir("Cool Mod"), "romfs/bin/field/cool.bin")) && is_file(join(mod_dir("Cool Mod"), "notes.md")),
          "nested folder: %s", g.err.c_str());
    CHECK(g.r.version.compat == COMPAT_MISMATCH && has(g.r.warnings, "another game version"), "romfs mod for 1.1.1: warned, installed");
    g = install(*mock, "3");
    CHECK(g.ok && is_file(join(mod_dir("Title Folder Mod"), "romfs/bin/field/tid.bin")) &&
          is_file(join(mod_dir("Title Folder Mod"), "exefs/fps.pchtxt")), "TitleID folder: %s", g.err.c_str());
    CHECK(g.r.version.compat == COMPAT_MATCH && g.r.version.code_specific, "pchtxt build id match");
    g = install(*mock, "4");
    CHECK(g.ok && slurp(join(mod_dir("Bare Bin Mod"), "romfs/bin/field/bare.bin")) == "BARE", "bare bin/: %s", g.err.c_str());
    CHECK(g.r.version.compat == COMPAT_UNKNOWN, "romfs only, no info: unknown");
    g = install(*mock, "15");
    CHECK(g.ok && is_file(join(mod_dir("Seven Zip Mod"), "romfs/bin/field/seven.bin")), "7z: %s", g.err.c_str());

    // ---- refused archives leave nothing behind
    g = install(*mock, "5");
    CHECK(!g.ok && g.err.find("unsafe") != std::string::npos && !is_dir(mod_dir("Zip Slip")) && tmp_empty(), "zip slip: %s", g.err.c_str());
    g = install(*mock, "6");
    CHECK(!g.ok && g.err.find("limit") != std::string::npos && !is_dir(mod_dir("Zip Bomb")) && tmp_empty(), "zip bomb: %s", g.err.c_str());
    g = install(*mock, "12");
    CHECK(!g.ok && g.err.find("link") != std::string::npos && !is_dir(mod_dir("Symlink Mod")) && tmp_empty(), "symlink: %s", g.err.c_str());
    g = install(*mock, "13");
    CHECK(!g.ok && g.err.find("no mod content") != std::string::npos && tmp_empty(), "nothing inside: %s", g.err.c_str());
    g = install(*mock, "14");
    CHECK(!g.ok && g.err.find("MD5") != std::string::npos && !is_dir(mod_dir("Bad Hash")), "bad hash: %s", g.err.c_str());

    // ---- executables refused, the rest installed
    g = install(*mock, "7");
    CHECK(g.ok && has(g.r.refused, "installer.exe") && has(g.r.refused, "run.bat") && has(g.r.refused, "payload.dll"),
          "exe refused: %s (%zu refused)", g.err.c_str(), g.r.refused.size());
    CHECK(is_file(join(mod_dir("Has An Exe"), "romfs/bin/field/exe_mod.bin")) &&
          !is_file(join(mod_dir("Has An Exe"), "romfs/bin/field/payload.dll")) && !is_dir(join(mod_dir("Has An Exe"), "tools")),
          "exe mod content");

    // ---- version rules at install time
    g = install(*mock, "8");
    CHECK(g.ok && g.r.version.compat == COMPAT_MATCH, "mixed romfs + matching pchtxt: %s", g.err.c_str());
    g = install(*mock, "9");
    CHECK(!g.ok && g.r.blocked_version && !is_dir(mod_dir("Wrong Build Patch")) && tmp_empty(), "wrong build blocked: %s", g.err.c_str());
    g = install(*mock, "9", true);
    CHECK(g.ok && g.r.version.compat == COMPAT_MISMATCH && is_dir(mod_dir("Wrong Build Patch")), "install anyway: %s", g.err.c_str());
    CHECK(read_meta(mod_dir("Wrong Build Patch"), &m) && m.compat == "mismatch", "mismatch stored in mod_meta.json");
    g = install(*mock, "10");
    CHECK(g.ok && is_file(join(mod_dir("Money Cheats"), "cheats/AABBCCDD11223344.txt")) && g.r.version.compat == COMPAT_MATCH,
          "cheats: %s", g.err.c_str());

    // ---- variants: one mod each, only the first switched on
    g = install(*mock, "11");
    CHECK(g.ok && g.r.folders.size() == 2 && !is_file(join(mod_dir("Two Variants - Option A"), ".disabled")) &&
          is_file(join(mod_dir("Two Variants - Option B"), ".disabled")), "variants: %s (%zu)", g.err.c_str(), g.r.folders.size());

    // ---- update: same source + mod id replaces the folder (a .disabled marker is kept)
    write_text_atomic(join(mod_dir("Shiny Textures"), ".disabled"), "");
    std::string v1 = join(g_fx, "index.json"), backup = join(argv[2], "index_v1.json");
    copy_file(v1, backup);
    copy_file(join(g_fx, "index_v2.json"), v1);
    auto mock2 = make_mock_source(g_fx);
    copy_file(backup, v1);
    std::string latest;
    read_meta(mod_dir("Shiny Textures"), &m);
    CHECK(mock2->check_update(m, &latest, &e) && latest == "1.1" && version_newer(latest, m.version), "update found: %s", latest.c_str());
    g = install(*mock2, "1");
    CHECK(g.ok && slurp(join(mod_dir("Shiny Textures"), "romfs/bin/field/mock_a.bin")) == "MOCK_A_V2" &&
          is_file(join(mod_dir("Shiny Textures"), "romfs/bin/field/mock_new.bin")) && !is_dir(mod_dir("Shiny Textures (2)")) &&
          is_file(join(mod_dir("Shiny Textures"), ".disabled")), "update in place: %s", g.err.c_str());
    CHECK(read_meta(mod_dir("Shiny Textures"), &m) && m.version == "1.1", "updated version in mod_meta.json");

    // ---- installed list, uninstall
    std::vector<InstalledMeta> inst = list_installed(g_root);
    CHECK(inst.size() == 11, "installed: %zu", inst.size());   // 1,2,3,4,15,7,8,9,10 + 2 variants
    bool deferred = false;
    CHECK(uninstall_mod(mod_dir("Cool Mod"), g_root, &e, &deferred) && !is_dir(mod_dir("Cool Mod")) && !deferred, "uninstall: %s", e.c_str());
    CHECK(!uninstall_mod(argv[2], g_root, &e, &deferred) && is_dir(argv[2]), "uninstall outside the mods folder refused");
    CHECK(list_installed(g_root).size() == 10, "after uninstall");
    CHECK(tmp_empty(), "no leftovers in .mb_tmp");

    // ---- the Browser (what the in-game UI drives): tabs, search, details, queue, installed, uninstall, images
    {
        std::string broot = join(argv[2], "bmods");
        make_dirs(broot);
        Browser b;
        BrowserPaths bp;
        bp.settings_file = join(argv[2], "browser_settings.json");
        bp.mods_root = broot;
        bp.cache_dir = join(argv[2], "bcache");
        bp.mock_dir = g_fx;
        bp.target = g_target;
        CHECK(b.start(bp) && b.tab_count() >= 4, "browser start: %d tabs", b.tab_count());
        TabView t0 = b.tab(0);
        CHECK(t0.id == "mock" && t0.available, "tab 0 = mock");
        TabView gb = b.tab(1);
        CHECK(gb.id == "gamebanana" && !gb.available && !gb.why.empty(), "GameBanana without a game id: %s", gb.why.c_str());
        SearchQuery bq;
        bq.per_page = 12;
        b.search(0, bq, false);
        b.wait_idle();
        t0 = b.tab(0);
        CHECK(t0.state == ST_OK && t0.mods.size() == 12 && t0.more && t0.compat.size() == 12, "browser search: %d %zu", t0.state, t0.mods.size());
        bq.page = 2;
        b.search(0, bq, true);
        b.wait_idle();
        CHECK(b.tab(0).mods.size() == 24, "next page appended: %zu", b.tab(0).mods.size());
        b.search(1, bq, false);
        b.wait_idle();
        CHECK(b.tab(1).state == ST_ERROR && !b.tab(1).error.empty(), "an unavailable source shows its error, nothing crashes");
        // badges from the listing: Shiny Textures says 1.0.0, Cool Mod names 1.1.1, Bare Bin Mod says nothing
        bq.page = 1;
        bq.per_page = 50;
        b.search(0, bq, false);
        b.wait_idle();
        t0 = b.tab(0);
        int c1 = -1, c2 = -1, c4 = -1;
        for (size_t i = 0; i < t0.mods.size(); i++) {
            if (t0.mods[i].id == "1") c1 = t0.compat[i].compat;
            if (t0.mods[i].id == "4") c4 = t0.compat[i].compat;
        }
        b.open_details(0, "2");
        b.wait_idle();
        DetailView dv = b.details();
        c2 = dv.compat.compat;
        CHECK(c1 == COMPAT_MATCH && c2 == COMPAT_MISMATCH && c4 == COMPAT_UNKNOWN, "badges: %d %d %d", c1, c2, c4);
        CHECK(dv.state == ST_OK && dv.mod.name == "Cool Mod" && !dv.mod.files.empty(), "detail page");
        Image *im = b.image(0, t0.mods[0].thumb_url);
        b.wait_idle();
        CHECK(im && im->state == ST_OK && im->w > 0 && im->h > 0 && im->rgba.size() == (size_t)im->w * im->h * 4,
              "thumbnail decoded: %d %dx%d", im ? im->state : -1, im ? im->w : 0, im ? im->h : 0);
        // queue: install, blocked + install anyway, local file
        b.open_details(0, "1");
        b.wait_idle();
        ModDetails m1 = b.details().mod;
        int j1 = b.install(0, m1, m1.files[0], false);
        b.open_details(0, "9");
        b.wait_idle();
        ModDetails m9 = b.details().mod;
        int j9 = b.install(0, m9, m9.files[0], false);
        int jl = b.install_local(join(g_fx, "files/bare_bin.zip"), false);
        int jbad = b.install_local("http://example.invalid/x.zip", false);
        b.wait_idle();
        std::vector<JobView> js = b.jobs();
        auto st = [&](int id) { for (auto &j : js) if (j.id == id) return j.state; return -1; };
        CHECK(st(j1) == JOB_DONE && st(j9) == JOB_BLOCKED && st(jl) == JOB_DONE && jbad == 0,
              "jobs: %d %d %d (plain http refused: %d)", st(j1), st(j9), st(jl), jbad);
        int j9b = b.install(0, m9, m9.files[0], true);
        b.wait_idle();
        js = b.jobs();
        CHECK(st(j9b) == JOB_DONE, "install anyway: %d", st(j9b));
        std::vector<InstalledView> iv = b.installed();
        CHECK(iv.size() == 3, "browser installed: %zu", iv.size());
        std::string ver;
        CHECK(b.installed_state("mock", "1", &ver) == 1 && ver == "1.0", "installed badge");
        std::string folder;
        for (auto &i : iv) if (i.meta.mod_id == "1") folder = i.meta.folder;
        CHECK(b.set_enabled(folder, false) && is_file(join(folder, ".disabled")), "switch off (marker)");
        CHECK(b.set_enabled(folder, true) && !is_file(join(folder, ".disabled")), "switch on");
        std::string msg;
        CHECK(b.uninstall(folder, &msg) && !is_dir(folder) && b.installed().size() == 2, "browser uninstall: %s", msg.c_str());
        // offline: every call fails softly
        _putenv("SW_MB_MOCK_OFFLINE=1");
        b.search(0, bq, false);
        b.wait_idle();
        CHECK(b.tab(0).state == ST_ERROR && b.tab(0).error.find("offline") != std::string::npos, "offline tab error");
        _putenv("SW_MB_MOCK_OFFLINE=");
        b.stop();
    }

    // ---- settings round trip; keys stay local
    BrowserSettings s, s2;
    s.nexus_key = "secret-key";
    s.github_repos = {"owner/repo"};
    s.only_compatible = false;
    std::string sp = join(argv[2], "modbrowser.json");
    CHECK(settings_save(sp, s) && settings_load(sp, &s2) && s2.nexus_key == "secret-key" && s2.github_repos.size() == 1 &&
          !s2.only_compatible && s2.target_version == "1.0.0", "settings round trip");

    printf("modbrowser: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
