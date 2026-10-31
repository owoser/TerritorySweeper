add_rules("mode.debug", "mode.release")

if is_plat("windows") then
    set_toolchains("clang-cl")
end

-- 本地补丁仓库：把 levilamina 的 on_install 换成「预编译 SDK」下载，免去本机与 CI 每次编译上游本体。
-- 必须放在 levimc-repo 之前：同名包以先注册者为准。
-- 上游 LeviLamina 26.51.9 在 clang-cl 23 下可从源码构建（clang 19 会触发 MinecraftCommands.h
-- 的 incomplete type 错误），因此预编译 SDK 只是构建加速手段，不是绕开上游编译失败的补丁。
-- 细节见 xmake-repo-extra/packages/l/levilamina/xmake.lua。
add_repositories("ts-extra xmake-repo-extra")
add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- 版本号：CI 通过环境变量 TS_VERSION 注入（取自 tag，如 v1.2.0，去掉前导 v）。
-- 本地留空时保持原行为：由 @levibuildscript/modpacker 回退 `git describe --tags --abbrev=0` 推导。
local tsVersion = os.getenv("TS_VERSION")
if tsVersion then
    tsVersion = tsVersion:gsub("^[vV]", "")
end

-- 说明：LeviLamina SDK 版本线需与服务器实际部署的 LL3 保持同一 26.51.x 小版本线。
-- 这里锁定 26.51.9（官方改用 LLVM 23 工具链后的版本，本机与 CI 均已验证），不使用浮动版本，
-- 避免上游小版本升级悄悄改变 ABI / 依赖版本。
if is_config("target_type", "server") then
    add_requires("levilamina 26.51.9", {configs = {target_type = "server"}})
else
    add_requires("levilamina 26.51.9", {configs = {target_type = "client"}})
end

-- 强制所有传递依赖使用同一份 levilamina，避免 ABI 混用
add_requireconfs("**.levilamina", {override = true, version = "26.51.9", configs = {target_type = "server"}})

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

-- ============================================================================
-- 目标定义
-- ============================================================================
--
-- ⚠️ 关于 os API 的作用域限制（xmake 3.x 的项目脚本沙箱）：
--   在本文件「文件作用域」里拿到的 os 表是受限的（只有 getenv / isfile / files / mtime /
--   projectdir / isdir 等只读接口），没有 execv / iorunv / mkdir / cp。而要探测 clang-cl 版本、
--   调用 llvm-lib 生成导入库都需要它们，所以这类逻辑必须写在 on_load 回调「内部」执行。

target("TerritorySweeper")
    set_default(true)
    add_rules("@levibuildscript/linkrule")

    -- modVersion 显式声明：避免 modpacker 在 CI 的浅克隆仓库里回退 git describe 失败或取错 tag
    -- （本地未设置 TS_VERSION 时该键为 nil，仍然走 git describe 回退）。
    -- pluginName / modName 同时提供：不同版本 modpacker 规则的模板变量名不一致。
    add_rules("@levibuildscript/modpacker", {
        pluginName = "TerritorySweeper",
        modName    = "TerritorySweeper",
        modFile    = "TerritorySweeper.dll",
        modVersion = tsVersion
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

    -- iListenAttentively：头文件内置 + 构建期生成导入库，使用延迟加载；
    -- 缺失该插件时本插件仍能正常加载（只是不再拦截液体流入）。
    -- 注意：延迟加载开关**不能**用 add_ldflags 声明 —— @levibuildscript/linkrule 会用自己的
    -- 链接步骤覆盖 ldflags（实测链接命令行里没有 /DELAYLOAD），因此开关写在
    -- src/LiquidGuard.cpp 的 #pragma comment(linker, "/delayload:...") 里。
    add_includedirs("third_party/iListenAttentively/src")
    add_linkdirs("third_party/lib")
    add_links("ILA")
    -- 延迟加载开关必须走 shflags：xmake 的 ldflags 不会进入 @levibuildscript/linkrule 的链接命令行
    -- （该规则自己也是用 shflags 注入 /DELAYLOAD:bedrock_runtime.dll 的）。
    -- delayimp.lib 由 link.exe 在出现 /DELAYLOAD 时自动链接，无需显式声明。
    add_shflags("/DELAYLOAD:iListenAttentively.dll")

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

    on_load(function (target)
        -- ------------------------------------------------------------------
        -- 1) 编译器版本护栏
        -- ------------------------------------------------------------------
        -- LeviLamina 26.51.x 的 MinecraftCommands.h 在 clang 19 上会触发
        --   error: invalid application of 'sizeof' to an incomplete type
        -- 必须用 clang-cl >= 23。VS 2022 自带的 Clang 组件只有 19.1.5，如果 PATH 里先命中了它
        -- （例如在 VS 开发者命令提示符里构建），这里直接给出可读报错，而不是等编译上游头文件才炸。
        if is_plat("windows") then
            local clangCl = nil
            for _, dir in ipairs(path.splitenv(os.getenv("PATH") or "")) do
                local exe = path.join(dir, "clang-cl.exe")
                if os.isfile(exe) then
                    clangCl = exe
                    break
                end
            end
            if clangCl then
                local major = os.iorunv(clangCl, {"--version"}):match("clang version (%d+)")
                if major and tonumber(major) < 23 then
                    raise("LeviLamina 26.51.9 需要 clang-cl >= 23，当前 PATH 里先找到的 clang-cl 是 %s.x（%s）。\n"
                        .. "请安装 LLVM 23（https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.4），\n"
                        .. "并把它的 bin 目录放到 PATH 最前面；VS 2022 自带的 19.1.5 不够。\n"
                        .. "改完 PATH 后必须执行 xmake f -c -y 让 xmake 重新探测工具链。", major, clangCl)
                end
            end
        end

        -- ------------------------------------------------------------------
        -- 2) iListenAttentively 导入库（构建期由入库的 .def 生成，不入库）
        -- ------------------------------------------------------------------
        -- 上游 xmake 源码包会连带编译整个 ILA 并锁死某个补丁版 LeviLamina，与本项目的 SDK 线冲突，
        -- 因此改为「头文件入库 + 导入库构建期生成 + /DELAYLOAD 延迟加载」：
        --   * third_party/lib/ILA.def  纯文本导出表，**入库跟踪**（由 tools/gen-ila-lib.ps1 从测试服
        --     已部署的 iListenAttentively.dll 反向导出；ILA 升级时重新导出并提交）；
        --   * third_party/lib/ILA.lib  由这里用 LLVM 自带的 llvm-lib 从 .def 生成，**不入库**
        --     （已 gitignore），因此 CI 上即使没有测试服 DLL 也能正常链接。
        local libdir  = path.join(os.projectdir(), "third_party", "lib")
        local deffile = path.join(libdir, "ILA.def")
        local libfile = path.join(libdir, "ILA.lib")

        if not os.isfile(deffile) then
            raise("缺少 third_party/lib/ILA.def（iListenAttentively 导出表）。\n"
                .. "请执行 tools/gen-ila-lib.ps1 重新导出该文件并提交到仓库。")
        end

        -- .def 未更新时复用已有 .lib，避免每次配置都刷新 mtime 触发无谓的重链接
        if not os.isfile(libfile) or os.mtime(libfile) < os.mtime(deffile) then
            import("lib.detect.find_program")
            local llvmlib = find_program("llvm-lib")
            if not llvmlib then
                raise("未找到 llvm-lib（LLVM 自带工具）。\n"
                    .. "请安装 LLVM 23.1.4 并把它的 bin 目录放到 PATH 最前面，再执行 xmake f -c -y。")
            end

            os.mkdir(libdir)
            os.execv(llvmlib, {"/def:" .. deffile, "/machine:x64", "/out:" .. libfile})

            if not os.isfile(libfile) then
                raise("生成 third_party/lib/ILA.lib 失败（llvm-lib 未产出文件），请手动执行 tools/gen-ila-lib.ps1。")
            end
            cprint("${bright green}[iListenAttentively]${clear} 已由 ILA.def 生成导入库 ILA.lib")
        end
    end)

    after_build(function(target)
        local outputdir = path.join(os.projectdir(), "bin", target:name())
        local langsrc   = path.join(os.projectdir(), "lang")
        if os.isdir(langsrc) then
            os.mkdir(outputdir)
            os.cp(path.join(langsrc, "**"), outputdir)
        end
    end)
