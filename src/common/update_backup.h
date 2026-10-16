#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The previous version's program files, kept for a few days after an update so it can be
// reverted. Written by the installer, cleaned up by the installer and the bot once it expires.
// Stored as <install dir>/previous/ with revert.json describing it.
namespace strojanka {

inline constexpr const char* kUpdateBackupDir = "previous";
inline constexpr int kRevertDays = 3;

struct UpdateBackup {
    std::string version;                   // the version that was replaced
    std::string created_at, expires_at;    // ISO UTC
    std::vector<std::string> files;        // paths relative to the install dir, saved under previous/
    std::vector<std::string> added_files;  // files the update added, removed again on revert
    std::string database_path;             // absolute
    std::string database_backup;           // copy the update's database upgrade made; "" = schema unchanged
};

std::optional<UpdateBackup> read_update_backup(const std::string& install_dir);
bool write_update_backup(const std::string& install_dir, const UpdateBackup& backup, std::string* error);
bool update_backup_expired(const UpdateBackup& backup, int64_t now_unix);
void remove_update_backup(const std::string& install_dir);
// Deletes the backup once its days are over; returns the version it held, or "" if nothing was removed.
std::string remove_expired_update_backup(const std::string& install_dir, int64_t now_unix);

}  // namespace strojanka
