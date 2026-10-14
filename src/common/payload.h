#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Installer payload format. The packer appends program files to the installer executable:
//
//   [installer stub][entry 1]...[entry N][footer]
//   entry  = u32 path_len | path (UTF-8, '/' separated) | u32 flags | u64 size | u64 fnv1a64 | data
//   footer = u64 payload_offset | u64 entry_count | 8 bytes magic "STRJPAK1"
//
// All integers are little-endian. flags bit 0 = executable.
namespace strojanka::payload {

constexpr char kMagic[8] = {'S', 'T', 'R', 'J', 'P', 'A', 'K', '1'};
constexpr uint32_t kFlagExecutable = 1;

struct Entry {
    std::string path;
    uint32_t flags = 0;
    uint64_t size = 0;
    uint64_t hash = 0;
    uint64_t data_offset = 0;  // absolute offset of the data inside the container file
};

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL);

// Hash of a file on disk, comparable with Entry::hash. False if the file cannot be read.
bool hash_file(const std::string& path, uint64_t& hash);

// Creates `output` = `stub` + every regular file under `source_dir` (recursively).
bool pack(const std::string& stub, const std::string& source_dir, const std::string& output, std::string* error);

// Reads the entry table from a packed file. Returns false if the file has no payload.
bool read_index(const std::string& container, std::vector<Entry>& entries, std::string* error);

// Extracts one entry to `destination` (parent folders are created), verifying its hash.
bool extract(const std::string& container, const Entry& entry, const std::string& destination, std::string* error);

}  // namespace strojanka::payload
