#include "Compat.h"

#include "Global.h"

#include "ll/api/mod/ModManagerRegistry.h"
#include "ll/api/mod/NativeMod.h"

#include <Windows.h>

#include <memory>
#include <mutex>

namespace ts::compat {

namespace {

/// PLand 导出符号（取自 PLand.dll 的实际导出表，MSVC x64 修饰名）
constexpr char const* kPLandGetInstance   = "?getInstance@PLand@land@@SAAEAV12@XZ";
constexpr char const* kPLandGetRegistry   = "?getLandRegistry@PLand@land@@QEBAAEAVLandRegistry@2@XZ";
constexpr char const* kRegistryGetLandAt  = "?getLandAt@LandRegistry@land@@QEBA?AV?$shared_ptr@VLand@land@@@std@@"
                                            "AEBVBlockPos@@H@Z";
constexpr char const* kRegistryIsOperator = "?isOperator@LandRegistry@land@@QEBA_NAEBVUUID@mce@@@Z";
/// 0.25.0 中 getPermType 与 getEffectiveRole 返回值均为 LandRole，二者签名相同
constexpr char const* kLandGetPermType    = "?getPermType@Land@land@@QEBA?AW4LandRole@2@AEBVUUID@mce@@@Z";
constexpr char const* kLandGetRole        = "?getEffectiveRole@Land@land@@QEBA?AW4LandRole@2@AEBVUUID@mce@@@Z";
constexpr char const* kLandGetAABB        = "?getAABB@Land@land@@QEBAAEBVLandAABB@2@XZ";
constexpr char const* kLandGetId          = "?getId@Land@land@@QEBA_JXZ";
constexpr char const* kLandGetDimensionId = "?getDimensionId@Land@land@@QEBAHXZ";
constexpr char const* kLandGetName        = "?getName@Land@land@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@"
                                            "V?$allocator@D@2@@std@@XZ";

/// DebugShape 导出符号
constexpr char const* kDebugShapeDrawer =
    "?getInstance@IDebugShapeDrawer@debug_shape@@SAAEAV12@XZ";
constexpr char const* kDebugShapeBoundsBox =
    "?create@IBoundsBox@extension@debug_shape@@SA?AV?$unique_ptr@VIBoundsBox@extension@debug_shape@@"
    "U?$default_delete@VIBoundsBox@extension@debug_shape@@@std@@@std@@AEBVAABB@@AEBVColor@mce@@@Z";

std::mutex& reportMutex() {
    static std::mutex m;
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// ModuleSymbols
// ---------------------------------------------------------------------------

bool ModuleSymbols::open(std::string_view modName) {
    close();

    auto& registry = ll::mod::ModManagerRegistry::getInstance();
    auto  mod      = registry.getMod(std::string{modName});
    if (!mod) {
        return false;
    }
    if (!mod->isEnabled()) {
        log::get().warn("依赖插件 {} 已加载但处于禁用状态", std::string{modName});
        return false;
    }
    if (registry.getModType(std::string{modName}) != ll::mod::NativeModManagerName) {
        return false;
    }

    auto nativeMod = std::static_pointer_cast<ll::mod::NativeMod>(mod);
    auto handle    = reinterpret_cast<HMODULE>(nativeMod->getHandle());
    if (!handle) {
        return false;
    }

    mHandle = handle;
    mName   = mod->getName();
    mPath   = mod->getModDir().string();
    if (auto const& ver = mod->getManifest().version; ver) {
        mVersion = ver->to_string();
    } else {
        mVersion = "unknown";
    }
    return true;
}

void ModuleSymbols::close() {
    mHandle = nullptr;
    mName.clear();
    mVersion.clear();
    mPath.clear();
}

void* ModuleSymbols::resolve(std::string_view symbol) const {
    if (!mHandle || symbol.empty()) {
        return nullptr;
    }
    std::string name{symbol};
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(mHandle), name.c_str()));
}

// ---------------------------------------------------------------------------
// LandSnapshot
// ---------------------------------------------------------------------------

bool LandSnapshot::contains(int x, int y, int z) const {
    return x >= aabb.min.x && x <= aabb.max.x && y >= aabb.min.y && y <= aabb.max.y && z >= aabb.min.z
        && z <= aabb.max.z;
}

bool LandSnapshot::contains(BlockPos const& pos) const { return contains(pos.x, pos.y, pos.z); }

// ---------------------------------------------------------------------------
// PLandCompat
// ---------------------------------------------------------------------------

PLandCompat& PLandCompat::getInstance() {
    static PLandCompat instance;
    return instance;
}

bool PLandCompat::probe() {
    std::lock_guard lock(reportMutex());

    mAvailable = false;
    mReport.clear();
    mGetInstance = nullptr;
    mGetRegistry = nullptr;
    mGetLandAt   = nullptr;
    mIsOperator  = nullptr;
    mGetRole     = nullptr;
    mGetAABB     = nullptr;
    mGetId       = nullptr;
    mGetDimId    = nullptr;
    mGetName     = nullptr;

    if (!mSymbols.open("PLand")) {
        mReport.push_back({"PLand 模块句柄", "", false, nullptr});
        return false;
    }

    auto add = [this](char const* label, char const* symbol, void* addr) {
        mReport.push_back({label, symbol, addr != nullptr, addr});
        return addr;
    };

    // 主入口
    mGetInstance = reinterpret_cast<GetInstanceFn>(add("PLand::getInstance()", kPLandGetInstance,
                                                       mSymbols.resolve(kPLandGetInstance)));
    mGetRegistry = reinterpret_cast<GetRegistryFn>(add("PLand::getLandRegistry()", kPLandGetRegistry,
                                                       mSymbols.resolve(kPLandGetRegistry)));

    // 注册表查询
    mGetLandAt = reinterpret_cast<GetLandAtFn>(add("LandRegistry::getLandAt(BlockPos, dimId)", kRegistryGetLandAt,
                                                   mSymbols.resolve(kRegistryGetLandAt)));
    mIsOperator = reinterpret_cast<IsOperatorFn>(add("LandRegistry::isOperator(UUID)", kRegistryIsOperator,
                                                     mSymbols.resolve(kRegistryIsOperator)));

    // 领地对象
    mGetRole = reinterpret_cast<GetRoleFn>(add("Land::getPermType(UUID)", kLandGetPermType,
                                               mSymbols.resolve(kLandGetPermType)));
    if (!mGetRole) {
        mGetRole = reinterpret_cast<GetRoleFn>(add("Land::getEffectiveRole(UUID)", kLandGetRole,
                                                   mSymbols.resolve(kLandGetRole)));
        // 该符号为备选，若成功则不应视为失败
        if (!mReport.empty() && mReport.back().ok) {
            mReport.back().label += "（备选，替代 getPermType）";
        }
    }
    mGetAABB = reinterpret_cast<GetAABBFn>(add("Land::getAABB()", kLandGetAABB, mSymbols.resolve(kLandGetAABB)));
    mGetId   = reinterpret_cast<GetIdFn>(add("Land::getId()", kLandGetId, mSymbols.resolve(kLandGetId)));
    mGetDimId = reinterpret_cast<GetDimIdFn>(add("Land::getDimensionId()", kLandGetDimensionId,
                                                 mSymbols.resolve(kLandGetDimensionId)));
    mGetName = reinterpret_cast<GetNameFn>(add("Land::getName()", kLandGetName, mSymbols.resolve(kLandGetName)));

    bool allOk = true;
    for (auto const& item : mReport) {
        if (!item.ok) {
            allOk = false;
            break;
        }
    }

    mAvailable = allOk;
    return mAvailable;
}

std::optional<LandSnapshot> PLandCompat::landAt(BlockPos const& pos, int dimId) const {
    if (!mAvailable) {
        return std::nullopt;
    }

    void const* pland = mGetInstance();
    if (!pland) {
        return std::nullopt;
    }
    void const* registry = mGetRegistry(pland);
    if (!registry) {
        return std::nullopt;
    }

    std::shared_ptr<void> land;
    mGetLandAt(registry, &land, pos, dimId);
    if (!land) {
        return std::nullopt;
    }

    LandSnapshot snapshot;
    snapshot.land  = land.get() ? land : nullptr;
    snapshot.id    = mGetId(land.get());
    snapshot.dimId = mGetDimId ? mGetDimId(land.get()) : dimId;

    if (mGetAABB) {
        if (auto* aabb = mGetAABB(land.get()); aabb) {
            snapshot.aabb = *aabb;
        }
    }
    if (mGetName) {
        if (auto* name = mGetName(land.get()); name) {
            snapshot.name = *name;
        }
    }

    snapshot.land = std::move(land);
    return snapshot;
}

std::optional<LandRole> PLandCompat::roleOf(LandSnapshot const& land, mce::UUID const& uuid) const {
    if (!mAvailable || !land.land || !mGetRole) {
        return std::nullopt;
    }
    auto raw = mGetRole(land.land.get(), uuid);
    if (raw > 3) {
        return std::nullopt;
    }
    return static_cast<LandRole>(raw);
}

bool PLandCompat::isOperator(mce::UUID const& uuid) const {
    if (!mAvailable || !mIsOperator) {
        return false;
    }
    void const* pland = mGetInstance();
    if (!pland) {
        return false;
    }
    void const* registry = mGetRegistry(pland);
    if (!registry) {
        return false;
    }
    return mIsOperator(registry, uuid);
}

// ---------------------------------------------------------------------------
// DebugShapeCompat
// ---------------------------------------------------------------------------

DebugShapeCompat& DebugShapeCompat::getInstance() {
    static DebugShapeCompat instance;
    return instance;
}

bool DebugShapeCompat::probe() {
    std::lock_guard lock(reportMutex());

    mAvailable = false;
    mReport.clear();
    mCreate    = nullptr;
    mGetDrawer = nullptr;

    if (!mSymbols.open("DebugShape")) {
        mReport.push_back({"DebugShape 模块句柄", "", false, nullptr});
        return false;
    }

    mGetDrawer = reinterpret_cast<GetDrawerFn>(mSymbols.resolve(kDebugShapeDrawer));
    mReport.push_back({"IDebugShapeDrawer::getInstance()", kDebugShapeDrawer, mGetDrawer != nullptr,
                       reinterpret_cast<void*>(mGetDrawer)});

    mCreate = reinterpret_cast<CreateBoxFn>(mSymbols.resolve(kDebugShapeBoundsBox));
    mReport.push_back({"extension::IBoundsBox::create(AABB, Color)", kDebugShapeBoundsBox, mCreate != nullptr,
                       reinterpret_cast<void*>(mCreate)});

    mAvailable = mGetDrawer && mCreate;
    return mAvailable;
}

std::unique_ptr<IBoundsBox> DebugShapeCompat::createBox(AABB const& bounds, mce::Color const& color) const {
    if (!mAvailable || !mCreate) {
        return nullptr;
    }
    return mCreate(bounds, color);
}

// ---------------------------------------------------------------------------
// LLMoneyCompat
// ---------------------------------------------------------------------------

LLMoneyCompat& LLMoneyCompat::getInstance() {
    static LLMoneyCompat instance;
    return instance;
}

bool LLMoneyCompat::probe() {
    std::lock_guard lock(reportMutex());

    mAvailable = false;
    mReport.clear();
    mGet    = nullptr;
    mReduce = nullptr;
    mAdd    = nullptr;

    if (!mSymbols.open("LegacyMoney")) {
        mReport.push_back({"LegacyMoney 模块句柄", "", false, nullptr});
        return false;
    }

    mGet = reinterpret_cast<GetFn>(mSymbols.resolve("LLMoney_Get"));
    mReport.push_back({"LLMoney_Get(xuid)", "LLMoney_Get", mGet != nullptr, reinterpret_cast<void*>(mGet)});

    mReduce = reinterpret_cast<ReduceFn>(mSymbols.resolve("LLMoney_Reduce"));
    mReport.push_back({"LLMoney_Reduce(xuid, amount)", "LLMoney_Reduce", mReduce != nullptr,
                       reinterpret_cast<void*>(mReduce)});

    // LLMoney_Add 用于终止任务时按比例退款；缺失只影响退款，不影响正常扣款
    mAdd = reinterpret_cast<AddFn>(mSymbols.resolve("LLMoney_Add"));
    mReport.push_back({"LLMoney_Add(xuid, amount)", "LLMoney_Add", mAdd != nullptr,
                       reinterpret_cast<void*>(mAdd)});

    mAvailable = mGet && mReduce;
    return mAvailable;
}

bool LLMoneyCompat::get(std::string const& xuid, int64_t& out) const {
    if (!mAvailable || !mGet) {
        return false;
    }
    out = static_cast<int64_t>(mGet(xuid));
    return true;
}

bool LLMoneyCompat::reduce(std::string const& xuid, int64_t amount) const {
    if (!mAvailable || !mReduce) {
        return false;
    }
    return mReduce(xuid, static_cast<long long>(amount));
}

bool LLMoneyCompat::add(std::string const& xuid, int64_t amount) const {
    if (!mAdd) {
        return false;
    }
    return mAdd(xuid, static_cast<long long>(amount));
}

// ---------------------------------------------------------------------------
// 启动自检
// ---------------------------------------------------------------------------

namespace {

void printItems(ll::io::Logger& logger, std::vector<ProbeItem> const& items) {
    for (auto const& item : items) {
        if (item.ok) {
            logger.info("   [+] {:<46} 正常 (0x{:012X})", item.label, reinterpret_cast<uintptr_t>(item.address));
        } else {
            logger.warn("   [-] {:<46} 缺失（依赖插件可能已更新 API）", item.label);
        }
    }
}

} // namespace

void runStartupSelfCheck() {
    auto& logger = log::get();

    logger.info("================== 依赖插件 API 校验 ==================");

    bool plandOk = PLandCompat::getInstance().probe();
    if (PLandCompat::getInstance().available()) {
        logger.info("正在检查 PLand ...");
        printItems(logger, PLandCompat::getInstance().report());
    } else {
        auto const& report = PLandCompat::getInstance().report();
        if (report.size() == 1 && report.front().symbol.empty()) {
            logger.warn("PLand 未安装或未启用 —— 领地校验与清扫功能不可用。");
        } else {
            logger.error("PLand API 校验未通过（版本不匹配或依赖插件已更新）：");
            printItems(logger, report);
        }
    }

    DebugShapeCompat::getInstance().probe();
    if (DebugShapeCompat::getInstance().available()) {
        logger.info("正在检查 DebugShape ...");
        printItems(logger, DebugShapeCompat::getInstance().report());
    } else {
        auto const& report = DebugShapeCompat::getInstance().report();
        if (report.size() == 1 && report.front().symbol.empty()) {
            logger.warn("DebugShape 未安装或未启用 —— 选区/任务边框可视化将降级为不显示。");
        } else {
            logger.error("DebugShape API 校验未通过：");
            printItems(logger, report);
        }
    }

    LLMoneyCompat::getInstance().probe();
    if (LLMoneyCompat::getInstance().available()) {
        logger.info("正在检查 LegacyMoney ...");
        printItems(logger, LLMoneyCompat::getInstance().report());
    } else {
        auto const& report = LLMoneyCompat::getInstance().report();
        if (report.size() == 1 && report.front().symbol.empty()) {
            logger.warn("LegacyMoney 未安装或未启用 —— 若经济系统配置为 LLMoney，将无法扣款。");
        } else {
            logger.error("LegacyMoney API 校验未通过：");
            printItems(logger, report);
        }
    }

    if (plandOk && DebugShapeCompat::getInstance().available()) {
        logger.info("依赖检查完毕：全部通过，插件核心功能处于安全可用状态！");
    } else {
        logger.warn("依赖检查完毕：存在缺失项，相关功能已自动降级（插件本体仍可正常加载）。");
    }
    logger.info("========================================================");
}

} // namespace ts::compat
