#include "TaskStore.h"

#include "Database.h"

#include "Global.h"

#include <sqlite3.h>

#include <ctime>
#include <initializer_list>
#include <string>
#include <unordered_set>

namespace ts::db {

namespace {

// 列顺序 == 绑定顺序 == SELECT 顺序，改动时三处一起改
constexpr std::initializer_list<char const*> kTaskColumns = {
    "id",
    "owner_name",
    "owner_xuid",
    "owner_uuid",
    "state",
    "layers_done",
    "countdown",
    "chunk_index",
    "chunk_cooldown",
    "cursor",
    "paused_secs",
    "paid_amount",
    // ---- SweepPlan 展平（21 列，起始下标 12）----
    "dim_id",
    "min_x",
    "max_x",
    "min_z",
    "max_z",
    "start_y",
    "min_y",
    "max_y",
    "direction",
    "layer_count",
    "layer_auto",
    "levelname",
    "unit_price",
    "total_price",
    "chunked",
    "chunk_width",
    "chunk_depth",
    "chunk_interval",
    "layer_countdown",
    "land_id",
    "land_name",
    "land_min_x",
    "land_max_x",
    "land_min_y",
    "land_max_y",
    "land_min_z",
    "land_max_z",
    // ---
    "updated_at",
};

constexpr std::initializer_list<char const*> kBillColumns = {
    "land_id",
    "owner_name",
    "owner_xuid",
    "owner_uuid",
    "created_at",
    // ---- SweepPlan 展平（21 列，起始下标 5）----
    "dim_id",
    "min_x",
    "max_x",
    "min_z",
    "max_z",
    "start_y",
    "min_y",
    "max_y",
    "direction",
    "layer_count",
    "layer_auto",
    "levelname",
    "unit_price",
    "total_price",
    "chunked",
    "chunk_width",
    "chunk_depth",
    "chunk_interval",
    "layer_countdown",
    // 注意：bills 的 land_id 就是主键，已在上方声明，这里不能再重复一份
    "land_name",
    "land_min_x",
    "land_max_x",
    "land_min_y",
    "land_max_y",
    "land_min_z",
    "land_max_z",
};

constexpr int kTaskPlanBase    = 12; ///< 0-based，tasks 中 plan 首列（含 land_id）
constexpr int kBillPlanBase    = 5;  ///< 0-based，bills 中 plan 首列（不含 land_id）
constexpr int kPlanColumnCount = 27; ///< tasks 的 plan 列数（含 land_id 与 6 个领地 AABB 列）

std::string joinColumns(std::initializer_list<char const*> cols) {
    std::string out;
    for (auto c : cols) {
        if (!out.empty()) {
            out += ", ";
        }
        out += c;
    }
    return out;
}

std::string buildUpsertSql(
    char const*                  table,
    std::initializer_list<char const*> cols,
    char const*                  pk
) {
    std::string names;
    std::string marks;
    std::string sets;
    bool        firstSet = true;

    for (auto c : cols) {
        if (!names.empty()) {
            names += ", ";
            marks += ", ";
        }
        names += c;
        marks += "?";

        if (std::string_view{c} != std::string_view{pk}) {
            if (!firstSet) {
                sets += ", ";
            }
            firstSet = false;
            sets += std::string{c} + "=excluded." + c;
        }
    }

    return std::string{"INSERT INTO "} + table + " (" + names + ") VALUES (" + marks + ") ON CONFLICT(" + pk
         + ") DO UPDATE SET " + sets + ";";
}

/// base 为 1-based 绑定起始下标；withLandId 为 false 时跳过 land_id（bills 主键已单独绑定）
void bindPlan(Statement& st, int base, SweepPlan const& plan, bool withLandId) {
    int i = base;
    st.bind(i++, (int64_t)plan.dimId);
    st.bind(i++, (int64_t)plan.minX);
    st.bind(i++, (int64_t)plan.maxX);
    st.bind(i++, (int64_t)plan.minZ);
    st.bind(i++, (int64_t)plan.maxZ);
    st.bind(i++, (int64_t)plan.startY);
    st.bind(i++, (int64_t)plan.minY);
    st.bind(i++, (int64_t)plan.maxY);
    st.bind(i++, (int64_t)plan.direction);
    st.bind(i++, (int64_t)plan.layerCount);
    st.bind(i++, (int64_t)(plan.layerCountAuto ? 1 : 0));
    st.bind(i++, plan.levelname);
    st.bind(i++, plan.unitPrice);
    st.bind(i++, plan.totalPrice);
    st.bind(i++, (int64_t)(plan.chunked ? 1 : 0));
    st.bind(i++, (int64_t)plan.chunkWidth);
    st.bind(i++, (int64_t)plan.chunkDepth);
    st.bind(i++, (int64_t)plan.chunkInterval);
    st.bind(i++, (int64_t)plan.layerCountdown);
    if (withLandId) {
        st.bind(i++, plan.landId);
    }
    st.bind(i++, plan.landName);
    st.bind(i++, (int64_t)plan.landMinX);
    st.bind(i++, (int64_t)plan.landMaxX);
    st.bind(i++, (int64_t)plan.landMinY);
    st.bind(i++, (int64_t)plan.landMaxY);
    st.bind(i++, (int64_t)plan.landMinZ);
    st.bind(i++, (int64_t)plan.landMaxZ);
}

SweepPlan readPlan(Statement const& st, int base, bool withLandId) {
    SweepPlan plan;
    int       i = base;
    plan.dimId          = (int)st.columnInt(i++);
    plan.minX           = (int)st.columnInt(i++);
    plan.maxX           = (int)st.columnInt(i++);
    plan.minZ           = (int)st.columnInt(i++);
    plan.maxZ           = (int)st.columnInt(i++);
    plan.startY         = (int)st.columnInt(i++);
    plan.minY           = (int)st.columnInt(i++);
    plan.maxY           = (int)st.columnInt(i++);
    plan.direction      = (SweepDirection)st.columnInt(i++);
    plan.layerCount     = (int)st.columnInt(i++);
    plan.layerCountAuto = st.columnBool(i++);
    plan.levelname      = st.columnText(i++);
    plan.unitPrice      = st.columnInt(i++);
    plan.totalPrice     = st.columnInt(i++);
    plan.chunked        = st.columnBool(i++);
    plan.chunkWidth     = (int)st.columnInt(i++);
    plan.chunkDepth     = (int)st.columnInt(i++);
    plan.chunkInterval  = (int)st.columnInt(i++);
    plan.layerCountdown = (int)st.columnInt(i++);
    if (withLandId) {
        plan.landId = st.columnInt(i++);
    }
    plan.landName = st.columnText(i++);
    plan.landMinX = (int)st.columnInt(i++);
    plan.landMaxX = (int)st.columnInt(i++);
    plan.landMinY = (int)st.columnInt(i++);
    plan.landMaxY = (int)st.columnInt(i++);
    plan.landMinZ = (int)st.columnInt(i++);
    plan.landMaxZ = (int)st.columnInt(i++);
    return plan;
}

int64_t nowSeconds() { return (int64_t)std::time(nullptr); }

/// 幂等补列：表里没有该列才 ALTER TABLE 添加
void ensureColumn(sqlite3* db, std::string_view table, std::string_view column, std::string_view decl) {
    std::string pragma = "PRAGMA table_info(" + std::string{table} + ");";
    {
        Statement info{db, pragma, "schema:table_info"};
        if (!info.valid()) {
            return;
        }
        while (info.next()) {
            if (info.columnText(1) == column) {
                return; // 已存在
            }
        }
    }

    std::string sql = "ALTER TABLE " + std::string{table} + " ADD COLUMN " + std::string{column} + " "
                    + std::string{decl} + ";";
    Database::getInstance().exec(sql, "schema:alter");
}

} // namespace

// ---------------------------------------------------------------------------
// schema
// ---------------------------------------------------------------------------

bool initSchema() {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return false;
    }

    static char const* kDdl = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS tasks (
    id              INTEGER PRIMARY KEY,
    owner_name      TEXT    NOT NULL,
    owner_xuid      TEXT    NOT NULL,
    owner_uuid      TEXT    NOT NULL,
    state           INTEGER NOT NULL,
    layers_done     INTEGER NOT NULL,
    countdown       INTEGER NOT NULL,
    chunk_index     INTEGER NOT NULL,
    chunk_cooldown  INTEGER NOT NULL,
    cursor          INTEGER NOT NULL,
    paused_secs     INTEGER NOT NULL,
    paid_amount     INTEGER NOT NULL,
    dim_id          INTEGER NOT NULL,
    min_x           INTEGER NOT NULL,
    max_x           INTEGER NOT NULL,
    min_z           INTEGER NOT NULL,
    max_z           INTEGER NOT NULL,
    start_y         INTEGER NOT NULL,
    min_y           INTEGER NOT NULL,
    max_y           INTEGER NOT NULL,
    direction       INTEGER NOT NULL,
    layer_count     INTEGER NOT NULL,
    layer_auto      INTEGER NOT NULL,
    levelname       TEXT    NOT NULL,
    unit_price      INTEGER NOT NULL,
    total_price     INTEGER NOT NULL,
    chunked         INTEGER NOT NULL,
    chunk_width     INTEGER NOT NULL,
    chunk_depth     INTEGER NOT NULL,
    chunk_interval  INTEGER NOT NULL,
    layer_countdown INTEGER NOT NULL,
    land_id         INTEGER NOT NULL,
    land_name       TEXT    NOT NULL,
    land_min_x      INTEGER NOT NULL DEFAULT 0,
    land_max_x      INTEGER NOT NULL DEFAULT 0,
    land_min_y      INTEGER NOT NULL DEFAULT 0,
    land_max_y      INTEGER NOT NULL DEFAULT 0,
    land_min_z      INTEGER NOT NULL DEFAULT 0,
    land_max_z      INTEGER NOT NULL DEFAULT 0,
    updated_at      INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_tasks_land  ON tasks(land_id);
CREATE INDEX IF NOT EXISTS idx_tasks_owner ON tasks(owner_uuid);

CREATE TABLE IF NOT EXISTS bills (
    land_id         INTEGER PRIMARY KEY,
    owner_name      TEXT    NOT NULL,
    owner_xuid      TEXT    NOT NULL,
    owner_uuid      TEXT    NOT NULL,
    created_at      INTEGER NOT NULL,
    dim_id          INTEGER NOT NULL,
    min_x           INTEGER NOT NULL,
    max_x           INTEGER NOT NULL,
    min_z           INTEGER NOT NULL,
    max_z           INTEGER NOT NULL,
    start_y         INTEGER NOT NULL,
    min_y           INTEGER NOT NULL,
    max_y           INTEGER NOT NULL,
    direction       INTEGER NOT NULL,
    layer_count     INTEGER NOT NULL,
    layer_auto      INTEGER NOT NULL,
    levelname       TEXT    NOT NULL,
    unit_price      INTEGER NOT NULL,
    total_price     INTEGER NOT NULL,
    chunked         INTEGER NOT NULL,
    chunk_width     INTEGER NOT NULL,
    chunk_depth     INTEGER NOT NULL,
    chunk_interval  INTEGER NOT NULL,
    layer_countdown INTEGER NOT NULL,
    land_name       TEXT    NOT NULL,
    land_min_x      INTEGER NOT NULL DEFAULT 0,
    land_max_x      INTEGER NOT NULL DEFAULT 0,
    land_min_y      INTEGER NOT NULL DEFAULT 0,
    land_max_y      INTEGER NOT NULL DEFAULT 0,
    land_min_z      INTEGER NOT NULL DEFAULT 0,
    land_max_z      INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS refunds (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id    INTEGER NOT NULL,
    owner_name TEXT    NOT NULL,
    owner_xuid TEXT    NOT NULL,
    amount     INTEGER NOT NULL,
    progress   INTEGER NOT NULL,
    reason     TEXT    NOT NULL,
    created_at INTEGER NOT NULL
);
)SQL";

    if (!database.exec(kDdl, "init:schema")) {
        return false;
    }

    int64_t version = 0;
    {
        Statement st{database.raw(), "PRAGMA user_version;", "schema:version"};
        if (st.valid() && st.next()) {
            version = st.columnInt(0);
        }
    }
    constexpr int64_t kCurrentVersion = 2;

    if (version == 0) {
        database.exec("PRAGMA user_version = 1;", "schema:set-version");
        version = 1;
    }

    // v1 -> v2：任务/账单补充领地 AABB 六列（幂等，已存在则跳过）
    if (version < 2) {
        for (auto const* table : {"tasks", "bills"}) {
            ensureColumn(database.raw(), table, "land_min_x", "INTEGER NOT NULL DEFAULT 0");
            ensureColumn(database.raw(), table, "land_max_x", "INTEGER NOT NULL DEFAULT 0");
            ensureColumn(database.raw(), table, "land_min_y", "INTEGER NOT NULL DEFAULT 0");
            ensureColumn(database.raw(), table, "land_max_y", "INTEGER NOT NULL DEFAULT 0");
            ensureColumn(database.raw(), table, "land_min_z", "INTEGER NOT NULL DEFAULT 0");
            ensureColumn(database.raw(), table, "land_max_z", "INTEGER NOT NULL DEFAULT 0");
        }
        database.exec("PRAGMA user_version = 2;", "schema:set-version");
        log::get().info("数据库已从 schema v{} 迁移到 v2（新增领地 AABB 列）。", version);
        version = 2;
    }

    if (version > kCurrentVersion) {
        log::get().error(
            "数据库 schema 版本 {} 高于插件支持的 {}，可能来自更新版本，读写行为可能异常。",
            version,
            kCurrentVersion
        );
    } else {
        log::get().info("数据库 schema v{} 就绪。", version);
    }
    return true;
}

// ---------------------------------------------------------------------------
// meta
// ---------------------------------------------------------------------------

int64_t getMetaInt(std::string_view key, int64_t fallback) {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return fallback;
    }
    Statement st{database.raw(), "SELECT value FROM meta WHERE key = ?;", "meta:get"};
    if (!st.valid()) {
        return fallback;
    }
    st.bind(1, key);
    if (!st.next()) {
        return fallback;
    }
    auto text = st.columnText(0);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoll(text);
    } catch (...) {
        return fallback;
    }
}

bool setMetaInt(std::string_view key, int64_t value) {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return false;
    }
    Statement st{
        database.raw(),
        "INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
        "meta:set"
    };
    if (!st.valid()) {
        return false;
    }
    st.bind(1, key);
    st.bind(2, std::to_string(value));
    return st.execute();
}

// ---------------------------------------------------------------------------
// tasks
// ---------------------------------------------------------------------------

bool upsertTask(SweepTask const& task) {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return false;
    }

    static std::string const kSql = buildUpsertSql("tasks", kTaskColumns, "id");

    std::lock_guard lock(mutex());
    Statement       st{database.raw(), kSql, "tasks:upsert"};
    if (!st.valid()) {
        return false;
    }

    st.bind(1, (int64_t)task.id);
    st.bind(2, task.ownerName);
    st.bind(3, task.ownerXuid);
    st.bind(4, task.ownerUuid.asString());
    st.bind(5, (int64_t)task.state);
    st.bind(6, (int64_t)task.layersDone);
    st.bind(7, (int64_t)task.countdown);
    st.bind(8, (int64_t)task.chunkIndex);
    st.bind(9, (int64_t)task.chunkCooldown);
    st.bind(10, task.cursor);
    st.bind(11, (int64_t)task.pausedSeconds);
    st.bind(12, task.paidAmount);
    bindPlan(st, kTaskPlanBase + 1, task.plan, true);
    st.bind(kTaskPlanBase + 1 + kPlanColumnCount, nowSeconds());

    return st.execute();
}

bool deleteTask(uint64_t id) {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return false;
    }
    std::lock_guard lock(mutex());
    Statement       st{database.raw(), "DELETE FROM tasks WHERE id = ?;", "tasks:delete"};
    if (!st.valid()) {
        return false;
    }
    st.bind(1, (int64_t)id);
    return st.execute();
}

std::vector<SweepTask> loadTasks() {
    std::vector<SweepTask> out;
    auto&                  database = Database::getInstance();
    if (!database.ok()) {
        return out;
    }

    std::string sql = "SELECT " + joinColumns(kTaskColumns) + " FROM tasks ORDER BY id;";

    std::lock_guard lock(mutex());
    Statement       st{database.raw(), sql, "tasks:load"};
    if (!st.valid()) {
        return out;
    }

    std::unordered_set<int64_t> seenLands;
    while (st.next()) {
        SweepTask task;
        task.id            = (uint64_t)st.columnInt(0);
        task.ownerName     = st.columnText(1);
        task.ownerXuid     = st.columnText(2);
        task.ownerUuid     = mce::UUID::fromString(st.columnText(3));
        task.state         = (TaskState)st.columnInt(4);
        task.layersDone    = (int)st.columnInt(5);
        task.countdown     = (int)st.columnInt(6);
        task.chunkIndex    = (int)st.columnInt(7);
        task.chunkCooldown = (int)st.columnInt(8);
        task.cursor        = st.columnInt(9);
        task.pausedSeconds = (int)st.columnInt(10);
        task.paidAmount    = st.columnInt(11);
        task.plan          = readPlan(st, kTaskPlanBase, true);

        if (seenLands.count(task.plan.landId) > 0) {
            log::get().warn(
                "检测到领地 #{} 存在重复任务行，已忽略 task_id={}（保留 updated_at 最新的一条）。",
                task.plan.landId,
                task.id
            );
            continue;
        }
        seenLands.insert(task.plan.landId);

        // 重启后统一恢复为暂停，等待领主手动继续
        task.state         = TaskState::Paused;
        task.pausedSeconds = 0;
        out.push_back(std::move(task));
    }
    return out;
}

// ---------------------------------------------------------------------------
// bills
// ---------------------------------------------------------------------------

bool upsertBill(PendingBill const& bill) {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return false;
    }

    static std::string const kSql = buildUpsertSql("bills", kBillColumns, "land_id");

    std::lock_guard lock(mutex());
    Statement       st{database.raw(), kSql, "bills:upsert"};
    if (!st.valid()) {
        return false;
    }

    st.bind(1, bill.plan.landId);
    st.bind(2, bill.ownerName);
    st.bind(3, bill.ownerXuid);
    st.bind(4, bill.ownerUuid.asString());
    st.bind(5, bill.createdAt);
    bindPlan(st, kBillPlanBase + 1, bill.plan, false);

    return st.execute();
}

bool deleteBill(int64_t landId) {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return false;
    }
    std::lock_guard lock(mutex());
    Statement       st{database.raw(), "DELETE FROM bills WHERE land_id = ?;", "bills:delete"};
    if (!st.valid()) {
        return false;
    }
    st.bind(1, landId);
    return st.execute();
}

std::vector<PendingBill> loadBills() {
    std::vector<PendingBill> out;
    auto&                    database = Database::getInstance();
    if (!database.ok()) {
        return out;
    }

    std::string sql = "SELECT " + joinColumns(kBillColumns) + " FROM bills ORDER BY land_id;";

    std::lock_guard lock(mutex());
    Statement       st{database.raw(), sql, "bills:load"};
    if (!st.valid()) {
        return out;
    }

    while (st.next()) {
        PendingBill bill;
        bill.plan.landId = st.columnInt(0);
        bill.ownerName   = st.columnText(1);
        bill.ownerXuid   = st.columnText(2);
        bill.ownerUuid   = mce::UUID::fromString(st.columnText(3));
        bill.createdAt   = st.columnInt(4);
        // land_id 是 bills 主键（第 0 列），plan 块中已省略该列，读取后补回
        auto plan    = readPlan(st, kBillPlanBase, false);
        plan.landId  = bill.plan.landId;
        bill.plan    = std::move(plan);
        out.push_back(std::move(bill));
    }
    return out;
}

// ---------------------------------------------------------------------------
// refunds
// ---------------------------------------------------------------------------

bool recordRefund(
    uint64_t           taskId,
    std::string const& ownerName,
    std::string const& ownerXuid,
    int64_t            amount,
    int                progressPermille,
    std::string const& reason
) {
    auto& database = Database::getInstance();
    if (!database.ok()) {
        return false;
    }

    std::lock_guard lock(mutex());
    Statement       st{
        database.raw(),
        "INSERT INTO refunds (task_id, owner_name, owner_xuid, amount, progress, reason, created_at) "
              "VALUES (?, ?, ?, ?, ?, ?, ?);",
        "refunds:insert"
    };
    if (!st.valid()) {
        return false;
    }
    st.bind(1, (int64_t)taskId);
    st.bind(2, ownerName);
    st.bind(3, ownerXuid);
    st.bind(4, amount);
    st.bind(5, (int64_t)progressPermille);
    st.bind(6, reason);
    st.bind(7, nowSeconds());
    return st.execute();
}

} // namespace ts::db
