#include "Plugin.h"

#include "Compat.h"
#include "Config.h"
#include "Global.h"

#include "ll/api/mod/RegisterHelper.h"

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

    compat::runStartupSelfCheck();
    return true;
}

bool Plugin::enable() {
    log::get().info("TerritorySweeper 已启用。");
    return true;
}

bool Plugin::disable() {
    log::get().info("TerritorySweeper 已停用。");
    return true;
}

bool Plugin::unload() { return true; }

} // namespace ts

LL_REGISTER_MOD(ts::Plugin, ts::Plugin::getInstance());
