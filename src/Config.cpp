#include "Config.h"

#include "Global.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>

namespace ts {

namespace {

std::string jsonString(std::string const& value) { return nlohmann::json(value).dump(); }

void appendComment(std::string& out, std::initializer_list<char const*> commentLines) {
    for (auto const* line : commentLines) {
        out += "    // ";
        out += line;
        out += "\n";
    }
}

void appendField(std::string& out, std::string const& key, std::string const& value) {
    out += "    \"";
    out += key;
    out += "\": ";
    out += value;
    out += ",\n\n\n";
}

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

std::string renderConfig(Config const& cfg) {
    std::string out;
    out += "{\n";

    appendComment(out, {"配置文件版本号，请勿手动修改"});
    appendField(out, "config_version", std::to_string(cfg.configVersion));

    appendComment(out, {"经济系统后端",
                        "可选值：LLMoney（使用 LegacyMoney 插件）、Scoreboard（使用原版计分板）"});
    appendField(out, "economy_type", jsonString(cfg.economyType));

    appendComment(out, {"计分板后端绑定的目标名称，仅当 economy_type 为 Scoreboard 时生效",
                        "需提前用 /scoreboard objectives add <名称> dummy \"<显示名>\" 创建该目标"});
    appendField(out, "scoreboard_objective", jsonString(cfg.scoreboardObjective));

    appendComment(out, {"界面文案中显示的货币名称，不影响实际结算"});
    appendField(out, "moneyname", jsonString(cfg.moneyname));

    appendComment(out, {"全服同时存在的清理任务数量上限"});
    appendField(out, "global_task_limit", std::to_string(cfg.globalTaskLimit));

    appendComment(out, {"单个玩家同时存在的清理任务数量上限"});
    appendField(out, "player_task_limit", std::to_string(cfg.playerTaskLimit));

    appendComment(out, {"每层开始清理前的倒计时秒数"});
    appendField(out, "layer_countdown_seconds", std::to_string(cfg.layerCountdownSeconds));

    appendComment(out, {"是否启用分块清理",
                        "启用后大区域会被拆成若干小块分批处理，降低瞬时卡顿"});
    appendField(out, "chunked_clean_enabled", cfg.chunkedCleanEnabled ? "true" : "false");

    appendComment(out, {"分块矩阵：单个分块覆盖的 X 方向跨度（方块数）"});
    appendField(out, "chunk_matrix_width", std::to_string(cfg.chunkMatrixWidth));

    appendComment(out, {"分块矩阵：单个分块覆盖的 Z 方向跨度（方块数）"});
    appendField(out, "chunk_matrix_depth", std::to_string(cfg.chunkMatrixDepth));

    appendComment(out, {"选区水平面积（X 跨度 × Z 跨度）超过该值时才启用分块清理"});
    appendField(out, "chunked_min_area", std::to_string(cfg.chunkedMinArea));

    appendComment(out, {"相邻两个分块之间的等待秒数"});
    appendField(out, "chunk_interval_seconds", std::to_string(cfg.chunkIntervalSeconds));

    appendComment(out, {"领主离开领地范围时是否自动暂停任务"});
    appendField(out, "pause_on_player_leave", cfg.pauseOnPlayerLeave ? "true" : "false");

    appendComment(out, {"领主离开领地多少秒后触发暂停，仅在上一项开启时生效"});
    appendField(out, "player_leave_pause_seconds", std::to_string(cfg.playerLeavePauseSeconds));

    appendComment(out, {"任务暂停多少秒后自动终止并释放名额，手动暂停同样适用"});
    appendField(out, "pause_auto_cancel_seconds", std::to_string(cfg.pauseAutoCancelSeconds));

    appendComment(out, {"清扫收费规格阶梯",
                        "字段说明：",
                        "  levelname       该档位的显示名称",
                        "  max_width       该档位覆盖的最大 X 跨度（含）",
                        "  max_depth       该档位覆盖的最大 Z 跨度（含）",
                        "  price_per_layer 该档位的每层单价",
                        "按面积从小到大排列；选区按自身 X/Z 跨度依次匹配第一个满足条件的档位"});
    out += "    \"price_levels\": [\n";

    for (size_t i = 0; i < cfg.priceLevels.size(); ++i) {
        auto const& level = cfg.priceLevels[i];
        out += "\n\n";
        out += "        // 第 " + std::to_string(i + 1) + " 档\n";
        out += "        {\n";
        out += "            \"levelname\": " + jsonString(level.levelname) + ",\n";
        out += "            \"max_width\": " + std::to_string(level.maxWidth) + ",\n";
        out += "            \"max_depth\": " + std::to_string(level.maxDepth) + ",\n";
        out += "            \"price_per_layer\": " + std::to_string(level.pricePerLayer) + "\n";
        out += "        }";
        if (i + 1 < cfg.priceLevels.size()) {
            out += ",";
        }
        out += "\n";
    }

    out += "\n\n    ]\n}\n";
    return out;
}


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
        // 首次运行：按默认值渲染带注释的配置文件
        std::ofstream out(mPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            log::get().error("无法写入默认配置文件: {}", mPath.string());
            mConfig = defaultConfig;
            return true;
        }
        out << renderConfig(defaultConfig);
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

    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    // 允许 // 与 /* */ 注释（JSONC）；注释剥离在词法层完成，字符串内的 // 不会被误判
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false, true);
    if (root.is_discarded()) {
        log::get().error("配置文件解析失败（支持 // 注释，但 JSON 结构必须合法），将使用内存中的默认配置，原文件不会被覆盖。");
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

    // 旧版本把注释写成 "// xxx": value 这样的伪装键；检测到该格式时迁移为新版 JSONC 排版
    bool legacyCommentStyle = false;
    if (root.is_object()) {
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (it.key().rfind("//", 0) == 0) {
                legacyCommentStyle = true;
                break;
            }
        }
    }

    if (legacyCommentStyle) {
        auto stamp = [] {
            auto    now = std::time(nullptr);
            std::tm tmBuf{};
            localtime_s(&tmBuf, &now);
            char buf[32]{};
            std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmBuf);
            return std::string{buf};
        }();

        auto backup = mPath;
        backup += ".bak-" + stamp;

        std::error_code copyEc;
        std::filesystem::copy_file(mPath, backup, std::filesystem::copy_options::overwrite_existing, copyEc);

        std::ofstream out(mPath, std::ios::binary | std::ios::trunc);
        if (!copyEc && out) {
            out << renderConfig(cfg);
            out.close();
            log::get().info(
                "检测到旧版注释键格式，已自动迁移为新版 JSONC 注释（原文件备份为 {}）",
                backup.filename().string()
            );
        } else {
            log::get().warn("旧版配置迁移失败（备份或写入出错），继续使用已解析的配置，原文件保持不变。");
        }
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
