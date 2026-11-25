// mb_http.cpp - see mb_http.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <map>
#include <mutex>

#include "mb_http.h"

#pragma comment(lib, "winhttp.lib")

namespace mb {

const char *http_user_agent() { return "LodeStar-ModBrowser/1.0"; }

static std::mutex g_rate_lock;
static std::map<std::wstring, ULONGLONG> g_last_request;

// UTF-8 -> UTF-16 as it is.  wide() (mb_util) is for file paths and turns every '/' into a backslash: that broke the
// User-Agent ("LodeStar-ModBrowser<backslash>1.0") and headers like the Accept type, and every request then hung
// until it timed out (WinHTTP 12002 / 12030; found 2026-11-25 testing GitHub).  URLs, the agent and headers use this.
static std::wstring wtext(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static void rate_wait(const std::wstring &host, double interval_s)
{
    ULONGLONG wait = 0;
    {
        std::lock_guard<std::mutex> l(g_rate_lock);
        ULONGLONG now = GetTickCount64(), gap = (ULONGLONG)(interval_s * 1000);
        ULONGLONG &last = g_last_request[host];
        ULONGLONG next = last + gap;
        if (last && now < next) wait = next - now;
        last = now + wait;
    }
    if (wait) Sleep((DWORD)wait);
}

struct Conn {
    HINTERNET s = NULL, c = NULL, r = NULL;
    ~Conn()
    {
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
    }
};

static std::string last_error_text(const char *what)
{
    DWORD e = GetLastError();
    char b[160];
    const char *why = e == ERROR_WINHTTP_SECURE_FAILURE ? "certificate check failed"
                      : e == ERROR_WINHTTP_TIMEOUT ? "timed out"
                      : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "no network or unknown host (offline?)"
                      : e == ERROR_WINHTTP_CANNOT_CONNECT ? "cannot connect (offline?)"
                      : "network error";
    snprintf(b, sizeof b, "%s: %s (%lu)", what, why, e);
    return b;
}

// open the request and read the status; the caller reads the body
static bool open_request(const HttpRequest &rq, Conn *cn, int *status, std::string *ctype, std::string *retry,
                         std::string *err, const std::wstring &extra_header = L"")
{
    std::wstring url = wtext(rq.url);
    URL_COMPONENTS u = {sizeof u};
    wchar_t host[256], path[4096];
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 4096;
    wchar_t extra[4096];
    u.lpszExtraInfo = extra;
    u.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) { *err = "bad URL"; return false; }
    if (u.nScheme != INTERNET_SCHEME_HTTPS) { *err = "refused: only https:// downloads are allowed"; return false; }
    std::wstring h(host, u.dwHostNameLength), p = std::wstring(path, u.dwUrlPathLength) + std::wstring(extra, u.dwExtraInfoLength);
    rate_wait(h, rq.min_interval_s);
    cn->s = WinHttpOpen(wtext(http_user_agent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!cn->s) cn->s = WinHttpOpen(wtext(http_user_agent()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!cn->s) { *err = last_error_text("WinHttpOpen"); return false; }
    WinHttpSetTimeouts(cn->s, (int)rq.timeout_ms, (int)rq.timeout_ms, (int)rq.timeout_ms, (int)rq.timeout_ms);
    DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(cn->s, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
    DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    tls |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(cn->s, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
    cn->c = WinHttpConnect(cn->s, h.c_str(), u.nPort, 0);
    if (!cn->c) { *err = last_error_text("connect"); return false; }
    cn->r = WinHttpOpenRequest(cn->c, L"GET", p.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               WINHTTP_FLAG_SECURE);
    if (!cn->r) { *err = last_error_text("request"); return false; }
    // certificate checks stay on: no WINHTTP_OPTION_SECURITY_FLAGS that ignore errors are ever set
    std::wstring hdr;
    for (const auto &kv : rq.headers) hdr += wtext(kv.first) + L": " + wtext(kv.second) + L"\r\n";
    hdr += extra_header;
    if (!WinHttpSendRequest(cn->r, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : (DWORD)-1,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(cn->r, NULL)) {
        *err = last_error_text("request");
        return false;
    }
    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(cn->r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    *status = (int)st;
    wchar_t buf[512];
    sz = sizeof buf;
    if (ctype && WinHttpQueryHeaders(cn->r, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, buf, &sz, WINHTTP_NO_HEADER_INDEX))
        *ctype = utf8(std::wstring(buf, sz / sizeof(wchar_t)));
    sz = sizeof buf;
    if (retry && WinHttpQueryHeaders(cn->r, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, buf, &sz, WINHTTP_NO_HEADER_INDEX))
        *retry = utf8(std::wstring(buf, sz / sizeof(wchar_t)));
    return true;
}

static uint64_t content_length(HINTERNET r)
{
    wchar_t buf[64];
    DWORD sz = sizeof buf;
    if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &sz, WINHTTP_NO_HEADER_INDEX)) return 0;
    return _wcstoui64(buf, NULL, 10);
}

bool http_get(const HttpRequest &rq, HttpResponse *out)
{
    Conn cn;
    out->body.clear();
    out->error.clear();
    if (!open_request(rq, &cn, &out->status, &out->content_type, &out->retry_after, &out->error)) return false;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(cn.r, &avail)) { out->error = last_error_text("read"); return false; }
        if (!avail) break;
        if (out->body.size() + avail > rq.max_bytes) { out->error = "answer too large"; return false; }
        size_t at = out->body.size();
        out->body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(cn.r, &out->body[at], avail, &got)) { out->error = last_error_text("read"); return false; }
        out->body.resize(at + got);
    }
    return true;
}

bool http_download(const HttpRequest &rq, const std::string &dest, const Progress &p, std::string *err)
{
    uint64_t have = file_size(dest);
    Conn cn;
    int status = 0;
    std::wstring range;
    if (have) { wchar_t b[64]; swprintf(b, 64, L"Range: bytes=%llu-\r\n", (unsigned long long)have); range = b; }
    if (!open_request(rq, &cn, &status, NULL, NULL, err, range)) return false;
    if (status != 200 && status != 206) { *err = "download failed: HTTP " + std::to_string(status); return false; }
    bool append = status == 206 && have;                    // the server resumed; 200 = it sends everything again
    uint64_t total = content_length(cn.r) + (append ? have : 0), done = append ? have : 0;
    if (total > rq.max_bytes) { *err = "the file is larger than the download limit"; return false; }
    HANDLE f = CreateFileW(wide(dest).c_str(), GENERIC_WRITE, 0, NULL, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { *err = "cannot write " + dest; return false; }
    if (append) SetFilePointer(f, 0, NULL, FILE_END);
    std::vector<char> buf(256 * 1024);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(cn.r, buf.data(), (DWORD)buf.size(), &got)) { *err = last_error_text("download"); ok = false; break; }
        if (!got) break;
        DWORD put = 0;
        if (!WriteFile(f, buf.data(), got, &put, NULL) || put != got) { *err = "disk full or not writable"; ok = false; break; }
        done += got;
        if (done > rq.max_bytes) { *err = "the file is larger than the download limit"; ok = false; break; }
        if (p && !p(done, total)) { *err = "cancelled"; ok = false; break; }
    }
    CloseHandle(f);
    return ok;
}

static std::string key_file(const HttpCache &c, const std::string &key)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : key) h = (h ^ ch) * 1099511628211ull;
    char b[32];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return join(c.dir, b);
}

bool HttpCache::get(const std::string &key, int64_t max_age, std::string *body) const
{
    if (dir.empty()) return false;
    std::string f = key_file(*this, key);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(f).c_str(), GetFileExInfoStandard, &d)) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = now.dwLowDateTime; a.HighPart = now.dwHighDateTime;
    b.LowPart = d.ftLastWriteTime.dwLowDateTime; b.HighPart = d.ftLastWriteTime.dwHighDateTime;
    if (a.QuadPart < b.QuadPart || (int64_t)((a.QuadPart - b.QuadPart) / 10000000ull) > max_age) return false;
    return read_text(f, body);
}

void HttpCache::put(const std::string &key, const std::string &body) const
{
    if (dir.empty() || !make_dirs(dir)) return;
    write_text_atomic(key_file(*this, key), body);
}

bool http_get_cached(const HttpCache &c, const HttpRequest &r, int64_t max_age, HttpResponse *out)
{
    // the key is the URL only: headers (API keys) never go into a file name or the cache
    if (c.get(r.url, max_age, &out->body)) { out->status = 200; out->error.clear(); return true; }
    if (!http_get(r, out)) return false;
    if (out->status == 200) c.put(r.url, out->body);
    return true;
}

}  // namespace mb
