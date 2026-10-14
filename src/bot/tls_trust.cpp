#include "bot/tls_trust.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/utf8_path.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

namespace strojanka {

namespace {

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

#if defined(_WIN32)
size_t append_store(const wchar_t* store_name, DWORD location, std::string& pem) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, location | CERT_STORE_READONLY_FLAG, store_name);
    if (!store) return 0;
    size_t count = 0;
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
        DWORD len = 0;
        if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &len)) continue;
        std::vector<char> buf(len);
        if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER, buf.data(), &len)) continue;
        pem.append(buf.data(), len);
        ++count;
    }
    CertCloseStore(store, 0);
    return count;
}
#endif

}  // namespace

std::string configure_tls_trust(const std::string& pem_path) {
    if (const char* existing = std::getenv("SSL_CERT_FILE"); existing && *existing) {
        return std::string("using SSL_CERT_FILE=") + existing;
    }
#if defined(_WIN32)
    std::string pem;
    size_t count = append_store(L"ROOT", CERT_SYSTEM_STORE_LOCAL_MACHINE, pem);
    count += append_store(L"ROOT", CERT_SYSTEM_STORE_CURRENT_USER, pem);
    if (count == 0) return "could not read the Windows root certificate store; Discord connections may fail";
    const auto path = utf8_path(pem_path);
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << pem;
    if (!out) return "could not write " + pem_path + "; Discord connections may fail";
    out.close();
    set_env("SSL_CERT_FILE", pem_path);
    return "trusting " + std::to_string(count) + " Windows root certificates";
#elif defined(__APPLE__)
    (void)pem_path;
    if (std::filesystem::exists("/etc/ssl/cert.pem")) {
        set_env("SSL_CERT_FILE", "/etc/ssl/cert.pem");
        return "trusting macOS root certificates from /etc/ssl/cert.pem";
    }
    return "/etc/ssl/cert.pem not found; Discord connections may fail";
#else
    (void)pem_path;
    return "using OpenSSL default certificate paths";
#endif
}

}  // namespace strojanka
