#pragma once

namespace ts::interaction {
/// 注册所有事件监听（木棍交互 / 玩家上下线 / 每秒驱动）
void registerListeners();
void unregisterListeners();
}
