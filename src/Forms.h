#pragma once

/**
 * 表单流转
 *
 *  方向选择 → 层数配置 → 费用确认 → 二次支付确认
 *                                └→ 挂起账单 / 关闭
 *  运行中/暂停中：任务管理面板 → 终止二次确认
 *  待支付账单：账单面板
 */

#include "Selection.h"
#include "Task.h"

class Player;

namespace ts::form {

/// 打开"领地清扫任务"方向选择
void openDirection(Player& player, PlayerLand const& land, SelectionState const& selection);

/// 打开"清扫任务配置"层数输入
void openLayerInput(Player& player, SweepDirection direction, PlayerLand const& land, SelectionState const& selection);

/// 打开"清扫任务费用"详情
void openCostDetail(Player& player, SweepPlan plan);

/// 打开"费用二次确认"（支付 / 挂起 / 关闭）
void openPayment(Player& player, SweepPlan plan);

/// 打开"领地清扫管理"（暂停或继续、终止、关闭）
void openManage(Player& player, SweepTask& task);

/// 终止任务二次确认
void openKillConfirm(Player& player, uint64_t taskId);

/// 打开待支付账单面板
void openBill(Player& player, PendingBill const& bill);

/// 打开任务列表
void openTaskList(Player& player);

/// 木棍交互入口：依据领地状态自动选择面板
void openSmartMenu(Player& player, PlayerLand const& land);

} // namespace ts::form
