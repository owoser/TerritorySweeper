#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ts {

/// 清扫收费规格（阶梯价）
struct PriceLevel {
    std::string levelname{"9*9"};
    int         maxWidth{9};        ///< 规格覆盖的最大 X 跨度（含）
    int         maxDepth{9};        ///< 规格覆盖的最大 Z 跨度（含）
    int64_t     pricePerLayer{1};   ///< 每层单价
};

/**
 * 插件配置。
 * 所有字段名与 config.json 中的键一一对应，缺省值即为默认配置。
 */
struct Config {
    int configVersion{2};

    /// 调试模式：输出发包、任务推进、交互等详细日志（排查用，平时保持 false）
    bool debugEnabled{false};

    // ---- 经济 ----
    std::string economyType{"LLMoney"};        ///< LLMoney / Scoreboard
    std::string scoreboardObjective{"money"};  ///< 计分板目标名（Scoreboard 模式）
    std::string moneyname{"金币"};

    // ---- 任务并发 ----
    int globalTaskLimit{3};
    int playerTaskLimit{1};

    // ---- 显示 ----
    /// BossBar 进度条（BossEvent + AddActor 假实体）。进服异常时可用它快速排查。
    bool bossBarEnabled{true};
    /// BossBar 刷新方式：incremental（只发变化字段）/ recreate（每秒完整重建）
    std::string bossBarUpdateMode{"incremental"};
    /// DebugShape 选区 / 任务区域线框
    bool regionFrameEnabled{true};

    // ---- 清理节奏 ----
    int layerCountdownSeconds{10};

    bool chunkedCleanEnabled{true};
    int  chunkMatrixWidth{10};
    int  chunkMatrixDepth{10};
    int  chunkedMinArea{1024};
    int  chunkIntervalSeconds{2};

    // ---- 看门狗 ----
    bool pauseOnPlayerLeave{true};
    int  playerLeavePauseSeconds{15};
    /// 领主回到领地后是否自动继续"因离开而暂停"的任务
    bool autoResumeOnReturn{true};
    /// 判定是否在领地内时允许的水平越界容差（格）
    int  leaveCheckTolerance{2};

    /// 是否要求"下蹲"才响应木棍；false 时站立与下蹲都可触发
    bool stickRequiresSneak{false};

    /// 分块清理时是否跳过"整块都无需清理"的分块（不消耗分块间隔）
    bool skipEmptyChunks{true};
    /// 整层都无需清理时是否跳过该层（不消耗每层倒计时）
    bool skipEmptyLayers{true};
    /// 每 tick 用于空区域扫描的方块读取预算
    int  emptyScanBudget{8192};
    int  pauseAutoCancelSeconds{300};

    /// 离开领地判定是否忽略 Y（true 时只看水平范围）
    bool leaveCheckIgnoreY{true};
    /// 清理时是否主动加载未加载的目标区块
    bool forceLoadChunks{true};
    /// 每秒最多主动加载多少个区块
    int  chunkLoadBudget{4};
    /// 是否连基岩一起清理
    bool clearBedrock{true};
    /// 保留世界最底部的 N 层基岩
    int  preserveBottomLayers{1};
    /// 任务运行期间阻止液体流入清理区域
    bool blockLiquidInflow{true};
    /// 任务完成前对选区做一次终检清扫（清除残留液体/方块）
    bool finalSweep{true};

    // ---- 价格阶梯（按面积从小到大）----
    std::vector<PriceLevel> priceLevels;

    [[nodiscard]] bool isScoreboardEconomy() const;
    /// 匹配第一个满足 width<=maxWidth && depth<=maxDepth 的规格；超出最大规格时返回最后一档。
    [[nodiscard]] PriceLevel const& matchPriceLevel(int width, int depth) const;
    /// 选区是否超出所有已配置规格
    [[nodiscard]] bool exceedsLargest(int width, int depth) const;
    /// 校验并修正明显非法的取值，返回被修正项的描述（用于启动日志）
    [[nodiscard]] std::vector<std::string> sanitize();

    [[nodiscard]] static Config makeDefault();
};

/// 按配置值渲染出带中文注释的 JSONC 文本（注释只描述字段含义，不包含当前取值）
[[nodiscard]] std::string renderConfig(Config const& cfg);

class ConfigManager {
public:
    static ConfigManager& getInstance();

    /// configDir 为插件配置目录（<mods>/TerritorySweeper/config）
    bool load(std::filesystem::path const& configDir);
    bool reload();

    [[nodiscard]] Config const&                get() const { return mConfig; }
    [[nodiscard]] std::filesystem::path const& path() const { return mPath; }

private:
    Config                mConfig{};
    std::filesystem::path mPath;
};

} // namespace ts
