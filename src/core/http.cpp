#include "core/http.h"

#include <curl/curl.h>

#include <mutex>

namespace strojanka {

namespace {

std::once_flag g_curl_init;

void init_curl() {
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

HttpResponse http_get(const std::string& url) {
    init_curl();
    HttpResponse r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "curl init failed";
        return r;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; Strojanka)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        r.error = std::string("network error: ") + curl_easy_strerror(rc);
        return r;
    }
    r.ok = true;
    return r;
}

std::string url_encode(const std::string& value) {
    init_curl();
    CURL* curl = curl_easy_init();
    if (!curl) return value;
    char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string out = escaped ? escaped : value;
    curl_free(escaped);
    curl_easy_cleanup(curl);
    return out;
}

}  // namespace strojanka
