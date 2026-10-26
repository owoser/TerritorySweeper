#pragma once

#include "ll/api/mod/NativeMod.h"

namespace ts {

/**
 * 插件主入口（单例）。
 * LeviLamina 通过 LL_REGISTER_MOD 绑定本类的 load/enable/disable/unload 生命周期。
 */
class Plugin {
public:
    static Plugin& getInstance();

    Plugin();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace ts
