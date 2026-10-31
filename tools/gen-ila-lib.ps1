# 从服务器上已安装的 iListenAttentively.dll 反向导出符号表（third_party/lib/ILA.def）
#
# 说明（与构建流程的分工）：
#   * third_party/lib/ILA.def  纯文本导出表，**入库跟踪**（.gitignore 已放行该文件）；
#   * third_party/lib/ILA.lib  由 xmake.lua 在配置阶段用 LLVM 自带 llvm-lib 自动从 ILA.def 生成，
#     **不入库**；本脚本也会顺手生成一份供本地排查，但真正让 CI 可用的是 .def 入库。
#   * 因此本脚本只在「iListenAttentively 升级」时需要跑一次：重新导出 .def 并提交，
#     其它机器与 CI 会自动用新导出表生成导入库。
#
# 用法：pwsh -File tools/gen-ila-lib.ps1 [-Dll <路径>] [-OutDir <目录>]
param(
    [string]$Dll = 'D:\MCserver\Master-26.51\plugins\iListenAttentively\iListenAttentively.dll',
    [string]$OutDir = 'third_party/lib'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Dll)) { throw "找不到 iListenAttentively.dll: $Dll" }

$studio = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$dumpbin = Get-ChildItem "$studio\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1

# 优先用 PATH 里的 LLVM（应为 23.x），回退到 VS 自带的 LLVM 组件
$llvmlib = (Get-Command llvm-lib -ErrorAction SilentlyContinue).Source
if (-not $llvmlib) { $llvmlib = Join-Path $studio 'VC\Tools\Llvm\x64\bin\llvm-lib.exe' }

if (-not $dumpbin) { throw '找不到 dumpbin.exe（需要 Visual Studio Build Tools）' }
if (-not (Test-Path $llvmlib)) { throw "找不到 llvm-lib.exe：请安装 LLVM 23 并放到 PATH，或安装 VS BuildTools。当前查找路径：$llvmlib" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exportsFile = Join-Path $OutDir 'exports.txt'
& $dumpbin.FullName /exports $Dll | Out-File -Encoding ascii $exportsFile

$names = @()
foreach ($line in Get-Content $exportsFile) {
    $m = [regex]::Match($line, '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\?\S+?)(?:\s+=|$)')
    if ($m.Success) { $names += $m.Groups[1].Value }
}

if ($names.Count -eq 0) { throw '未从 DLL 中解析到 C++ 导出符号' }

$nl = [string][char]13 + [string][char]10
$defFile = Join-Path $OutDir 'ILA.def'
$content = @('LIBRARY iListenAttentively', 'EXPORTS') + ($names | ForEach-Object { '  ' + $_ })
Set-Content -Path $defFile -Value ($content -join $nl) -Encoding ascii

& $llvmlib /def:$defFile /machine:x64 ("/out:" + (Join-Path $OutDir 'ILA.lib')) | Out-Null

Write-Host ("已导出 {0}\ILA.def（{1} 个导出符号），并生成 ILA.lib 供本地排查" -f $OutDir, $names.Count)
Write-Host "请把 ILA.def 提交到仓库（ILA.lib 不入库，构建时自动生成）。"
