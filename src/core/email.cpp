#include "core/email.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>

#include "common/strings.h"

namespace strojanka {

namespace {

struct Payload {
    std::string data;
    size_t sent = 0;
};

size_t read_payload(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* p = static_cast<Payload*>(userdata);
    const size_t room = size * nitems;
    const size_t n = std::min(room, p->data.size() - p->sent);
    std::memcpy(buffer, p->data.data() + p->sent, n);
    p->sent += n;
    return n;
}

std::string rfc5322_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    static const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d +0000", kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon],
                  tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string crlf(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\n') out += "\r\n";
        else if (c != '\r') out.push_back(c);
    }
    return out;
}

}  // namespace

std::string guess_smtp_server(const std::string& address) {
    const auto at = address.rfind('@');
    if (at == std::string::npos) return {};
    const std::string domain = to_lower(trim(address.substr(at + 1)));
    if (domain == "gmail.com" || domain == "googlemail.com") return "smtps://smtp.gmail.com:465";
    if (domain == "outlook.com" || domain == "hotmail.com" || domain == "live.com" || domain == "msn.com") return "smtp://smtp-mail.outlook.com:587";
    if (domain == "yahoo.com") return "smtps://smtp.mail.yahoo.com:465";
    if (domain == "icloud.com" || domain == "me.com" || domain == "mac.com") return "smtp://smtp.mail.me.com:587";
    return {};
}

bool send_email(const Config& config, const std::string& subject, const std::string& body, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string to = trim(config.backup_email);
    if (to.empty() || to.find('@') == std::string::npos) return fail("no email address is set (backup_email)");
    const std::string server = trim(config.smtp_server).empty() ? guess_smtp_server(to) : trim(config.smtp_server);
    if (server.empty()) return fail("the mail server for " + to + " isn't known; set smtp_server in strojanka.conf");
    if (config.smtp_password.empty()) return fail("no mail password is set (smtp_password; Gmail needs an app password)");

    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) return fail("curl init failed");
    Payload payload;
    payload.data = crlf("Date: " + rfc5322_now() + "\nTo: <" + to + ">\nFrom: Strojanka <" + to + ">\nSubject: " + subject +
                        "\nMIME-Version: 1.0\nContent-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n\n" + body + "\n");
    curl_slist* rcpt = curl_slist_append(nullptr, ("<" + to + ">").c_str());
    const std::string from = "<" + to + ">";
    const std::string user = trim(config.smtp_user).empty() ? to : trim(config.smtp_user);
    curl_easy_setopt(curl, CURLOPT_URL, server.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, config.smtp_password.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_payload);
    curl_easy_setopt(curl, CURLOPT_READDATA, &payload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // smtp:// must upgrade to TLS (STARTTLS) before the password is sent; only a local test server may stay plain.
    const bool local = server.find("://127.0.0.1") != std::string::npos || server.find("://localhost") != std::string::npos;
    curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(local ? CURLUSESSL_NONE : CURLUSESSL_ALL));
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        std::string why = curl_easy_strerror(rc);
        if (rc == CURLE_LOGIN_DENIED) why += " (wrong password? Gmail needs an app password, not your normal one)";
        return fail("could not send email via " + server + ": " + why);
    }
    return true;
}

}  // namespace strojanka
