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
    int configVersion{1};

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
    int  pauseAutoCancelSeconds{300};

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
