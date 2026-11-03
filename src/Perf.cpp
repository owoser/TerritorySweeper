#include "Perf.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ts::perf {

namespace {

using Clock = std::chrono::steady_clock;

/// 正在累计的 1 秒窗口
struct Window {
    int64_t blocks{0};
    int     pumps{0};
    double  workSum{0.0};
    double  workMax{0.0};
    int     ticks{0};
    double  periodSum{0.0};
    double  periodMax{0.0};
    int     lagTicks{0};
};

Window           gWindow;
Snapshot         gSnapshot;
Clock::time_point gLastTick{};
bool             gHaveLastTick  = false;
double           gLastPeriodMs  = 50.0;
int              gLastTasks     = 0;
double           gLastScale     = 1.0;

// 自加载以来的累计，用于算平均吞吐
int64_t gTotalBlocks   = 0;
int64_t gActiveSeconds = 0;
int64_t gPeakBlocksPerSecond = 0;

constexpr double kStatsClampMs = 1000.0; ///< 超长停顿只按 1 秒计入均值，避免一次卡死把均值拉飞

} // namespace

void onServerTick() {
    auto const now = Clock::now();
    if (gHaveLastTick) {
        double const ms = std::chrono::duration<double, std::milli>(now - gLastTick).count();
        gLastPeriodMs   = ms; // 自适应预算看真实值：停顿越长越该退让

        double const clamped = std::min(ms, kStatsClampMs);
        gWindow.periodSum += clamped;
        gWindow.periodMax = std::max(gWindow.periodMax, clamped);
        ++gWindow.ticks;
        if (ms > kLagThresholdMs) {
            ++gWindow.lagTicks;
        }
    }
    gLastTick     = now;
    gHaveLastTick = true;
}

double lastPeriodMs() { return gLastPeriodMs; }

void recordPump(double workMs, int64_t blocks, int tasks, double budgetScale) {
    ++gWindow.pumps;
    gWindow.workSum += workMs;
    gWindow.workMax = std::max(gWindow.workMax, workMs);
    gWindow.blocks += blocks;
    gTotalBlocks += blocks;
    gLastTasks = tasks;
    gLastScale = budgetScale;
}

void rollSecond() {
    Snapshot s;
    s.blocks      = gWindow.blocks;
    s.pumps       = gWindow.pumps;
    s.tasks       = gLastTasks;
    s.workMsAvg   = gWindow.pumps > 0 ? gWindow.workSum / gWindow.pumps : 0.0;
    s.workMsMax   = gWindow.workMax;
    s.periodMsAvg = gWindow.ticks > 0 ? gWindow.periodSum / gWindow.ticks : 0.0;
    s.periodMsMax = gWindow.periodMax;
    s.lagTicks    = gWindow.lagTicks;
    s.budgetScale = gLastScale;
    gSnapshot     = s;

    if (gWindow.pumps > 0) {
        ++gActiveSeconds;
        gPeakBlocksPerSecond = std::max(gPeakBlocksPerSecond, gWindow.blocks);
    } else {
        // 没有任务在跑：把任务数和缩放复位，避免 /tsperf 一直显示上次任务的残留值
        gLastTasks = 0;
        gLastScale = 1.0;
    }
    gWindow = Window{};
}

Snapshot const& snapshot() { return gSnapshot; }

std::string describe() {
    auto const& s = gSnapshot;

    long long const avgActive =
        gActiveSeconds > 0 ? static_cast<long long>(gTotalBlocks / gActiveSeconds) : 0LL;

    char buf[768];
    std::snprintf(
        buf,
        sizeof(buf),
        "[领地清扫] 性能统计（最近 1 秒）\n"
        "  清除方块: %lld /s    运行中任务: %d    清理 tick 数: %d/20\n"
        "  清理耗时: 均值 %.2f ms/tick    峰值 %.2f ms\n"
        "  服务器 tick 间隔: 均值 %.1f ms    峰值 %.1f ms    卡顿 tick(>%.0fms): %d\n"
        "  自适应预算缩放: %.2f\n"
        "累计: 清除 %lld 块，活跃 %lld 秒，平均 %lld /s，单秒峰值 %lld /s",
        static_cast<long long>(s.blocks),
        s.tasks,
        s.pumps,
        s.workMsAvg,
        s.workMsMax,
        s.periodMsAvg,
        s.periodMsMax,
        kLagThresholdMs,
        s.lagTicks,
        s.budgetScale,
        static_cast<long long>(gTotalBlocks),
        static_cast<long long>(gActiveSeconds),
        avgActive,
        static_cast<long long>(gPeakBlocksPerSecond)
    );
    return std::string{buf};
}

} // namespace ts::perf
