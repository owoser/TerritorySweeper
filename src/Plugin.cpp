#include "Plugin.h"

#include "Commands.h"
#include "Compat.h"
#include "Config.h"
#include "Database.h"
#include "Economy.h"
#include "Global.h"
#include "Interaction.h"
#include "Task.h"
#include "TaskStore.h"

#include "ll/api/mod/RegisterHelper.h"

#include <string>

namespace ts {

Plugin& Plugin::getInstance() {
    static Plugin instance;
    return instance;
}

Plugin::Plugin() : mSelf(*ll::mod::NativeMod::current()) {}

Plugin& getPlugin() { return Plugin::getInstance(); }

namespace log {
ll::io::Logger& get() { return Plugin::getInstance().getSelf().getLogger(); }
} // namespace log

bool Plugin::load() {
    auto& logger = log::get();
    auto  ver    = getSelf().getManifest().version;
    logger.info("TerritorySweeper v{} 正在加载...", ver ? ver->to_string() : std::string{"dev"});

    if (!ConfigManager::getInstance().load(getSelf().getConfigDir())) {
        logger.error("配置文件加载失败，插件将不会启用。");
        return false;
    }
    return true;
}

bool Plugin::enable() {
    auto& logger = log::get();

    // 依赖插件（PLand / DebugShape / LegacyMoney）在 enable 阶段才真正就绪，
    // 因此 API 自检必须放在这里，而不是 load 阶段。
    compat::runStartupSelfCheck();

    if (!compat::PLandCompat::getInstance().available()) {
        logger.warn("PLand 不可用：领地校验无法进行，木棍与 /ts 均会提示“你不在任何领地内”。");
    }
    if (!compat::DebugShapeCompat::getInstance().available()) {
        logger.warn("DebugShape 不可用：选区与任务边框将不会显示（其余功能正常）。");
    }

    economy::EconomyService::getInstance().init();

    // 任务与账单持久化（SQLite）；失败时降级为仅内存模式，不阻断启用
    auto dbPath = getSelf().getDataDir() / "territorysweeper.db";
    if (db::Database::getInstance().open(dbPath) && db::initSchema()) {
        logger.info("任务持久化已就绪: {}", db::Database::getInstance().path());
    } else {
        logger.error("数据库初始化失败，任务与账单将仅在内存中保存（重启后丢失）。");
    }

    std::error_code ec;
    if (auto legacy = getSelf().getDataDir() / "tasks.json"; std::filesystem::exists(legacy, ec)) {
        logger.warn("检测到旧版 JSON 数据文件 {}，现已改用 SQLite，该文件不再读取，可手动删除。", legacy.string());
    }

    TaskManager::getInstance().init();
    command::registerCommands();
    interaction::registerListeners();

    logger.info("TerritorySweeper 已启用。");
    return true;
}

bool Plugin::disable() {
    interaction::unregisterListeners();
    TaskManager::getInstance().shutdown();
    db::Database::getInstance().close();
    log::get().info("TerritorySweeper 已停用。");
    return true;
}

bool Plugin::unload() { return true; }

} // namespace ts

LL_REGISTER_MOD(ts::Plugin, ts::Plugin::getInstance());
