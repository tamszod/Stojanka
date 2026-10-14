#pragma once

#include <string>
#include <vector>

#include "core/database.h"

namespace strojanka {

// A schema change. Migrations are applied in order and never edited once released:
// to change the schema, append a new migration with the next version number.
struct Migration {
    int version;
    const char* description;
    const char* sql;
};

const std::vector<Migration>& all_migrations();
int latest_schema_version();

struct MigrationReport {
    int from_version = 0;
    int to_version = 0;
    std::string backup_path;  // set when an existing database was backed up before migrating
    std::vector<std::string> applied;
};

// Brings the database up to date. An existing, non-empty database is copied to
// "<db>.backup-v<old>-<timestamp>" first (pass make_backup=false to skip, e.g. in tests).
// Throws DatabaseError if the database comes from a newer Strojanka version.
MigrationReport migrate_database(Database& db, bool make_backup = true);

}  // namespace strojanka
