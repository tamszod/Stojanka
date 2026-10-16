#pragma once

#include <optional>
#include <string>

// Just enough decompression for the public data Strojanka reads (zlib):
// PDF FlateDecode streams and the House disclosure index ZIP.
namespace strojanka {

// zlib (raw = false: with the 2-byte header PDFs use) or raw deflate data. nullopt if corrupt.
std::optional<std::string> inflate_data(const std::string& compressed, bool raw = false);

// The first file in a ZIP archive whose name ends with `name_suffix` (case-insensitive), e.g.
// "FD.txt". Supports stored and deflated entries; no ZIP64 or encryption.
std::optional<std::string> zip_entry(const std::string& zip, const std::string& name_suffix, std::string* error);

}  // namespace strojanka
