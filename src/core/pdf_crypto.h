#pragma once

#include <string>

// The two primitives the PDF "standard security handler" (revisions 2-3) needs, to read House
// trade reports that are RC4-encrypted with an empty user password (permissions only). Not for
// anything security-relevant: both algorithms are long broken, which is fine for that.
namespace strojanka {

std::string md5(const std::string& data);  // 16 raw bytes (RFC 1321)
std::string rc4(const std::string& key, const std::string& data);

}  // namespace strojanka
