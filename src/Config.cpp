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

    appendComment(out, {"调试模式：开启后输出发包 / 任务推进 / 交互等详细日志",
                        "仅在排查问题时开启，会产生大量控制台输出"});
    appendField(out, "debug", cfg.debugEnabled ? "true" : "false");

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

    appendComment(out, {"是否显示 BossBar 进度条（每层倒计时与整体进度）",
                        "若进服出现异常断开，可先关闭此项排查"});
    appendField(out, "bossbar_enabled", cfg.bossBarEnabled ? "true" : "false");

    appendComment(out, {"BossBar 刷新方式：incremental 只发送变化的字段（省流量）",
                        "recreate 每秒完整重建（兼容性最好，出现显示异常时可切到它）"});
    appendField(out, "bossbar_update_mode", jsonString(cfg.bossBarUpdateMode));

    appendComment(out, {"是否显示选区与任务区域的调试线框（依赖 DebugShape）"});
    appendField(out, "region_frame_enabled", cfg.regionFrameEnabled ? "true" : "false");

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

    auto defaultConfig = Config::makeDefault();

    auto jsoncPath = configDir / "config.jsonc";
    auto jsonPath  = configDir / "config.json";
    jsoncPath.make_preferred();
    jsonPath.make_preferred();

    bool const hasJsonc = fs::exists(jsoncPath, ec);
    bool const hasJson  = fs::exists(jsonPath, ec);

    auto writeText = [](fs::path const& file, std::string const& content) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << content;
        out.close();
        return true;
    };

    auto readText = [](fs::path const& file, std::string& out) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    };

    auto timestamp = [] {
        auto    now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_s(&tmBuf, &now);
        char buf[32]{};
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmBuf);
        return std::string{buf};
    };

    // 两个文件都不存在 -> 全新安装，直接生成 config.jsonc
    if (!hasJsonc && !hasJson) {
        mPath = jsoncPath;
        if (writeText(mPath, renderConfig(defaultConfig))) {
            log::get().info("已生成默认配置文件: {}", mPath.string());
        } else {
            log::get().error("无法写入默认配置文件: {}", mPath.string());
        }
        mConfig = defaultConfig;
        return true;
    }

    if (hasJsonc && hasJson) {
        log::get().warn("同时存在 config.jsonc 与 config.json，只会读取 config.jsonc；config.json 已忽略，可手动删除。");
    }

    // 读取来源：优先 config.jsonc，其次旧版的 config.json
    bool const fromLegacyJson = !hasJsonc && hasJson;
    mPath                     = fromLegacyJson ? jsonPath : jsoncPath;

    std::string text;
    if (!readText(mPath, text)) {
        log::get().error("无法读取配置文件: {}", mPath.string());
        return false;
    }

    // 允许 // 与 /* */ 注释（JSONC）；注释剥离在词法层完成，字符串内的 // 不会被误判
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false, true);
    if (root.is_discarded()) {
        log::get().error("配置文件解析失败（支持 // 注释，但 JSON 结构必须合法），将使用内存中的默认配置，原文件不会被覆盖。");
        mConfig = defaultConfig;
        return true;
    }

    Config cfg = defaultConfig;

    readIfPresent(root, "config_version", cfg.configVersion);
    readIfPresent(root, "debug", cfg.debugEnabled);
    readIfPresent(root, "economy_type", cfg.economyType);
    readIfPresent(root, "scoreboard_objective", cfg.scoreboardObjective);
    readIfPresent(root, "moneyname", cfg.moneyname);
    readIfPresent(root, "global_task_limit", cfg.globalTaskLimit);
    readIfPresent(root, "player_task_limit", cfg.playerTaskLimit);
    readIfPresent(root, "bossbar_enabled", cfg.bossBarEnabled);
    readIfPresent(root, "bossbar_update_mode", cfg.bossBarUpdateMode);
    readIfPresent(root, "region_frame_enabled", cfg.regionFrameEnabled);
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

    // 旧版本用过 "// 说明": 值 的伪装键当注释；检测到就重排成新版 JSONC
    bool legacyPseudoKeys = false;
    if (root.is_object()) {
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (it.key().rfind("//", 0) == 0) {
                legacyPseudoKeys = true;
                break;
            }
        }
    }

    if (fromLegacyJson && !legacyPseudoKeys) {
        // 内容已经是新版 JSONC：直接改后缀，完整保留用户手写的注释与排版
        std::error_code renameEc;
        fs::rename(jsonPath, jsoncPath, renameEc);
        if (!renameEc) {
            mPath = jsoncPath;
            log::get().info("配置文件已更名为 config.jsonc（内容未改动）");
        } else if (writeText(jsoncPath, renderConfig(cfg))) {
            mPath = jsoncPath;
            log::get().warn("config.json 改名失败，已另写出 config.jsonc；config.json 可手动删除。");
        } else {
            log::get().warn("无法生成 config.jsonc，继续使用 config.json 中的配置。");
        }
    } else if (legacyPseudoKeys) {
        auto stamp  = timestamp();
        auto backup = mPath;
        backup += ".bak-" + stamp;

        // 先把原文件改名成带时间戳的备份（失败则退化为拷贝），避免新旧文件并存造成困惑
        std::error_code renameEc;
        fs::rename(mPath, backup, renameEc);
        if (renameEc) {
            std::error_code copyEc;
            fs::copy_file(mPath, backup, fs::copy_options::overwrite_existing, copyEc);
        }

        if (writeText(jsoncPath, renderConfig(cfg))) {
            mPath = jsoncPath;
            log::get().info(
                "配置已迁移为 {}（原文件备份为 {}）",
                jsoncPath.filename().string(),
                backup.filename().string()
            );
        } else {
            mPath = jsoncPath;
            log::get().warn("配置迁移写入失败，继续使用已解析的配置，请检查目录写权限。");
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
