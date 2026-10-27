#include "Database.h"

#include "Global.h"

#include <sqlite3.h>

#include <utility>

namespace ts::db {

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------

Statement::Statement(sqlite3* db, std::string_view sql, char const* tag) : mDb(db), mTag(tag ? tag : "stmt") {
    if (!mDb) {
        return;
    }
    std::string text{sql};
    if (sqlite3_prepare_v2(mDb, text.c_str(), (int)text.size(), &mStmt, nullptr) != SQLITE_OK) {
        log::get().error("SQL 预编译失败 [{}]: {}", mTag, sqlite3_errmsg(mDb));
        mStmt = nullptr;
    }
}

Statement::Statement(sqlite3* db, sqlite3_stmt* stmt, bool isCached, char const* tag)
: mDb(db), mStmt(stmt), mCached(isCached), mTag(tag ? tag : "cached_stmt") {}

Statement::~Statement() {
    if (mStmt) {
        if (mCached) {
            sqlite3_reset(mStmt);
            sqlite3_clear_bindings(mStmt);
        } else {
            sqlite3_finalize(mStmt);
        }
        mStmt = nullptr;
    }
}

Statement::Statement(Statement&& other) noexcept
: mDb(std::exchange(other.mDb, nullptr)),
  mStmt(std::exchange(other.mStmt, nullptr)),
  mCached(std::exchange(other.mCached, false)),
  mTag(std::exchange(other.mTag, "")) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (mStmt) {
            if (mCached) {
                sqlite3_reset(mStmt);
                sqlite3_clear_bindings(mStmt);
            } else {
                sqlite3_finalize(mStmt);
            }
        }
        mDb     = std::exchange(other.mDb, nullptr);
        mStmt   = std::exchange(other.mStmt, nullptr);
        mCached = std::exchange(other.mCached, false);
        mTag    = std::exchange(other.mTag, "");
    }
    return *this;
}

Statement Statement::cached(sqlite3* db, std::string_view sql, char const* tag) {
    if (!db) {
        return Statement{};
    }
    sqlite3_stmt* stmt = Database::getInstance().getCachedStmt(sql, tag);
    if (!stmt) {
        return Statement{};
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return Statement(db, stmt, true, tag);
}

bool Statement::bind(int index, int64_t value) {
    if (!mStmt) {
        return false;
    }
    return sqlite3_bind_int64(mStmt, index, value) == SQLITE_OK;
}

bool Statement::bind(int index, std::string_view text) {
    if (!mStmt) {
        return false;
    }
    return sqlite3_bind_text(mStmt, index, text.data(), (int)text.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Statement::bindNull(int index) {
    if (!mStmt) {
        return false;
    }
    return sqlite3_bind_null(mStmt, index) == SQLITE_OK;
}

bool Statement::execute() {
    if (!mStmt) {
        return false;
    }
    auto rc = sqlite3_step(mStmt);
    if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
        return true;
    }
    log::get().error("SQL 执行失败 [{}]: {}", mTag, sqlite3_errmsg(mDb));
    return false;
}

bool Statement::next() {
    if (!mStmt) {
        return false;
    }
    auto rc = sqlite3_step(mStmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc != SQLITE_DONE) {
        log::get().error("SQL 查询失败 [{}]: {}", mTag, sqlite3_errmsg(mDb));
    }
    return false;
}

void Statement::reset() {
    if (mStmt) {
        sqlite3_reset(mStmt);
        sqlite3_clear_bindings(mStmt);
    }
}

int64_t Statement::columnInt(int index) const {
    if (!mStmt) {
        return 0;
    }
    return sqlite3_column_int64(mStmt, index);
}

std::string Statement::columnText(int index) const {
    if (!mStmt) {
        return {};
    }
    auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(mStmt, index));
    if (!text) {
        return {};
    }
    int len = sqlite3_column_bytes(mStmt, index);
    return std::string{text, (size_t)(len > 0 ? len : 0)};
}

bool Statement::columnBool(int index) const { return columnInt(index) != 0; }

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database& Database::getInstance() {
    static Database instance;
    return instance;
}

bool Database::open(std::filesystem::path const& file) {
    std::lock_guard lock(mutex());
    close();

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    mPath = file;
    mPath.make_preferred();

    int rc = sqlite3_open_v2(
        mPath.string().c_str(),
        &mDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (rc != SQLITE_OK) {
        log::get().error(
            "无法打开数据库 {}: {}",
            mPath.string(),
            mDb ? sqlite3_errmsg(mDb) : "out of memory"
        );
        if (mDb) {
            sqlite3_close(mDb);
            mDb = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(mDb, 3000);

    exec("PRAGMA journal_mode=WAL;", "pragma:wal");
    exec("PRAGMA synchronous=NORMAL;", "pragma:sync");
    exec("PRAGMA foreign_keys=ON;", "pragma:fk");
    exec("PRAGMA mmap_size=268435456;", "pragma:mmap");      // 256MB 内存映射 I/O
    exec("PRAGMA cache_size=-16000;", "pragma:cache");        // 16MB 内存页缓存
    exec("PRAGMA temp_store=MEMORY;", "pragma:temp");         // 临时表与排序在内存处理
    exec("PRAGMA wal_autocheckpoint=1000;", "pragma:wal_ckpt");

    log::get().info("数据库已打开: {}", mPath.string());
    return true;
}

void Database::close() {
    clearCachedStmts();
    if (mDb) {
        sqlite3_close(mDb);
        mDb = nullptr;
    }
}

sqlite3_stmt* Database::getCachedStmt(std::string_view sql, char const* tag) {
    if (!mDb) {
        return nullptr;
    }
    std::string key{sql};
    auto it = mStmtCache.find(key);
    if (it != mStmtCache.end()) {
        return it->second;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(mDb, key.c_str(), static_cast<int>(key.size()), &stmt, nullptr) != SQLITE_OK) {
        log::get().error("SQL 预编译缓存失败 [{}]: {}", tag ? tag : "cached_stmt", sqlite3_errmsg(mDb));
        return nullptr;
    }
    mStmtCache.emplace(std::move(key), stmt);
    return stmt;
}

void Database::clearCachedStmts() {
    for (auto& [sql, stmt] : mStmtCache) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
    mStmtCache.clear();
}

bool Database::exec(std::string_view sql, char const* tag) {
    if (!mDb) {
        return false;
    }
    std::string text{sql};
    char*       err = nullptr;
    if (sqlite3_exec(mDb, text.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        log::get().error("SQL 执行失败 [{}]: {}", tag ? tag : "exec", err ? err : "unknown");
        if (err) {
            sqlite3_free(err);
        }
        return false;
    }
    return true;
}

bool Database::begin() { return exec("BEGIN IMMEDIATE;", "tx:begin"); }

bool Database::commit() { return exec("COMMIT;", "tx:commit"); }

bool Database::rollback() { return exec("ROLLBACK;", "tx:rollback"); }

int64_t Database::lastInsertRowId() const { return mDb ? sqlite3_last_insert_rowid(mDb) : 0; }

} // namespace ts::db
