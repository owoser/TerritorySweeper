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

Statement::~Statement() {
    if (mStmt) {
        sqlite3_finalize(mStmt);
        mStmt = nullptr;
    }
}

Statement::Statement(Statement&& other) noexcept
: mDb(std::exchange(other.mDb, nullptr)),
  mStmt(std::exchange(other.mStmt, nullptr)),
  mTag(std::exchange(other.mTag, "")) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (mStmt) {
            sqlite3_finalize(mStmt);
        }
        mDb   = std::exchange(other.mDb, nullptr);
        mStmt = std::exchange(other.mStmt, nullptr);
        mTag  = std::exchange(other.mTag, "");
    }
    return *this;
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

    if (!exec("PRAGMA journal_mode=WAL;", "pragma:wal") || !exec("PRAGMA synchronous=NORMAL;", "pragma:sync")
        || !exec("PRAGMA foreign_keys=ON;", "pragma:fk")) {
        log::get().warn("数据库 PRAGMA 设置未完全成功，继续使用默认行为。");
    }

    log::get().info("数据库已打开: {}", mPath.string());
    return true;
}

void Database::close() {
    if (mDb) {
        sqlite3_close(mDb);
        mDb = nullptr;
    }
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
