add_rules("mode.debug", "mode.release")

if is_plat("windows") then
    set_toolchains("clang-cl")
end

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")
-- 仅需 LiteLDev 仓库（levilamina / levibuildscript / sqlite3 都在其中）

-- 说明：LeviLamina SDK 版本线需与服务器实际部署的 LL3 保持同一 26.51.x 小版本线。
-- 这里使用 26.51.6（本机已缓存的构建产物）；26.51.7/26.51.8 为同线补丁版本，
-- 可通过 `xmake g --pkg_search` 或修改此处的版本号切换，产物 ABI 兼容。
if is_config("target_type", "server") then
    add_requires("levilamina 26.51.6", {configs = {target_type = "server"}})
else
    add_requires("levilamina 26.51.6", {configs = {target_type = "client"}})
end

-- 强制所有传递依赖使用同一份 levilamina，避免 ABI 混用
add_requireconfs("**.levilamina", {override = true, version = "26.51.6", configs = {target_type = "server"}})

add_requires("levibuildscript")

-- SQLite：任务与账单持久化（静态链接，随插件 DLL 分发，不引入额外运行时 DLL 依赖）
add_requires("sqlite3")

-- 说明：
--   PLand / DebugShape / LegacyMoney 都是运行期插件（plugins/*.dll），本插件不在链接期
--   静态引用它们的导入库，而是启动时通过 ModManagerRegistry + GetProcAddress 动态解析
--   所需导出符号并打印自检报告。这样依赖插件版本迭代时，即使符号缺失也只会降级某项
--   功能，而不会导致本插件 DLL 加载失败（导入表缺失会让整个插件无法加载）。
--   因此这里不再 add_requires(pland/debug_shape/legacymoney)。

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

target("TerritorySweeper")
    set_default(true)
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {
        pluginName = "TerritorySweeper",
        modFile    = "TerritorySweeper.dll"
    })

    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE", "WIN32_LEAN_AND_MEAN")
        set_exceptions("cxx")
        add_cxflags("/utf-8", "/W3")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-microsoft-unqualified-friend",
            "-Wno-deprecated-declarations",
            {tools = {"clang_cl"}}
        )
    end

    -- 仅链接 LeviLamina 与 sqlite3；跨插件调用全部走运行时符号解析（详见 Compat.cpp）
    add_packages("levilamina", "sqlite3")

    -- iListenAttentively：从服务器已安装的 DLL 反向生成导入库（见 tools/gen-ila-lib.ps1），
    -- 使用延迟加载，缺失该插件时本插件仍能正常加载（只是不再拦截液体）
    add_includedirs("third_party/iListenAttentively/src")
    add_linkdirs("third_party/lib")
    add_links("ILA")
    add_ldflags("/DELAYLOAD:iListenAttentively.dll", "/DEFAULTLIB:delayimp.lib")

    set_kind("shared")
    set_languages("c++23")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")

    if is_config("target_type", "server") then
        add_defines("LL_PLAT_S")
    else
        add_defines("LL_PLAT_C")
    end

    after_build(function(target)
        local outputdir = path.join(os.projectdir(), "bin", target:name())
        local langsrc   = path.join(os.projectdir(), "lang")
        if os.isdir(langsrc) then
            os.mkdir(outputdir)
            os.cp(path.join(langsrc, "**"), outputdir)
        end
    end)
