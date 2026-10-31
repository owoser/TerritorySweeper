#pragma once

/**
 * 任务 / 账单 / 退款审计的 SQLite 持久化
 *
 * 表结构见 initSchema()；所有函数在数据库未打开时安全返回空/false。
 */

#include "Task.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ts::db {

/// 建表 + 索引 + schema 版本；返回是否成功
bool initSchema();

/// 读取全部任务（按 id 升序）。同一领地允许多个任务，不做 land_id 去重。
std::vector<SweepTask> loadTasks();

/// 读取全部待支付账单（按 bill_id 升序）；同一领地可有多张
std::vector<PendingBill> loadBills();

bool upsertTask(SweepTask const& task);
bool flushDirtyTasks(std::vector<SweepTask>& tasks);
bool upsertBill(PendingBill const& bill); ///< 按 bill.id 写入（id=0 时插入并回填）

bool deleteTask(uint64_t id);
bool deleteBill(int64_t billId);

int64_t getMetaInt(std::string_view key, int64_t fallback);
bool    setMetaInt(std::string_view key, int64_t value);

/// 离线退款暂存
bool                        insertPendingPayout(PendingPayout const& payout);
std::vector<PendingPayout>  loadPendingPayouts(std::string const& ownerUuid);
bool                        markPayoutPaid(int64_t id);

/// 退款审计
bool recordRefund(
    uint64_t           taskId,
    std::string const& ownerName,
    std::string const& ownerXuid,
    int64_t            amount,
    int                progressPermille,
    std::string const& reason
);

} // namespace ts::db
