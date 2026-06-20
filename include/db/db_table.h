#pragma once

#include <functional>
#include <mutex>
#include <string>

#include <sqlite3.h>

namespace cortexlink {

// Abstract base class for all table operations.
// Holds a static sqlite3* connection and a static mutex to serialize
// all database accesses (reads and writes share the same connection).
class DBTable {
public:
    // Open/create the database at the given path.
    // Creates parent directories if needed.  Call once at startup.
    static bool Initialize(const std::string &db_path);

    // Close the database connection.
    static void Shutdown();

    // Return the shared database handle.
    static sqlite3 *GetDB();

    DBTable() = default;
    virtual ~DBTable() = default;

    // Create the table schema (CREATE TABLE IF NOT EXISTS).
    // Called after Initialize() to ensure all tables exist.
    virtual bool CreateTable() = 0;

protected:
    // Execute a write SQL statement (INSERT / UPDATE / DELETE).
    // Acquires write_mutex_ before executing.
    bool ExecuteWrite(const std::string &sql);

    // Execute a write SQL statement with parameter binding.
    // The bind_fn receives the prepared statement and should bind
    // parameters via sqlite3_bind_*.
    bool ExecuteWrite(const std::string &sql,
                      const std::function<int(sqlite3_stmt *)> &bind_fn);

    // Execute a read SQL statement (SELECT) without a result callback.
    // Acquires write_mutex_ for serialized access to the shared connection.
    bool ExecuteRead(const std::string &sql);

    // Execute a read SQL statement (SELECT) with parameter binding and
    // a row callback.  The row_fn is called once per result row.
    bool ExecuteRead(const std::string &sql,
                     const std::function<int(sqlite3_stmt *)> &bind_fn,
                     const std::function<void(sqlite3_stmt *)> &row_fn);

    static sqlite3 *db_;
    static std::mutex write_mutex_;
};

}  // namespace cortexlink
