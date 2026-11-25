// mb_install.h - the mod browser's install pipeline and version compatibility (MODS.md "Mod browser").
//
// install_archive: archive (zip / 7z / rar via Windows' own tar.exe = libarchive) -> checked -> extracted into a
// private temp folder under the mods root -> executables / scripts refused -> layout detected and normalized ->
// <mods_root>/01001F5010DFA000/<Name>/{romfs,exefs,cheats} + mod_meta.json, moved into place in one step.  Any
// failure removes everything it made; an update replaces the old folder only after the new one is complete.
#pragma once
#include <string>
#include <vector>

#include "mb_source.h"

namespace mb {

extern const char *const kTitleId;              // "01001F5010DFA000"

struct Target {                                 // the game this recomp was built from
    std::string build_id;                       // main NSO build id (hex), from the recompiled module table
    std::string version;                        // "1.0.0": config value (the dump has no control.nacp), see MODS.md
};

// Version compatibility (spec section 6).  Sources, in this order: (1) the source's game-version field,
// (2) build ids in .pchtxt / .ips names / cheat file names, (3) version keywords in names / descriptions,
// (4) unknown.  Build ids decide over the others (they are exact).
enum Compat { COMPAT_UNKNOWN, COMPAT_MATCH, COMPAT_MISMATCH };
struct VersionInfo {
    Compat compat = COMPAT_UNKNOWN;
    std::string detected;                       // "1.0.0", "build 7FCAD279...", "" (unknown)
    std::string why;                            // human text: where it came from
    bool code_specific = false;                 // the decision came from exefs patch / cheat build ids
};
VersionInfo detect_version_meta(const ModDetails &mod, const ModFile *file, const Target &t);   // before download
VersionInfo detect_version_tree(const std::string &dir, const ModDetails &mod, const ModFile *file, const Target &t);
const char *compat_name(Compat c);              // "match" / "mismatch" / "unknown"
std::string normalize_game_version(const std::string &s);   // "v1.0" -> "1.0.0", "" if not a version

struct InstallOptions {
    std::string mods_root;                      // the mods folder (installs go to <root>/01001F5010DFA000/...)
    Target target;
    bool allow_wrong_version = false;           // "install anyway" for exefs patches / cheats of another build
    uint64_t max_unpacked = 8ull << 30;
    uint32_t max_files = 50000;
    std::string tar_exe;                        // default %SystemRoot%\System32\tar.exe
};

struct InstallResult {
    bool ok = false;
    bool blocked_version = false;               // refused because of a build id mismatch (allow_wrong_version)
    std::string error;
    std::vector<std::string> refused;           // executables / scripts that were removed
    std::vector<std::string> dropped;           // other files that are not mod content (not installed)
    std::vector<std::string> warnings;
    std::vector<std::string> folders;           // installed mod folders (one per variant)
    VersionInfo version;
};

// the hash the source gave (SHA-256 preferred, else MD5); true when it matches or the source gave none
bool verify_download(const std::string &path, const ModFile &f, std::string *err);

bool install_archive(const std::string &archive, const ModDetails &mod, const ModFile &file, const InstallOptions &o,
                     InstallResult *r);
// remove one installed mod folder (it must be inside mods_root); files the running game still has open go with the
// next cleanup.  *deferred = some files could not be deleted yet.
bool uninstall_mod(const std::string &folder, const std::string &mods_root, std::string *err, bool *deferred);
// mods with a mod_meta.json under mods_root (what the browser installed)
std::vector<InstalledMeta> list_installed(const std::string &mods_root);
bool read_meta(const std::string &folder, InstalledMeta *m);
void cleanup_tmp(const std::string &mods_root);   // leftovers of interrupted installs / uninstalls
bool version_newer(const std::string &a, const std::string &b);   // a > b ("1.10" > "1.9", "v2" > "1.5")

}  // namespace mb
