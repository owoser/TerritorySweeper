#include "LiquidGuard.h"

#include "Config.h"
#include "Global.h"
#include "Task.h"

#include "ila/event/world/level/block/LiquidFlowEvent.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"

#include "mc/world/level/BlockSource.h"

#include <windows.h>

#include <memory>
#include <vector>

// 延迟加载：服务器未安装 iListenAttentively 时，本插件仍能正常加载（只降级液体拦截）。
// 开关写在 xmake.lua 的 add_shflags("/DELAYLOAD:iListenAttentively.dll") 里：
//   * 不能用 add_ldflags —— @levibuildscript/linkrule 的链接步骤只保留 shflags（它自己也用 shflags
//     注入 /DELAYLOAD:bedrock_runtime.dll），ldflags 不会出现在链接命令行；
//   * 也不能用 #pragma comment(linker, ...) —— /DELAYLOAD 不在 .drectve 支持的选项列表里，会被忽略。

namespace ts::liquid {

namespace {

using LiquidFlowBeforeEvent = ila::mc::world::level::block::LiquidFlowBeforeEvent;

std::vector<std::shared_ptr<ll::event::ListenerBase>> gListeners;

/// 启动自检：确认服务器上确实加载了 iListenAttentively，并打印版本/接口状态
/// @return DLL 是否存在（不存在时调用方必须直接返回，绝不能继续使用延迟加载符号）
bool reportEnvironment() {
    HMODULE module = GetModuleHandleW(L"iListenAttentively.dll");
    if (!module) {
        log::get().warn("[液体拦截] 未检测到 iListenAttentively.dll，液体流入拦截不可用（其它功能不受影响）");
        return false;
    }

    // LiquidFlowBeforeEvent::pos() 的导出符号，用于确认版本匹配
    char const* symbol = "?pos@LiquidFlowBeforeEvent@block@level@world@mc@ila@@QEBAAEAVBlockPos@@XZ";
    auto        proc   = reinterpret_cast<void*>(GetProcAddress(module, symbol));

    log::get().info(
        "[液体拦截] 检测到 iListenAttentively（{}），LiquidFlowBeforeEvent 接口{}",
        proc ? "已加载" : "已加载",
        proc ? "可用" : "缺失，可能是版本不匹配"
    );
    return true;
}

void onLiquidFlow(LiquidFlowBeforeEvent& event) {
    if (!ConfigManager::getInstance().get().blockLiquidInflow) {
        return;
    }

    BlockSource& source = event.blockSource();
    int const    dimId  = static_cast<int>(source.getDimensionId());
    BlockPos&    pos    = event.pos();

    if (!TaskManager::getInstance().isInsideActiveTask(dimId, pos)) {
        return;
    }

    event.cancel();
    TS_DBG("[liquid] 拦截流入 ({}, {}, {}) dim={}", pos.x, pos.y, pos.z, dimId);
}

} // namespace

void registerListener() {
    // 依赖缺失时必须在这里短路：emplaceListener 会首次解析延迟加载符号，
    // 若 iListenAttentively.dll 不存在，延迟加载助手会抛出异常并拖垮插件。
    if (!reportEnvironment()) {
        return;
    }

    if (!gListeners.empty()) {
        return;
    }

    gListeners.emplace_back(
        ll::event::EventBus::getInstance().emplaceListener<LiquidFlowBeforeEvent>(
            [](LiquidFlowBeforeEvent& event) {
                try {
                    onLiquidFlow(event);
                } catch (std::exception const& e) {
                    log::get().error("[液体拦截] 处理异常: {}", e.what());
                } catch (...) {
                    log::get().error("[液体拦截] 处理发生未知异常");
                }
            }
        )
    );

    log::get().info("[液体拦截] 已启用：任务运行期间阻止液体流入清理区域。");
}

void unregisterListener() { gListeners.clear(); }

} // namespace ts::liquid
