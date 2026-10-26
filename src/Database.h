#pragma once

/**
 * SQLite 轻量封装
 *
 * - 只做本项目需要的最小面：打开/关闭、直接执行 SQL、预编译语句 RAII、事务。
 * - 所有调用都假定发生在服务器线程；内部仍用互斥量兜底。
 * - 未打开数据库时，所有写入接口安全返回 false，调用方无需关心降级路径。
 */

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace ts::db {

/// 预编译语句；析构时自动 finalize
class Statement {
public:
    Statement() = default;
    Statement(sqlite3* db, std::string_view sql, char const* tag);
    ~Statement();

    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(Statement const&)            = delete;
    Statement& operator=(Statement const&) = delete;

    [[nodiscard]] bool valid() const { return mStmt != nullptr; }

    bool bind(int index, int64_t value);
    bool bind(int index, std::string_view text);
    bool bindNull(int index);

    /// 执行一条不返回结果集的语句（INSERT/UPDATE/DELETE/CREATE...），成功返回 true
    bool execute();

    /// 推进到下一条结果行；没有更多行或出错时返回 false
    bool next();

    void reset();

    [[nodiscard]] int64_t     columnInt(int index) const;
    [[nodiscard]] std::string columnText(int index) const;
    [[nodiscard]] bool        columnBool(int index) const;

private:
    sqlite3*      mDb{nullptr};
    sqlite3_stmt* mStmt{nullptr};
    char const*   mTag{""};
};

class Database {
public:
    static Database& getInstance();

    bool open(std::filesystem::path const& file);
    void close();

    [[nodiscard]] bool                    ok() const { return mDb != nullptr; }
    [[nodiscard]] std::string             path() const { return mPath.string(); }
    [[nodiscard]] sqlite3*                raw() const { return mDb; }

    bool exec(std::string_view sql, char const* tag = "exec");

    bool begin();
    bool commit();
    bool rollback();

    [[nodiscard]] int64_t lastInsertRowId() const;

private:
    Database() = default;

    sqlite3*              mDb{nullptr};
    std::filesystem::path mPath;
};

/// 全局互斥量：保证多入口（tick / 表单 / 指令）不会并发访问同一连接
[[nodiscard]] std::mutex& mutex();

} // namespace ts::db
