#include "Config.h"

#include "Global.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace ts {

namespace {

using nlohmann::ordered_json;

constexpr char const* kDefaultConfigText = R"JSON({
  "// 配置文件版本，请勿手动修改": 1,
  "config_version": 1,

  "// 经济系统类型：LLMoney（默认，需安装 LegacyMoney）或 Scoreboard（计分板）": "LLMoney",
  "economy_type": "LLMoney",

  "// Scoreboard 模式下绑定的计分板目标名称（需预先用 /scoreboard objectives add 创建）": "money",
  "scoreboard_objective": "money",

  "// 货币显示名称，仅用于界面文案": "金币",
  "moneyname": "金币",

  "// 全服允许同时存在的清理任务数量上限": 3,
  "global_task_limit": 3,

  "// 每个玩家允许同时存在的清理任务数量上限": 1,
  "player_task_limit": 1,

  "// 每层方块开始清理前的倒计时秒数": 10,
  "layer_countdown_seconds": 10,

  "// 是否开启分块清理（大区域分批处理，避免卡顿）": true,
  "chunked_clean_enabled": true,

  "// 分块矩阵：单块覆盖的 X 跨度": 10,
  "chunk_matrix_width": 10,
  "// 分块矩阵：单块覆盖的 Z 跨度": 10,
  "chunk_matrix_depth": 10,

  "// 选区面积（X*Z）大于该值时才启用分块清理": 1024,
  "chunked_min_area": 1024,

  "// 相邻两个分块之间的间隔秒数": 2,
  "chunk_interval_seconds": 2,

  "// 玩家离开领地时是否自动暂停任务": true,
  "pause_on_player_leave": true,

  "// 玩家离开领地多少秒后暂停任务（仅上一项为 true 时生效）": 15,
  "player_leave_pause_seconds": 15,

  "// 任务暂停多少秒后自动终结（手动暂停同样生效）": 300,
  "pause_auto_cancel_seconds": 300,

  "// 收费规格阶梯：从面积小到大排列，按 X/Z 跨度匹配第一个满足条件的规格": [],
  "price_levels": []
})JSON";

template <class T>
void readIfPresent(nlohmann::json const& j, char const* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return;
    }
    try {
        out = it->get<T>();
    } catch (...) {}
}

} // namespace

bool Config::isScoreboardEconomy() const {
    std::string lower = economyType;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return lower == "scoreboard";
}

PriceLevel const& Config::matchPriceLevel(int width, int depth) const {
    for (auto const& level : priceLevels) {
        if (width <= level.maxWidth && depth <= level.maxDepth) {
            return level;
        }
    }
    auto& fallback = priceLevels.empty() ? makeDefault().priceLevels.front() : priceLevels.back();
    return fallback;
}

bool Config::exceedsLargest(int width, int depth) const {
    if (priceLevels.empty()) {
        return false;
    }
    auto const& last = priceLevels.back();
    return width > last.maxWidth || depth > last.maxDepth;
}

std::vector<std::string> Config::sanitize() {
    std::vector<std::string> fixed;

    auto clampInt = [&](char const* name, int& value, int minimum, int fallbackValue) {
        if (value < minimum) {
            fixed.push_back(std::string(name) + " 非法（" + std::to_string(value) + "），已回退为 "
                            + std::to_string(fallbackValue));
            value = fallbackValue;
        }
    };

    clampInt("global_task_limit", globalTaskLimit, 1, 1);
    clampInt("player_task_limit", playerTaskLimit, 1, 1);
    clampInt("layer_countdown_seconds", layerCountdownSeconds, 1, 10);
    clampInt("chunk_matrix_width", chunkMatrixWidth, 1, 10);
    clampInt("chunk_matrix_depth", chunkMatrixDepth, 1, 10);
    clampInt("chunked_min_area", chunkedMinArea, 1, 1024);
    clampInt("chunk_interval_seconds", chunkIntervalSeconds, 0, 2);
    clampInt("player_leave_pause_seconds", playerLeavePauseSeconds, 0, 15);
    clampInt("pause_auto_cancel_seconds", pauseAutoCancelSeconds, 1, 300);

    if (!isScoreboardEconomy() && economyType != "LLMoney") {
        fixed.push_back("economy_type 仅支持 LLMoney / Scoreboard，已回退为 LLMoney");
        economyType = "LLMoney";
    }
    if (moneyname.empty()) {
        moneyname = "金币";
    }

    if (priceLevels.empty()) {
        fixed.push_back("price_levels 为空，已写入默认 9*9 / 17*17 两档");
        priceLevels = makeDefault().priceLevels;
    }

    for (auto& level : priceLevels) {
        if (level.maxWidth < 1) level.maxWidth = 1;
        if (level.maxDepth < 1) level.maxDepth = 1;
        if (level.pricePerLayer < 0) level.pricePerLayer = 0;
        if (level.levelname.empty()) {
            level.levelname = std::to_string(level.maxWidth) + "*" + std::to_string(level.maxDepth);
        }
    }

    std::stable_sort(priceLevels.begin(), priceLevels.end(), [](PriceLevel const& a, PriceLevel const& b) {
        return (int64_t)a.maxWidth * a.maxDepth < (int64_t)b.maxWidth * b.maxDepth;
    });

    return fixed;
}

Config Config::makeDefault() {
    Config cfg;
    cfg.priceLevels = {
        {"9*9",   9,   9,   1},
        {"17*17", 17,  17,  2},
        {"33*33", 33,  33,  4},
        {"65*65", 65,  65,  8},
    };
    return cfg;
}

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::load(std::filesystem::path const& configDir) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(configDir, ec)) {
        fs::create_directories(configDir, ec);
    }

    mPath = configDir / "config.json";
    mPath.make_preferred();

    auto defaultConfig = Config::makeDefault();

    if (!fs::exists(mPath, ec)) {
        // 首次运行：落盘带中文注释键的默认配置
        ordered_json root = ordered_json::parse(kDefaultConfigText);

        ordered_json levels = ordered_json::array();
        for (auto const& level : defaultConfig.priceLevels) {
            ordered_json item = ordered_json::object();
            item["levelname"]        = level.levelname;
            item["max_width"]        = level.maxWidth;
            item["max_depth"]        = level.maxDepth;
            item["price_per_layer"]  = level.pricePerLayer;
            levels.push_back(item);
        }
        root["price_levels"] = levels;

        std::ofstream out(mPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            log::get().error("无法写入默认配置文件: {}", mPath.string());
            mConfig = defaultConfig;
            return true;
        }
        out << root.dump(2);
        out.close();

        mConfig = defaultConfig;
        log::get().info("已生成默认配置文件: {}", mPath.string());
        return true;
    }

    std::ifstream in(mPath, std::ios::binary);
    if (!in) {
        log::get().error("无法读取配置文件: {}", mPath.string());
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (std::exception const& e) {
        log::get().error("配置文件 JSON 解析失败: {}（将使用内存中的默认配置，原文件不会被覆盖）", e.what());
        mConfig = defaultConfig;
        return true;
    }

    Config cfg = defaultConfig;

    readIfPresent(root, "config_version", cfg.configVersion);
    readIfPresent(root, "economy_type", cfg.economyType);
    readIfPresent(root, "scoreboard_objective", cfg.scoreboardObjective);
    readIfPresent(root, "moneyname", cfg.moneyname);
    readIfPresent(root, "global_task_limit", cfg.globalTaskLimit);
    readIfPresent(root, "player_task_limit", cfg.playerTaskLimit);
    readIfPresent(root, "layer_countdown_seconds", cfg.layerCountdownSeconds);
    readIfPresent(root, "chunked_clean_enabled", cfg.chunkedCleanEnabled);
    readIfPresent(root, "chunk_matrix_width", cfg.chunkMatrixWidth);
    readIfPresent(root, "chunk_matrix_depth", cfg.chunkMatrixDepth);
    readIfPresent(root, "chunked_min_area", cfg.chunkedMinArea);
    readIfPresent(root, "chunk_interval_seconds", cfg.chunkIntervalSeconds);
    readIfPresent(root, "pause_on_player_leave", cfg.pauseOnPlayerLeave);
    readIfPresent(root, "player_leave_pause_seconds", cfg.playerLeavePauseSeconds);
    readIfPresent(root, "pause_auto_cancel_seconds", cfg.pauseAutoCancelSeconds);

    if (root.contains("price_levels") && root["price_levels"].is_array() && !root["price_levels"].empty()) {
        std::vector<PriceLevel> levels;
        for (auto const& item : root["price_levels"]) {
            if (!item.is_object()) {
                continue;
            }
            PriceLevel level;
            readIfPresent(item, "levelname", level.levelname);
            readIfPresent(item, "max_width", level.maxWidth);
            readIfPresent(item, "max_depth", level.maxDepth);
            readIfPresent(item, "price_per_layer", level.pricePerLayer);
            levels.push_back(level);
        }
        if (!levels.empty()) {
            cfg.priceLevels = std::move(levels);
        }
    }

    for (auto const& note : cfg.sanitize()) {
        log::get().warn("配置修正: {}", note);
    }

    mConfig = std::move(cfg);
    log::get().info(
        "配置已加载: 经济={} 全局上限={} 单玩家上限={} 每层倒计时={}s 分块={} 离开暂停={}",
        mConfig.economyType,
        mConfig.globalTaskLimit,
        mConfig.playerTaskLimit,
        mConfig.layerCountdownSeconds,
        mConfig.chunkedCleanEnabled ? "开" : "关",
        mConfig.pauseOnPlayerLeave ? "开" : "关"
    );
    return true;
}

bool ConfigManager::reload() {
    if (mPath.empty()) {
        return false;
    }
    return load(mPath.parent_path());
}

} // namespace ts
