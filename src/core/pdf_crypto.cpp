#include "core/pdf_crypto.h"

#include <cmath>
#include <cstdint>
#include <utility>

namespace strojanka {

std::string md5(const std::string& data) {
    static const uint32_t kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    // K[i] = floor(|sin(i + 1)| * 2^32), as RFC 1321 defines it (checked by the test vectors).
    static const auto kK = [] {
        struct Table {
            uint32_t v[64];
        } t{};
        for (int i = 0; i < 64; ++i) t.v[i] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0));
        return t;
    }();
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::string msg = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = 0;
            for (int b = 3; b >= 0; --b) m[i] = m[i] << 8 | static_cast<unsigned char>(msg[off + 4 * i + b]);
        }
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            f += a + kK.v[i] + m[g];
            a = d;
            d = c;
            c = b;
            b += (f << kShift[i]) | (f >> (32 - kShift[i]));
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }
    std::string out;
    for (uint32_t w : {a0, b0, c0, d0}) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((w >> (8 * i)) & 0xFF));
    }
    return out;
}

std::string rc4(const std::string& key, const std::string& data) {
    unsigned char s[256];
    for (int i = 0; i < 256; ++i) s[i] = static_cast<unsigned char>(i);
    if (key.empty()) return data;
    for (int i = 0, j = 0; i < 256; ++i) {
        j = (j + s[i] + static_cast<unsigned char>(key[i % key.size()])) & 0xFF;
        std::swap(s[i], s[j]);
    }
    std::string out(data.size(), '\0');
    for (size_t n = 0, i = 0, j = 0; n < data.size(); ++n) {
        i = (i + 1) & 0xFF;
        j = (j + s[i]) & 0xFF;
        std::swap(s[i], s[j]);
        out[n] = static_cast<char>(static_cast<unsigned char>(data[n]) ^ s[(s[i] + s[j]) & 0xFF]);
    }
    return out;
}

}  // namespace strojanka
