#pragma once
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include "json.hpp"

// Forward declaration to avoid including sqlite3.h everywhere
struct sqlite3;
struct sqlite3_stmt;

using json = nlohmann::json;

// Lightweight SQLite3 wrapper
class Database {
public:
    Database();
    ~Database();

    // Open or create a database file
    bool open(const std::string &path);

    // Close the database
    void close();

    // Execute a SQL statement (no results)
    bool exec(const std::string &sql);

    // Execute a query and return rows as JSON array
    json query(const std::string &sql);

    // Execute a query with parameters and return rows as JSON array
    json query(const std::string &sql, const json &params);

    // Execute a statement with parameters (INSERT/UPDATE/DELETE)
    bool execute(const std::string &sql, const json &params);

    // Get last inserted row ID
    int64_t lastInsertRowId() const;

    // Get number of changes from last statement
    int changes() const;

    // Check if database is open
    bool isOpen() const { return m_db != nullptr; }

    // Get raw sqlite3 handle (for advanced usage)
    sqlite3 *handle() { return m_db; }

private:
    sqlite3 *m_db = nullptr;
    mutable std::mutex m_dbMutex;
};
