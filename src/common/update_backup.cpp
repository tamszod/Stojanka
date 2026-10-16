#include "common/update_backup.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "common/strings.h"
#include "common/utf8_path.h"

namespace fs = std::filesystem;

namespace strojanka {

namespace {

fs::path manifest_path(const std::string& install_dir) { return utf8_path(install_dir) / kUpdateBackupDir / "revert.json"; }

std::vector<std::string> strings_of(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key) || !j[key].is_array()) return out;
    for (const auto& v : j[key]) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::string text_of(const nlohmann::json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

}  // namespace

std::optional<UpdateBackup> read_update_backup(const std::string& install_dir) {
    std::ifstream f(manifest_path(install_dir));
    if (!f) return std::nullopt;
    const auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    UpdateBackup b;
    b.version = text_of(j, "version");
    b.created_at = text_of(j, "created_at");
    b.expires_at = text_of(j, "expires_at");
    b.files = strings_of(j, "files");
    b.added_files = strings_of(j, "added_files");
    b.database_path = text_of(j, "database_path");
    b.database_backup = text_of(j, "database_backup");
    if (b.version.empty() || b.expires_at.empty() || b.files.empty()) return std::nullopt;
    return b;
}

bool write_update_backup(const std::string& install_dir, const UpdateBackup& b, std::string* error) {
    const nlohmann::json j = {{"version", b.version},
                              {"created_at", b.created_at},
                              {"expires_at", b.expires_at},
                              {"files", b.files},
                              {"added_files", b.added_files},
                              {"database_path", b.database_path},
                              {"database_backup", b.database_backup}};
    std::error_code ec;
    fs::create_directories(manifest_path(install_dir).parent_path(), ec);
    std::ofstream f(manifest_path(install_dir), std::ios::trunc);
    if (!(f << j.dump(2))) {
        if (error) *error = "could not write " + path_utf8(manifest_path(install_dir));
        return false;
    }
    return true;
}

bool update_backup_expired(const UpdateBackup& b, int64_t now_unix) { return now_unix >= iso_to_unix(b.expires_at); }

void remove_update_backup(const std::string& install_dir) {
    std::error_code ec;
    fs::remove_all(utf8_path(install_dir) / kUpdateBackupDir, ec);
}

std::string remove_expired_update_backup(const std::string& install_dir, int64_t now_unix) {
    const auto b = read_update_backup(install_dir);
    if (!b || !update_backup_expired(*b, now_unix)) return {};
    remove_update_backup(install_dir);
    return b->version;
}

}  // namespace strojanka
