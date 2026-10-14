#pragma once

#include <string>

namespace strojanka {

// D++ verifies TLS certificates with OpenSSL, which (as built by vcpkg) does not know the
// operating system's trusted roots. Unless SSL_CERT_FILE is already set, this points OpenSSL
// at a root bundle: on Windows the system root store exported to `pem_path`, on macOS
// /etc/ssl/cert.pem. Must run before the dpp::cluster is created. Returns a log message.
std::string configure_tls_trust(const std::string& pem_path);

}  // namespace strojanka
