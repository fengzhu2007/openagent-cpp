#include "database/database.h"
#include "util/logger.h"
#include <sqlite3.h>
#include <stdexcept>

Database::Database() = default;

Database::~Database()
{
    close();
}

bool Database::open(const std::string &path)
{
    int rc = sqlite3_open(path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQLite open failed: " + std::string(sqlite3_errmsg(m_db)));
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    // Enable WAL mode for better concurrent performance
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");

    LOG_DEBUG("SQLite opened: " + path);
    return true;
}

void Database::close()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool Database::exec(const std::string &sql)
{
    if (!m_db) return false;
    std::lock_guard<std::mutex> lock(m_dbMutex);

    char *errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        LOG_ERROR("SQLite exec failed: " + err + " | SQL: " + sql.substr(0, 200));
        return false;
    }
    return true;
}

json Database::query(const std::string &sql)
{
    return query(sql, json::array());
}

json Database::query(const std::string &sql, const json &params)
{
    if (!m_db) return json::array();
    std::lock_guard<std::mutex> lock(m_dbMutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQLite prepare failed: " + std::string(sqlite3_errmsg(m_db)));
        return json::array();
    }

    // Bind parameters
    for (size_t i = 0; i < params.size(); ++i) {
        int idx = static_cast<int>(i + 1);
        const auto &val = params[i];
        if (val.is_null()) {
            sqlite3_bind_null(stmt, idx);
        } else if (val.is_number_integer()) {
            sqlite3_bind_int64(stmt, idx, val.get<int64_t>());
        } else if (val.is_number_float()) {
            sqlite3_bind_double(stmt, idx, val.get<double>());
        } else if (val.is_boolean()) {
            sqlite3_bind_int(stmt, idx, val.get<bool>() ? 1 : 0);
        } else {
            std::string s = val.get<std::string>();
            sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    // Get column names
    int colCount = sqlite3_column_count(stmt);
    std::vector<std::string> colNames;
    colNames.reserve(colCount);
    for (int i = 0; i < colCount; ++i) {
        colNames.push_back(sqlite3_column_name(stmt, i));
    }

    // Fetch rows
    json result = json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json row;
        for (int i = 0; i < colCount; ++i) {
            int type = sqlite3_column_type(stmt, i);
            switch (type) {
            case SQLITE_NULL:
                row[colNames[i]] = nullptr;
                break;
            case SQLITE_INTEGER:
                row[colNames[i]] = sqlite3_column_int64(stmt, i);
                break;
            case SQLITE_FLOAT:
                row[colNames[i]] = sqlite3_column_double(stmt, i);
                break;
            case SQLITE_TEXT:
                row[colNames[i]] = reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
                break;
            default:
                row[colNames[i]] = nullptr;
                break;
            }
        }
        result.push_back(row);
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Database::execute(const std::string &sql, const json &params)
{
    if (!m_db) return false;
    std::lock_guard<std::mutex> lock(m_dbMutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQLite prepare failed: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }

    // Bind parameters
    for (size_t i = 0; i < params.size(); ++i) {
        int idx = static_cast<int>(i + 1);
        const auto &val = params[i];
        if (val.is_null()) {
            sqlite3_bind_null(stmt, idx);
        } else if (val.is_number_integer()) {
            sqlite3_bind_int64(stmt, idx, val.get<int64_t>());
        } else if (val.is_number_float()) {
            sqlite3_bind_double(stmt, idx, val.get<double>());
        } else if (val.is_boolean()) {
            sqlite3_bind_int(stmt, idx, val.get<bool>() ? 1 : 0);
        } else {
            std::string s = val.get<std::string>();
            sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        LOG_ERROR("SQLite step failed: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    return true;
}

int64_t Database::lastInsertRowId() const
{
    if (!m_db) return 0;
    std::lock_guard<std::mutex> lock(m_dbMutex);
    return sqlite3_last_insert_rowid(m_db);
}

int Database::changes() const
{
    if (!m_db) return 0;
    std::lock_guard<std::mutex> lock(m_dbMutex);
    return sqlite3_changes(m_db);
}
