#pragma once

/**
 * 依赖插件兼容层
 *
 * 设计要点：
 *   PLand / DebugShape / LegacyMoney 都是"运行期插件"，本插件不在链接期引用它们的导入库，
 *   而是启动时通过 ModManagerRegistry 取得模块句柄，再用 GetProcAddress 逐个解析所需的
 *   导出符号，并把结果打印成可读报告。
 *
 *   这样做的好处：依赖插件升级后即使某个符号改名/缺失，本插件 DLL 依然能正常加载，
 *   只是对应功能被安全降级并给出明确告警，而不会出现"导入表缺失 -> 整个插件加载失败"。
 *
 * 注意：以下结构性类型（LandAABB / LandRole / IDrawer / IBoundsBox）按依赖插件的公开
 *      头文件逐字复刻其内存布局与虚表顺序，保证 ABI 一致。
 */

#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Player;

namespace ts::compat {

// ---------------------------------------------------------------------------
// 通用探针
// ---------------------------------------------------------------------------

struct ProbeItem {
    std::string label;
    std::string symbol;
    bool        ok{false};
    void*       address{nullptr};
};

/// 通过 LeviLamina 的 ModManagerRegistry 取得已加载原生插件的模块句柄
class ModuleSymbols {
public:
    bool open(std::string_view modName);
    void close();

    [[nodiscard]] bool                valid() const { return mHandle != nullptr; }
    [[nodiscard]] std::string const&  name() const { return mName; }
    [[nodiscard]] std::string const&  version() const { return mVersion; }
    [[nodiscard]] std::string const&  path() const { return mPath; }
    [[nodiscard]] void*               resolve(std::string_view symbol) const;

    template <class Fn>
    [[nodiscard]] Fn as(std::string_view symbol) const {
        return reinterpret_cast<Fn>(resolve(symbol));
    }

private:
    void*       mHandle{nullptr};
    std::string mName;
    std::string mVersion;
    std::string mPath;
};

// ---------------------------------------------------------------------------
// PLand（领地）
// ---------------------------------------------------------------------------

/// 与 pland::LandPos 布局一致
struct LandPos {
    int x{0};
    int y{0};
    int z{0};
};

/// 与 pland::LandAABB 布局一致（PlaneAABB: min/max 两个 LandPos）
struct LandAABB {
    LandPos min{};
    LandPos max{};
};

/// 与 pland::LandRole 取值一致
enum class LandRole : unsigned char {
    Admin  = 0,
    Owner  = 1,
    Member = 2,
    Actor  = 3,
};

/// 领地快照：持有 shared_ptr 保证查询期间领地对象存活
struct LandSnapshot {
    std::shared_ptr<void> land{};
    int64_t               id{0};
    int                   dimId{0};
    LandAABB              aabb{};
    std::string           name;

    [[nodiscard]] bool valid() const { return land != nullptr; }
    [[nodiscard]] bool contains(int x, int y, int z) const;
    [[nodiscard]] bool contains(BlockPos const& pos) const;
    [[nodiscard]] int  blockCountX() const { return aabb.max.x - aabb.min.x + 1; }
    [[nodiscard]] int  blockCountZ() const { return aabb.max.z - aabb.min.z + 1; }
};

class PLandCompat {
public:
    static PLandCompat& getInstance();

    /// 解析并校验 PLand 导出符号；返回是否可用
    bool probe();

    [[nodiscard]] bool               available() const { return mAvailable; }
    [[nodiscard]] std::string const& version() const { return mSymbols.version(); }
    [[nodiscard]] std::vector<ProbeItem> const& report() const { return mReport; }

    /// 查询坐标所在领地（pos 为方块坐标）
    [[nodiscard]] std::optional<LandSnapshot> landAt(BlockPos const& pos, int dimId) const;

    /// 查询玩家在某领地的角色
    [[nodiscard]] std::optional<LandRole> roleOf(LandSnapshot const& land, mce::UUID const& uuid) const;

    /// 是否为 PLand 全局管理员（operator）
    [[nodiscard]] bool isOperator(mce::UUID const& uuid) const;

private:
    PLandCompat() = default;

    using GetInstanceFn    = void* (*)();
    using GetRegistryFn    = void* (*)(void const*);
    using GetLandAtFn      = void (*)(void const*, std::shared_ptr<void>*, BlockPos const&, int);
    using IsOperatorFn     = bool (*)(void const*, mce::UUID const&);
    using GetRoleFn        = unsigned char (*)(void const*, mce::UUID const&);
    using GetAABBFn        = LandAABB const* (*)(void const*);
    using GetIdFn          = int64_t (*)(void const*);
    using GetDimIdFn       = int (*)(void const*);
    using GetNameFn        = std::string const* (*)(void const*);

    ModuleSymbols mSymbols;
    bool          mAvailable{false};
    std::vector<ProbeItem> mReport;

    GetInstanceFn mGetInstance{nullptr};
    GetRegistryFn mGetRegistry{nullptr};
    GetLandAtFn   mGetLandAt{nullptr};
    IsOperatorFn  mIsOperator{nullptr};
    GetRoleFn     mGetRole{nullptr};
    GetAABBFn     mGetAABB{nullptr};
    GetIdFn       mGetId{nullptr};
    GetDimIdFn    mGetDimId{nullptr};
    GetNameFn     mGetName{nullptr};
};

// ---------------------------------------------------------------------------
// DebugShape（调试线框）
//
// 只复刻 IDrawer 部分的虚表（dtor/draw/remove/update），实际调用仅用到
// draw(Player&) 与 remove(Player&)，因此对上层版本追加新虚函数不敏感。
// ---------------------------------------------------------------------------

class IDrawer {
public:
    virtual ~IDrawer() = default;

    virtual void draw() const                        = 0;
    virtual void draw(Player& player) const          = 0;
    virtual void draw(DimensionType dimension) const = 0;

    virtual void remove() const                        = 0;
    virtual void remove(Player& player) const          = 0;
    virtual void remove(DimensionType dimension) const = 0;

    virtual void update() const                        = 0;
    virtual void update(Player& player) const          = 0;
    virtual void update(DimensionType dimension) const = 0;
};

/// 与 debug_shape::extension::IBoundsBox 前段布局一致
class IBoundsBox : public IDrawer {
public:
    virtual void setBounds(AABB const& bounds) = 0;

    virtual std::optional<Vec3>         getRotation() const      = 0;
    virtual void                        setRotation(std::optional<Vec3> rot) = 0;
    virtual std::optional<mce::Color>   getColor() const         = 0;
    virtual void                        setColor(std::optional<mce::Color> color) = 0;
    virtual bool                        hasDuration() const      = 0;
    virtual std::optional<float>        getTotalTimeLeft() const = 0;
    virtual void                        setTotalTimeLeft(std::optional<float> t) = 0;
    virtual std::optional<DimensionType> getDimensionId() const = 0;
    virtual void                        setDimensionId(std::optional<DimensionType> d) = 0;
};

class DebugShapeCompat {
public:
    static DebugShapeCompat& getInstance();

    bool probe();

    [[nodiscard]] bool               available() const { return mAvailable; }
    [[nodiscard]] std::string const& version() const { return mSymbols.version(); }
    [[nodiscard]] std::vector<ProbeItem> const& report() const { return mReport; }

    [[nodiscard]] std::unique_ptr<IBoundsBox> createBox(AABB const& bounds, mce::Color const& color) const;

private:
    DebugShapeCompat() = default;

    using CreateBoxFn  = std::unique_ptr<IBoundsBox> (*)(AABB const&, mce::Color const&);
    using GetDrawerFn  = IDrawer* (*)();

    ModuleSymbols mSymbols;
    bool          mAvailable{false};
    std::vector<ProbeItem> mReport;

    CreateBoxFn mCreate{nullptr};
    GetDrawerFn mGetDrawer{nullptr};
};

// ---------------------------------------------------------------------------
// LegacyMoney（LLMoney）
// ---------------------------------------------------------------------------

class LLMoneyCompat {
public:
    static LLMoneyCompat& getInstance();

    bool probe();

    [[nodiscard]] bool               available() const { return mAvailable; }
    [[nodiscard]] std::string const& version() const { return mSymbols.version(); }
    [[nodiscard]] std::vector<ProbeItem> const& report() const { return mReport; }

    [[nodiscard]] bool    get(std::string const& xuid, int64_t& out) const;
    [[nodiscard]] bool    reduce(std::string const& xuid, int64_t amount) const;
    [[nodiscard]] bool    add(std::string const& xuid, int64_t amount) const;

private:
    LLMoneyCompat() = default;

    using GetFn    = long long (*)(std::string);
    using ReduceFn = bool (*)(std::string, long long);
    using AddFn    = bool (*)(std::string, long long);

    ModuleSymbols mSymbols;
    bool          mAvailable{false};
    std::vector<ProbeItem> mReport;

    GetFn    mGet{nullptr};
    ReduceFn mReduce{nullptr};
    AddFn    mAdd{nullptr};
};

// ---------------------------------------------------------------------------
// 启动自检报告
// ---------------------------------------------------------------------------

/// 依次探测 PLand / DebugShape / LegacyMoney 并打印控制台报告
void runStartupSelfCheck();

} // namespace ts::compat
