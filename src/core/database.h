#pragma once

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace strojanka {

class DatabaseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql);
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&&) = delete;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // Parameters are 1-based, like SQLite.
    Statement& bind(int index, const std::string& value);
    Statement& bind(int index, const char* value);
    Statement& bind(int index, double value);
    Statement& bind(int index, int64_t value);
    Statement& bind(int index, int value);
    Statement& bind_null(int index);

    bool step();  // true while a row is available
    void reset();

    // Columns are 0-based, like SQLite.
    std::string text(int column) const;
    double real(int column) const;
    int64_t integer(int column) const;
    bool is_null(int column) const;

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

// One SQLite connection shared by the whole process. Every access must hold `mutex()`
// (the Repository does this); SQLite work is short, so a single lock keeps things simple.
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const std::string& sql);
    Statement prepare(const std::string& sql);
    int64_t last_insert_id() const;
    int changes() const;
    int user_version();
    void set_user_version(int version);
    std::recursive_mutex& mutex() { return mutex_; }
    const std::string& path() const { return path_; }
    uint64_t file_size() const;  // database file + write-ahead log, in bytes

    // Moves the database to `new_path` while it is in use: copies it (VACUUM INTO), checks the copy's
    // integrity, switches this connection to it and deletes the old files. On any failure the old
    // file stays in use and DatabaseError is thrown. Callers must not hold Statements across this.
    void move_to(const std::string& new_path);

private:
    sqlite3* db_ = nullptr;
    std::string path_;
    std::recursive_mutex mutex_;
};

// BEGIN IMMEDIATE ... COMMIT, rolled back automatically unless commit() is called.
class SqlTransaction {
public:
    explicit SqlTransaction(Database& db);
    ~SqlTransaction();
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

}  // namespace strojanka
