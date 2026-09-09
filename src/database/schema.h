#pragma once
#include "database/database.h"

// Database schema management: creates tables and runs migrations
namespace Schema {

// Run all migrations (create tables if they don't exist)
void migrate(Database &db);

} // namespace Schema
