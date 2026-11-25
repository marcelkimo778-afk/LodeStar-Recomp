// mb_http.h - HTTPS GET for the mod browser (WinHTTP: system certificate validation, HTTPS only), a disk cache with
// expiry, and a per-host rate limiter.  Worker threads only.  No telemetry: the only requests are the ones a source
// makes for something the user asked for.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "mb_source.h"

namespace mb {

struct HttpResponse {
    int status = 0;
    std::string body;                         // get(): the body
    std::string content_type, retry_after;
    std::string error;                        // transport error ("" if a response came)
};

struct HttpRequest {
    std::string url;                          // https:// only (anything else is refused)
    std::vector<std::pair<std::string, std::string>> headers;   // never logged (they may hold API keys)
    uint64_t max_bytes = 32u << 20;           // body limit
    unsigned timeout_ms = 30000;
    double min_interval_s = 0.5;              // per host: at most one request every this many seconds
};

const char *http_user_agent();                // "LodeStar-ModBrowser/1.0"
bool http_get(const HttpRequest &r, HttpResponse *out);
// body straight into a file (downloads): progress + cancel, size limit; resumes a partial file with Range if the
// server allows it
bool http_download(const HttpRequest &r, const std::string &dest, const Progress &p, std::string *err);

// cache: key -> file under dir, valid for max_age seconds
struct HttpCache {
    std::string dir;
    bool get(const std::string &key, int64_t max_age, std::string *body) const;
    void put(const std::string &key, const std::string &body) const;
};
// GET with the cache (JSON answers, thumbnails); status 200 only is cached
bool http_get_cached(const HttpCache &c, const HttpRequest &r, int64_t max_age, HttpResponse *out);

}  // namespace mb
