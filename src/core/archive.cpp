#include "core/archive.h"

#include <zlib.h>

#include <cstdint>

#include "common/strings.h"

namespace strojanka {

namespace {

uint32_t le32(const std::string& s, size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[at])) | static_cast<uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(s[at + 2])) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24;
}

uint16_t le16(const std::string& s, size_t at) {
    return static_cast<uint16_t>(static_cast<unsigned char>(s[at]) | static_cast<unsigned char>(s[at + 1]) << 8);
}

}  // namespace

std::optional<std::string> inflate_data(const std::string& compressed, bool raw) {
    z_stream zs{};
    if (inflateInit2(&zs, raw ? -MAX_WBITS : MAX_WBITS) != Z_OK) return std::nullopt;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());
    std::string out;
    char buf[65536];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        out.append(buf, sizeof buf - zs.avail_out);
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;  // input ended without a final block
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && !(rc == Z_BUF_ERROR && !out.empty())) return std::nullopt;
    return out;
}

std::optional<std::string> zip_entry(const std::string& zip, const std::string& name_suffix, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<std::string> {
        if (error) *error = why;
        return std::nullopt;
    };
    // End of central directory: 22 bytes plus an optional comment of up to 64 KB at the end.
    if (zip.size() < 22) return fail("not a ZIP file (too short)");
    size_t eocd = std::string::npos;
    const size_t lowest = zip.size() > 22 + 65535 ? zip.size() - 22 - 65535 : 0;
    for (size_t i = zip.size() - 22 + 1; i-- > lowest;) {
        if (le32(zip, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return fail("not a ZIP file (no central directory)");
    const uint16_t entries = le16(zip, eocd + 10);
    size_t p = le32(zip, eocd + 16);
    const std::string want = to_lower(name_suffix);
    for (uint16_t e = 0; e < entries; ++e) {
        if (p + 46 > zip.size() || le32(zip, p) != 0x02014b50) return fail("damaged ZIP central directory");
        const uint16_t method = le16(zip, p + 10);
        const uint32_t comp_size = le32(zip, p + 20);
        const uint16_t name_len = le16(zip, p + 28), extra_len = le16(zip, p + 30), comment_len = le16(zip, p + 32);
        const uint32_t local = le32(zip, p + 42);
        if (p + 46 + name_len > zip.size()) return fail("damaged ZIP central directory");
        const std::string name = zip.substr(p + 46, name_len);
        p += 46 + name_len + extra_len + comment_len;
        const std::string lname = to_lower(name);
        if (lname.size() < want.size() || lname.compare(lname.size() - want.size(), want.size(), want) != 0) continue;

        if (static_cast<size_t>(local) + 30 > zip.size() || le32(zip, local) != 0x04034b50) return fail("damaged ZIP entry " + name);
        const size_t data = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
        if (data + comp_size > zip.size()) return fail("truncated ZIP entry " + name);
        const std::string payload = zip.substr(data, comp_size);
        if (method == 0) return payload;
        if (method != 8) return fail("unsupported ZIP compression in " + name);
        auto out = inflate_data(payload, true);
        if (!out) return fail("could not decompress " + name);
        return out;
    }
    return fail("no file ending in " + name_suffix + " in the ZIP");
}

}  // namespace strojanka
