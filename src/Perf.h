#pragma once

/**
 * 性能埋点：用来对比清理算法改动前后的吞吐，以及对服务器 tick 的影响。
 *
 * 全部在主线程调用（服务器 tick 事件 / 指令处理），因此不加锁。
 * 查看方式：控制台或 GameDirectors 以上权限执行 /tsperf。
 */

#include <cstdint>
#include <string>

namespace ts::perf {

/// 服务器 tick 间隔超过该值（毫秒）视为"卡顿 tick"；自适应预算也以它为退让阈值
constexpr double kLagThresholdMs = 55.0;

/// 最近一个完整的 1 秒统计窗口
struct Snapshot {
    int64_t blocks{0};         ///< 窗口内被清除的方块数
    int     pumps{0};          ///< 窗口内执行了清理的 tick 数（仅统计有运行中任务的 tick）
    int     tasks{0};          ///< 最近一次清理时运行中的任务数
    double  workMsAvg{0.0};    ///< 清理耗时均值（ms / tick）
    double  workMsMax{0.0};    ///< 清理耗时峰值（ms）
    double  periodMsAvg{0.0};  ///< 服务器 tick 间隔均值（健康时约 50ms）
    double  periodMsMax{0.0};  ///< 服务器 tick 间隔峰值
    int     lagTicks{0};       ///< 间隔超过 kLagThresholdMs 的 tick 数
    double  budgetScale{1.0};  ///< 自适应预算当前缩放（1.0 = 满额）
};

/// 每个服务器 tick 调用一次，用相邻两次调用的间隔近似 MSPT 上界
void onServerTick();

/// 最近一次测得的服务器 tick 间隔（毫秒）；尚无数据时为 50
[[nodiscard]] double lastPeriodMs();

/// 记录一次清理：耗时、清除的方块数、运行中任务数、当前预算缩放
void recordPump(double workMs, int64_t blocks, int tasks, double budgetScale);

/// 每秒调用一次：把当前窗口结算为 Snapshot 并清零
void rollSecond();

[[nodiscard]] Snapshot const& snapshot();

/// 供 /tsperf 输出的纯文本（多行，不含颜色码，控制台与游戏内都能读）
[[nodiscard]] std::string describe();

} // namespace ts::perf
