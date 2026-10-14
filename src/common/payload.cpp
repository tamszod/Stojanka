#include "common/payload.h"
#include "common/utf8_path.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace strojanka::payload {

namespace {

template <typename T>
void write_le(std::ostream& out, T v) {
    unsigned char buf[sizeof(T)];
    for (size_t i = 0; i < sizeof(T); ++i) buf[i] = static_cast<unsigned char>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF);
    out.write(reinterpret_cast<const char*>(buf), sizeof(T));
}

template <typename T>
bool read_le(std::istream& in, T& v) {
    unsigned char buf[sizeof(T)];
    if (!in.read(reinterpret_cast<char*>(buf), sizeof(T))) return false;
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof(T); ++i) r |= static_cast<uint64_t>(buf[i]) << (8 * i);
    v = static_cast<T>(r);
    return true;
}

bool copy_stream(std::istream& in, std::ostream& out, uint64_t size, uint64_t* hash) {
    std::vector<char> buf(1 << 16);
    uint64_t h = 1469598103934665603ULL;
    while (size > 0) {
        const auto chunk = static_cast<std::streamsize>(std::min<uint64_t>(size, buf.size()));
        if (!in.read(buf.data(), chunk)) return false;
        h = fnv1a64(buf.data(), static_cast<size_t>(chunk), h);
        out.write(buf.data(), chunk);
        size -= static_cast<uint64_t>(chunk);
    }
    if (hash) *hash = h;
    return static_cast<bool>(out);
}

bool hash_path(const fs::path& p, uint64_t& hash) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::vector<char> buf(1 << 16);
    uint64_t h = 1469598103934665603ULL;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        h = fnv1a64(buf.data(), static_cast<size_t>(in.gcount()), h);
    }
    hash = h;
    return true;
}

bool is_executable(const fs::path& p) {
    const auto ext = p.extension().string();
    if (ext == ".exe" || ext == ".dll" || ext == ".dylib" || ext == ".so" || ext == ".command" || ext == ".sh") return true;
#ifndef _WIN32
    std::error_code ec;
    const auto perms = fs::status(p, ec).permissions();
    if (!ec && (perms & fs::perms::owner_exec) != fs::perms::none) return true;
#endif
    return false;
}

}  // namespace

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool hash_file(const std::string& path, uint64_t& hash) { return hash_path(utf8_path(path), hash); }

bool pack(const std::string& stub, const std::string& source_dir, const std::string& output, std::string* error) {
    const fs::path src = utf8_path(source_dir);
    if (!fs::is_directory(src)) {
        if (error) *error = "source folder does not exist: " + source_dir;
        return false;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(src)) {
        if (e.is_regular_file()) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    std::error_code ec;
    const fs::path out_path = utf8_path(output);
    if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    std::ifstream stub_in(utf8_path(stub), std::ios::binary);
    if (!out || !stub_in) {
        if (error) *error = "cannot open stub or output file";
        return false;
    }
    out << stub_in.rdbuf();
    const uint64_t payload_offset = static_cast<uint64_t>(out.tellp());

    for (const auto& f : files) {
        const std::string rel = path_utf8_generic(fs::relative(f, src));
        uint64_t hash = 0;
        if (!hash_path(f, hash)) {
            if (error) *error = "cannot read " + path_utf8(f);
            return false;
        }
        const uint64_t size = fs::file_size(f);
        write_le<uint32_t>(out, static_cast<uint32_t>(rel.size()));
        out.write(rel.data(), static_cast<std::streamsize>(rel.size()));
        write_le<uint32_t>(out, is_executable(f) ? kFlagExecutable : 0u);
        write_le<uint64_t>(out, size);
        write_le<uint64_t>(out, hash);
        std::ifstream in(f, std::ios::binary);
        if (!copy_stream(in, out, size, nullptr)) {
            if (error) *error = "failed while copying " + path_utf8(f);
            return false;
        }
    }
    write_le<uint64_t>(out, payload_offset);
    write_le<uint64_t>(out, static_cast<uint64_t>(files.size()));
    out.write(kMagic, sizeof kMagic);
    out.close();
    if (!out) {
        if (error) *error = "failed writing " + output;
        return false;
    }
#ifndef _WIN32
    fs::permissions(out_path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
    return true;
}

bool read_index(const std::string& container, std::vector<Entry>& entries, std::string* error) {
    std::ifstream in(utf8_path(container), std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + container;
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    if (file_size < 24) {
        if (error) *error = "no payload found";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(file_size - 24));
    uint64_t offset = 0, count = 0;
    char magic[8];
    read_le(in, offset);
    read_le(in, count);
    in.read(magic, 8);
    if (!in || std::memcmp(magic, kMagic, 8) != 0 || offset >= file_size) {
        if (error) *error = "no payload found (this installer was not packed)";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(offset));
    entries.clear();
    for (uint64_t i = 0; i < count; ++i) {
        Entry e;
        uint32_t len = 0;
        if (!read_le(in, len) || len > 4096) {
            if (error) *error = "corrupt payload index";
            return false;
        }
        e.path.resize(len);
        in.read(e.path.data(), len);
        if (!read_le(in, e.flags) || !read_le(in, e.size) || !read_le(in, e.hash)) {
            if (error) *error = "corrupt payload index";
            return false;
        }
        e.data_offset = static_cast<uint64_t>(in.tellg());
        if (e.data_offset + e.size > file_size) {
            if (error) *error = "corrupt payload: entry beyond end of file";
            return false;
        }
        in.seekg(static_cast<std::streamoff>(e.data_offset + e.size));
        entries.push_back(std::move(e));
    }
    return true;
}

bool extract(const std::string& container, const Entry& entry, const std::string& destination, std::string* error) {
    if (entry.path.find("..") != std::string::npos) {
        if (error) *error = "refusing unsafe path " + entry.path;
        return false;
    }
    std::ifstream in(utf8_path(container), std::ios::binary);
    in.seekg(static_cast<std::streamoff>(entry.data_offset));
    const fs::path dest = utf8_path(destination);
    std::error_code ec;
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
    fs::path tmp = dest;
    tmp += ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + path_utf8(tmp) + " (is the program still running?)";
            return false;
        }
        uint64_t hash = 0;
        if (!copy_stream(in, out, entry.size, &hash)) {
            if (error) *error = "read error while extracting " + entry.path;
            return false;
        }
        if (hash != entry.hash) {
            if (error) *error = "checksum mismatch for " + entry.path + " (installer is damaged)";
            return false;
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
        if (ec) {
            fs::remove(tmp, ec);
            if (error) *error = "cannot replace " + path_utf8(dest) + " (is Strojanka still running?)";
            return false;
        }
    }
#ifndef _WIN32
    if (entry.flags & kFlagExecutable) {
        fs::permissions(dest, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    }
#endif
    return true;
}

}  // namespace strojanka::payload
