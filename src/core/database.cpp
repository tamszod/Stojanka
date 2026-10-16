#include "core/database.h"
#include "common/utf8_path.h"

#include <sqlite3.h>

#include <filesystem>

namespace strojanka {

Statement::Statement(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK) {
        throw DatabaseError(std::string("prepare failed: ") + sqlite3_errmsg(db_) + " in: " + sql);
    }
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) { other.stmt_ = nullptr; }

static void check_bind(sqlite3* db, int rc) {
    if (rc != SQLITE_OK) throw DatabaseError(std::string("bind failed: ") + sqlite3_errmsg(db));
}

Statement& Statement::bind(int index, const std::string& value) {
    check_bind(db_, sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::bind(int index, const char* value) { return bind(index, std::string(value)); }

Statement& Statement::bind(int index, double value) {
    check_bind(db_, sqlite3_bind_double(stmt_, index, value));
    return *this;
}

Statement& Statement::bind(int index, int64_t value) {
    check_bind(db_, sqlite3_bind_int64(stmt_, index, value));
    return *this;
}

Statement& Statement::bind(int index, int value) { return bind(index, static_cast<int64_t>(value)); }

Statement& Statement::bind_null(int index) {
    check_bind(db_, sqlite3_bind_null(stmt_, index));
    return *this;
}

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DatabaseError(std::string("step failed: ") + sqlite3_errmsg(db_));
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

std::string Statement::text(int column) const {
    const auto* p = sqlite3_column_text(stmt_, column);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(sqlite3_column_bytes(stmt_, column)))
             : std::string();
}

double Statement::real(int column) const { return sqlite3_column_double(stmt_, column); }

int64_t Statement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }

bool Statement::is_null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

namespace {

sqlite3* open_handle(const std::string& path) {
    if (path != ":memory:") {
        const auto p = utf8_path(path);
        std::error_code ec;
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw DatabaseError("cannot open database " + path + ": " + msg);
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    if (path != ":memory:") sqlite3_exec(db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    return db;
}

void remove_db_files(const std::string& path) {
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(utf8_path(path + suffix), ec);
}

}  // namespace

Database::Database(const std::string& path) : db_(open_handle(path)), path_(path) {}

uint64_t Database::file_size() const {
    if (path_ == ":memory:") return 0;
    uint64_t total = 0;
    std::error_code ec;
    for (const char* suffix : {"", "-wal"}) {
        const auto size = std::filesystem::file_size(utf8_path(path_ + suffix), ec);
        if (!ec) total += size;
    }
    return total;
}

void Database::backup_to(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto target = utf8_path(path);
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) throw DatabaseError("a file already exists at " + path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) throw DatabaseError("cannot create folder " + path_utf8(target.parent_path()) + ": " + ec.message());
    {
        auto st = prepare("VACUUM INTO ?1;");  // a consistent snapshot even while the bot keeps writing
        st.bind(1, path);
        st.step();
    }
    // Checked read-only, so opening it leaves no journal or WAL files beside the backup.
    sqlite3* copy = nullptr;
    std::string result;
    if (sqlite3_open_v2(path.c_str(), &copy, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* check = nullptr;
        if (sqlite3_prepare_v2(copy, "PRAGMA integrity_check;", -1, &check, nullptr) == SQLITE_OK && sqlite3_step(check) == SQLITE_ROW) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(check, 0));
        }
        sqlite3_finalize(check);
    }
    if (copy) sqlite3_close(copy);
    if (result != "ok") {
        remove_db_files(path);
        throw DatabaseError("the backup failed its integrity check (" + (result.empty() ? std::string("unreadable") : result) + ")");
    }
}

void Database::move_to(const std::string& new_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (path_ == ":memory:") throw DatabaseError("an in-memory database cannot be moved");
    const auto target = utf8_path(new_path);
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) throw DatabaseError("a file already exists at " + new_path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) throw DatabaseError("cannot create folder " + path_utf8(target.parent_path()) + ": " + ec.message());

    // A consistent copy, even while other connections or WAL frames exist.
    {
        auto st = prepare("VACUUM INTO ?1;");
        st.bind(1, new_path);
        st.step();
    }
    sqlite3* fresh = nullptr;
    try {
        fresh = open_handle(new_path);
        sqlite3_stmt* check = nullptr;
        std::string result;
        if (sqlite3_prepare_v2(fresh, "PRAGMA integrity_check;", -1, &check, nullptr) == SQLITE_OK &&
            sqlite3_step(check) == SQLITE_ROW) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(check, 0));
        }
        sqlite3_finalize(check);
        if (result != "ok") throw DatabaseError("the copy failed its integrity check (" + result + ")");
    } catch (...) {
        if (fresh) sqlite3_close(fresh);
        remove_db_files(new_path);
        throw;
    }

    const std::string old_path = path_;
    sqlite3_close(db_);
    db_ = fresh;
    path_ = new_path;
    remove_db_files(old_path);
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DatabaseError("exec failed: " + msg);
    }
}

Statement Database::prepare(const std::string& sql) { return Statement(db_, sql); }

int64_t Database::last_insert_id() const { return sqlite3_last_insert_rowid(db_); }

int Database::changes() const { return sqlite3_changes(db_); }

int Database::user_version() {
    auto st = prepare("PRAGMA user_version;");
    return st.step() ? static_cast<int>(st.integer(0)) : 0;
}

void Database::set_user_version(int version) { exec("PRAGMA user_version = " + std::to_string(version) + ";"); }

SqlTransaction::SqlTransaction(Database& db) : db_(db) {
    db_.mutex().lock();
    try {
        db_.exec("BEGIN IMMEDIATE;");
    } catch (...) {
        db_.mutex().unlock();
        throw;
    }
}

SqlTransaction::~SqlTransaction() {
    if (!done_) {
        try {
            db_.exec("ROLLBACK;");
        } catch (...) {
        }
    }
    db_.mutex().unlock();
}

void SqlTransaction::commit() {
    db_.exec("COMMIT;");
    done_ = true;
}

}  // namespace strojanka
