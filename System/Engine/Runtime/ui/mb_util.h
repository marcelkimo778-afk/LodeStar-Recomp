// mb_util.h - small helpers for the mod browser (ui/mb_*.cpp): UTF-8 paths on the wide Windows API, file walking,
// recursive delete, hashes (Windows CNG), running a helper process, a tiny JSON reader / writer.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace mb {

// ---- strings / paths (UTF-8 everywhere; '/' or '\' accepted)
std::wstring wide(const std::string &s);
std::string utf8(const std::wstring &w);
std::string join(const std::string &a, const std::string &b);
std::string lower(std::string s);
bool ends_with_i(const std::string &s, const char *suffix);
std::string file_ext(const std::string &name);           // ".zip" (lower case), "" if none
std::string base_name(const std::string &path);
std::string trim(const std::string &s);
std::string safe_folder_name(const std::string &name);   // a folder name Windows accepts, never "" / "." / ".."

// ---- file system
bool is_dir(const std::string &p);
bool is_file(const std::string &p);
uint64_t file_size(const std::string &p);                 // 0 if missing
bool make_dirs(const std::string &p);
bool remove_tree(const std::string &p);                   // files, folders, links (not followed)
bool move_path(const std::string &from, const std::string &to);   // same volume: rename; else copy + delete
bool read_text(const std::string &p, std::string *out, uint64_t limit = 64u << 20);
bool write_text_atomic(const std::string &p, const std::string &text);
bool copy_file(const std::string &from, const std::string &to);
struct Entry { std::string rel; bool dir = false, link = false; uint64_t size = 0; };
// every entry under root (relative paths with '/'); links are reported, never followed
void walk(const std::string &root, std::vector<Entry> *out, size_t limit = 1000000);
std::string canonical(const std::string &p);              // absolute, '/' separators, lower case (for comparing)
std::string temp_name(const char *prefix);                // unique name part (time + counter)

// ---- hashes (lower-case hex), 0 on error
bool file_md5(const std::string &p, std::string *hex);
bool file_sha256(const std::string &p, std::string *hex);

// ---- a helper process (no window); stdout captured (limit), 0 = exit code 0.  kill(): checked every 100 ms,
// return true to stop it (then the result is -2).  -1: could not start.
int run_process(const std::string &exe, const std::vector<std::string> &args, std::string *out, uint64_t out_limit,
                unsigned timeout_ms, const std::function<bool()> &kill);

// ---- JSON (enough for API answers and our own files)
struct Json;
using JsonP = std::shared_ptr<Json>;
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JsonP> a;
    std::vector<std::pair<std::string, JsonP>> o;
    const Json &operator[](const char *k) const;          // missing -> a null value
    const Json &operator[](size_t i) const;
    bool has(const char *k) const;
    std::string str(const char *def = "") const;          // numbers are formatted too
    double num(double def = 0) const;                      // strings holding numbers are parsed too
    int64_t i64(int64_t def = 0) const { return (int64_t)num((double)def); }
    bool truthy() const;
    size_t size() const { return t == Arr ? a.size() : t == Obj ? o.size() : 0; }
};
JsonP json_parse(const std::string &text, std::string *err = nullptr);
std::string json_quote(const std::string &s);

int64_t now_unix();
std::string iso_time(int64_t unix_seconds);               // "2026-11-12T17:30:00Z"

}  // namespace mb
