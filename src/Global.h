#pragma once

/**
 * TerritorySweeper - 领地分层清扫插件
 *
 * 全局公共头：日志器入口、常用标准库包含。
 * 所有模块统一通过 ts::log::get() 输出日志，前缀由 LevyLamina 的 mod 日志器自动带上。
 */

#include "ll/api/io/Logger.h"

#include <string>

namespace ts {

class Plugin;

[[nodiscard]] Plugin& getPlugin();

/// config.jsonc 里的 debug 开关；关闭时 TS_DBG 完全零成本
[[nodiscard]] bool isDebugEnabled();

namespace log {
[[nodiscard]] ll::io::Logger& get();
}

} // namespace ts

/// 仅在 debug: true 时输出的详细日志（发包、任务推进、交互等）
#define TS_DBG(...)                                                                                                        do {                                                                                                                       if (::ts::isDebugEnabled()) {                                                                                              ::ts::log::get().info(__VA_ARGS__);                                                                                }                                                                                                                  } while (0)
