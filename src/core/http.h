#pragma once

#include <string>

namespace strojanka {

struct HttpResponse {
    bool ok = false;      // the request completed (any HTTP status)
    long status = 0;
    std::string body;
    std::string error;    // network error text when !ok
};

// Blocking HTTPS GET with libcurl. Thread-safe; follows redirects; 30 s timeout.
HttpResponse http_get(const std::string& url);

// Percent-encodes a value for use in a URL query or path.
std::string url_encode(const std::string& value);

}  // namespace strojanka
