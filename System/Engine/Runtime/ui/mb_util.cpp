// mb_util.cpp - see mb_util.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <atomic>

#include "mb_util.h"

#pragma comment(lib, "bcrypt.lib")

namespace mb {

std::wstring wide(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    for (auto &c : w) if (c == L'/') c = L'\\';
    return w;
}

std::string utf8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::string join(const std::string &a, const std::string &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    char l = a.back();
    return (l == '/' || l == '\\') ? a + b : a + "/" + b;
}

std::string lower(std::string s)
{
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

bool ends_with_i(const std::string &s, const char *suf)
{
    size_t m = strlen(suf);
    return s.size() >= m && lower(s.substr(s.size() - m)) == lower(suf);
}

std::string file_ext(const std::string &name)
{
    std::string b = base_name(name);
    size_t d = b.rfind('.');
    return d == std::string::npos || d == 0 ? std::string() : lower(b.substr(d));
}

std::string base_name(const std::string &p)
{
    size_t e = p.size();
    while (e && (p[e - 1] == '/' || p[e - 1] == '\\')) e--;
    size_t s = e;
    while (s && p[s - 1] != '/' && p[s - 1] != '\\') s--;
    return p.substr(s, e - s);
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string safe_folder_name(const std::string &name)
{
    std::string o;
    for (unsigned char c : name) {
        if (c < 0x20 || strchr("<>:\"/\\|?*", c)) o += '_';
        else o += (char)c;
    }
    o = trim(o);
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    while (!o.empty() && o[0] == '.') o.erase(0, 1);
    if (o.size() > 80) {                                  // do not cut a UTF-8 sequence
        size_t k = 80;
        while (k && ((unsigned char)o[k] & 0xC0) == 0x80) k--;
        o.resize(k);
    }
    static const char *reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6",
                                     "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                                     "lpt7", "lpt8", "lpt9"};
    std::string l = lower(o);
    for (const char *r : reserved) if (l == r) o += "_";
    return o.empty() ? std::string("mod") : o;
}

bool is_dir(const std::string &p)
{
    DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_file(const std::string &p)
{
    DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t file_size(const std::string &p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(p).c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

bool make_dirs(const std::string &p)
{
    if (p.empty()) return false;
    if (is_dir(p)) return true;
    std::wstring w = wide(p);
    for (size_t i = 1; i < w.size(); i++)
        if (w[i] == L'\\' && w[i - 1] != L':' && w[i - 1] != L'\\') {
            std::wstring part = w.substr(0, i);
            CreateDirectoryW(part.c_str(), NULL);
        }
    CreateDirectoryW(w.c_str(), NULL);
    return is_dir(p);
}

static bool remove_tree_w(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(p.c_str()) != 0;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(p.c_str()) != 0;   // a junction: the link only
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            ok = remove_tree_w(p + L"\\" + fd.cFileName) && ok;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(p.c_str()) != 0 && ok;
}

bool remove_tree(const std::string &p) { return remove_tree_w(wide(p)); }

bool copy_file(const std::string &from, const std::string &to)
{
    return CopyFileW(wide(from).c_str(), wide(to).c_str(), FALSE) != 0;
}

static bool copy_tree(const std::string &from, const std::string &to)
{
    if (is_file(from)) return copy_file(from, to);
    if (!make_dirs(to)) return false;
    std::vector<Entry> es;
    walk(from, &es);
    for (const auto &e : es) {
        if (e.link) continue;
        std::string s = join(from, e.rel), d = join(to, e.rel);
        if (e.dir) { if (!make_dirs(d)) return false; }
        else if (!copy_file(s, d)) return false;
    }
    return true;
}

bool move_path(const std::string &from, const std::string &to)
{
    if (MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    if (GetLastError() != ERROR_NOT_SAME_DEVICE) return false;
    if (!copy_tree(from, to)) { remove_tree(to); return false; }
    remove_tree(from);
    return true;
}

bool read_text(const std::string &p, std::string *out, uint64_t limit)
{
    HANDLE h = CreateFileW(wide(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || (uint64_t)sz.QuadPart > limit) { CloseHandle(h); return false; }
    out->assign((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = sz.QuadPart == 0 || ReadFile(h, &(*out)[0], (DWORD)sz.QuadPart, &got, NULL);
    CloseHandle(h);
    return ok && got == (DWORD)sz.QuadPart;
}

bool write_text_atomic(const std::string &p, const std::string &text)
{
    std::string tmp = p + ".tmp";
    HANDLE h = CreateFileW(wide(tmp).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, NULL) && put == text.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(wide(tmp).c_str(), wide(p).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(wide(tmp).c_str());
    return ok != 0;
}

static void walk_w(const std::wstring &root, const std::string &rel, std::vector<Entry> *out, size_t limit, int depth)
{
    if (depth > 64 || out->size() >= limit) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        Entry e;
        std::string name = utf8(fd.cFileName);
        e.rel = rel.empty() ? name : rel + "/" + name;
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out->push_back(e);
        if (out->size() >= limit) break;
        if (e.dir && !e.link) walk_w(root + L"\\" + fd.cFileName, e.rel, out, limit, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void walk(const std::string &root, std::vector<Entry> *out, size_t limit) { walk_w(wide(root), "", out, limit, 0); }

std::string canonical(const std::string &p)
{
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(wide(p).c_str(), 4096, buf, NULL);
    std::string s = n && n < 4096 ? utf8(std::wstring(buf, n)) : p;
    for (auto &c : s) if (c == '\\') c = '/';
    s = lower(s);
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
}

std::string temp_name(const char *prefix)
{
    static std::atomic<unsigned> counter{0};
    char b[96];
    snprintf(b, sizeof b, "%s%llx-%lx-%u", prefix, (unsigned long long)GetTickCount64(), GetCurrentProcessId(), counter++);
    return b;
}

static bool file_hash(const std::string &p, LPCWSTR alg, std::string *hex)
{
    BCRYPT_ALG_HANDLE a = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    bool ok = false;
    HANDLE f = CreateFileW(wide(p).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    if (BCryptOpenAlgorithmProvider(&a, alg, NULL, 0) == 0 && BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        ok = true;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, NULL) && got)
            if (BCryptHashData(h, buf.data(), got, 0) != 0) { ok = false; break; }
        DWORD len = 0, cb = 0;
        unsigned char out[64];
        if (ok && BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof len, &cb, 0) == 0 && len <= 64 &&
            BCryptFinishHash(h, out, len, 0) == 0) {
            hex->clear();
            char t[3];
            for (DWORD i = 0; i < len; i++) { snprintf(t, sizeof t, "%02x", out[i]); *hex += t; }
        } else ok = false;
    }
    if (h) BCryptDestroyHash(h);
    if (a) BCryptCloseAlgorithmProvider(a, 0);
    CloseHandle(f);
    return ok;
}

bool file_md5(const std::string &p, std::string *hex) { return file_hash(p, BCRYPT_MD5_ALGORITHM, hex); }
bool file_sha256(const std::string &p, std::string *hex) { return file_hash(p, BCRYPT_SHA256_ALGORITHM, hex); }

static std::wstring quote_arg(const std::wstring &a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; bs = 0; continue; }
        o.append(bs, L'\\');
        bs = 0;
        o += c;
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}

int run_process(const std::string &exe, const std::vector<std::string> &args, std::string *out, uint64_t out_limit,
                unsigned timeout_ms, const std::function<bool()> &kill)
{
    std::wstring cmd = quote_arg(wide(exe));
    for (const auto &a : args) cmd += L" " + quote_arg(wide(a));
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    BOOL ok = CreateProcessW(wide(exe).c_str(), cl.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    int result = 0;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
            char buf[8192];
            DWORD got = 0;
            if (!ReadFile(rd, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &got, NULL) || !got) break;
            if (out && out->size() < out_limit) out->append(buf, got);
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 100);
        if (w == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            result = (int)code;
            DWORD got = 0;
            char buf[8192];
            while (PeekNamedPipe(rd, NULL, 0, NULL, &got, NULL) && got && ReadFile(rd, buf, sizeof buf, &got, NULL) && got)
                if (out && out->size() < out_limit) out->append(buf, got);
            break;
        }
        if ((kill && kill()) || (timeout_ms && GetTickCount64() - t0 > timeout_ms)) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            result = -2;
            break;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return result;
}

// ---- JSON
static const Json kNull;

const Json &Json::operator[](const char *k) const
{
    if (t == Obj) for (const auto &kv : o) if (kv.first == k) return *kv.second;
    return kNull;
}
const Json &Json::operator[](size_t i) const { return t == Arr && i < a.size() ? *a[i] : kNull; }
bool Json::has(const char *k) const
{
    if (t == Obj) for (const auto &kv : o) if (kv.first == k) return true;
    return false;
}
std::string Json::str(const char *def) const
{
    if (t == Str) return s;
    if (t == Num) {
        char buf[64];
        if (n == (double)(int64_t)n) snprintf(buf, sizeof buf, "%lld", (long long)n); else snprintf(buf, sizeof buf, "%g", n);
        return buf;
    }
    if (t == Bool) return b ? "true" : "false";
    return def;
}
double Json::num(double def) const
{
    if (t == Num) return n;
    if (t == Str && !s.empty()) { char *e; double v = strtod(s.c_str(), &e); return *e ? def : v; }
    if (t == Bool) return b ? 1 : 0;
    return def;
}
bool Json::truthy() const { return t == Bool ? b : t == Num ? n != 0 : t == Str ? !s.empty() : t != Null; }

struct JParser {
    const char *p, *e;
    std::string err;
    int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }
    static void put(std::string &o, unsigned cp)
    {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned *v)
    {
        if (e - p < 4) return false;
        *v = 0;
        for (int i = 0; i < 4; i++) {
            char c = p[i];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            *v = *v * 16 + (unsigned)d;
        }
        p += 4;
        return true;
    }
    bool string(std::string &o)
    {
        p++;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') { o += c; continue; }
            if (p >= e) break;
            char x = *p++;
            unsigned cp;
            switch (x) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u':
                if (!hex4(&cp)) { err = "bad \\u escape"; return false; }
                if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    const char *save = p;
                    p += 2;
                    unsigned lo;
                    if (hex4(&lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else p = save;
                }
                put(o, cp);
                break;
            default: o += x; break;
            }
        }
        if (p >= e) { err = "unterminated string"; return false; }
        p++;
        return true;
    }
    JsonP value()
    {
        ws();
        if (p >= e) { err = "unexpected end"; return nullptr; }
        if (++depth > 64) { err = "nested too deep"; return nullptr; }
        auto v = std::make_shared<Json>();
        char c = *p;
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            v->t = obj ? Json::Obj : Json::Arr;
            p++;
            for (;;) {
                ws();
                if (p < e && *p == (obj ? '}' : ']')) { p++; break; }
                std::string key;
                if (obj) {
                    if (p >= e || *p != '"' || !string(key)) { if (err.empty()) err = "expected a key"; return nullptr; }
                    ws();
                    if (p >= e || *p != ':') { err = "expected ':'"; return nullptr; }
                    p++;
                }
                JsonP it = value();
                if (!it) return nullptr;
                if (obj) v->o.emplace_back(key, it); else v->a.push_back(it);
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == (obj ? '}' : ']')) { p++; break; }
                err = "expected ',' or a closing bracket";
                return nullptr;
            }
        } else if (c == '"') {
            v->t = Json::Str;
            if (!string(v->s)) return nullptr;
        } else if (e - p >= 4 && !strncmp(p, "true", 4)) { v->t = Json::Bool; v->b = true; p += 4; }
        else if (e - p >= 5 && !strncmp(p, "false", 5)) { v->t = Json::Bool; p += 5; }
        else if (e - p >= 4 && !strncmp(p, "null", 4)) { p += 4; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            std::string num;
            while (p < e && strchr("+-0123456789.eE", *p)) num += *p++;
            v->t = Json::Num;
            v->n = strtod(num.c_str(), nullptr);
        } else { err = "unexpected character"; return nullptr; }
        depth--;
        return v;
    }
};

JsonP json_parse(const std::string &text, std::string *err)
{
    JParser jp{text.data(), text.data() + text.size(), {}, 0};
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) jp.p += 3;
    JsonP v = jp.value();
    if (v) { jp.ws(); if (jp.p != jp.e) { jp.err = "text after the end"; v = nullptr; } }
    if (!v && err) *err = jp.err;
    return v;
}

std::string json_quote(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

int64_t now_unix() { return (int64_t)time(NULL); }

std::string iso_time(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm g;
    gmtime_s(&g, &tt);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

}  // namespace mb
