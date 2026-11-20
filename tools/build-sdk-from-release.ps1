<#
.SYNOPSIS
  从上游 release 反向合成 LeviLamina 预编译 SDK（include/ + lib/），供 xmake 预编译包分发。

.DESCRIPTION
  26.60 起官方 release 资产只有 LeviLamina.dll/.pdb/manifest.json，不再附带头文件与导入库。
  本脚本按官方 tag 还原一份可直接用于编译的 SDK：
    1) sparse clone 上游 tag 的 src/ 与 src-server/（只取源码树，不取测试与 CI 资源）
    2) 两层 overlay 出 include/：src/mc + src-server/mc、src/ll + src-server/ll（后者覆盖前者）
    3) 从官方 release 的 LeviLamina.dll 导出表生成 lib/LeviLamina.def + LeviLamina.lib
    4) 打包成 bin/levilamina-sdk-<版本>-server.zip 并打印 sha256

  产物与 tools/build-sdk.ps1 的输出同名同结构，包定义无需区分来源。

.EXAMPLE
  pwsh tools/build-sdk-from-release.ps1 -Version 26.60.8
  pwsh tools/build-sdk-from-release.ps1 -Version 26.60.8 -OutputDir D:\tmp
  pwsh tools/build-sdk-from-release.ps1 -Version 26.60.8 -DllPath D:\tmp\LeviLamina.dll   # 复用已下载的 DLL
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Version,
    [string] $Repo = "LiteLDev/LeviLamina",
    [string] $AssetMatch = "Server-Windows-x64.zip",
    [string] $DllPath = "",
    [string] $LlvmBin = "",
    [string] $WorkDir = "$PSScriptRoot\..\build\_sdkenv",
    [string] $OutputDir = "$PSScriptRoot\..\bin"
)

$ErrorActionPreference = "Stop"

function Resolve-LlvmTool([string] $name) {
    if ($LlvmBin) {
        $p = Join-Path $LlvmBin $name
        if (Test-Path $p) { return $p }
        throw "在 -LlvmBin 指定的目录里找不到 $name：$LlvmBin"
    }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\LLVM-23.1.4\LLVM\bin"),
        (Join-Path $env:ProgramFiles "LLVM\bin")
    )
    foreach ($dir in $candidates) {
        $p = Join-Path $dir $name
        if (Test-Path $p) { return $p }
    }
    throw "找不到 $name：请安装 LLVM 23（https://github.com/llvm/llvm-project/releases）或用 -LlvmBin 指定其 bin 目录。"
}

$readobj = Resolve-LlvmTool "llvm-readobj.exe"
$llvmlib = Resolve-LlvmTool "llvm-lib.exe"

$stage = Join-Path $WorkDir "sdk-$Version"
$src   = Join-Path $WorkDir "llsrc"

# ---------------------------------------------------------------------------
# 1) sparse clone 上游 tag 的 src/ 与 src-server/
# ---------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $src ".git"))) {
    Write-Host "[1/4] sparse clone $Repo v$Version ..."
    New-Item -ItemType Directory -Force $WorkDir | Out-Null
    git clone --filter=blob:none --no-checkout --depth 1 --branch "v$Version" "https://github.com/$Repo.git" $src
} else {
    Write-Host "[1/4] 复用已有源码树 $src（如需换版本请先删除）"
}
git -C $src sparse-checkout init --cone 2>$null | Out-Null
git -C $src sparse-checkout set src src-server
git -C $src checkout --quiet

$headTag = (git -C $src describe --tags --always).Trim()
Write-Host "      源码 tag = $headTag"

# ---------------------------------------------------------------------------
# 2) 两层 overlay 出 include/
# ---------------------------------------------------------------------------
Write-Host "[2/4] 生成 include/（src + src-server overlay）..."
$inc = Join-Path $stage "include"
if (Test-Path $inc) { Remove-Item -Recurse -Force $inc }
New-Item -ItemType Directory -Force $inc | Out-Null

foreach ($layer in @("src", "src-server")) {
    foreach ($sub in @("mc", "ll")) {
        $dir = Join-Path $src (Join-Path $layer $sub)
        if (Test-Path $dir) {
            Copy-Item -Recurse -Force $dir $inc
            Write-Host "      overlay $layer/$sub"
        }
    }
}

# 关键：include/ll 也必须两层 overlay，漏掉 src-server/ll 会出现
#   fatal error: ll/api/service/TargetedBedrock.h file not found
$probe = Join-Path $inc "ll\api\service\TargetedBedrock.h"
if (-not (Test-Path $probe)) {
    throw "overlay 结果缺少 include/ll/api/service/TargetedBedrock.h，请检查 src-server 是否被 sparse-checkout 拉下来。"
}

# ---------------------------------------------------------------------------
# 3) 从官方 release DLL 生成 lib/
# ---------------------------------------------------------------------------
Write-Host "[3/4] 生成 lib/（从官方 release DLL 的导出表）..."
$dllDir = Join-Path $WorkDir "ll-$Version-dll"
if ($DllPath) {
    $dll = Get-Item $DllPath
} else {
    if (-not (Get-ChildItem $dllDir -Recurse -Filter "LeviLamina.dll" -ErrorAction SilentlyContinue)) {
        New-Item -ItemType Directory -Force $dllDir | Out-Null
        $rel = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/tags/v$Version"
        $asset = $rel.assets | Where-Object { $_.name -like "*$AssetMatch*" } | Select-Object -First 1
        if (-not $asset) { throw "release v$Version 里找不到匹配 *$AssetMatch* 的资产。" }
        $zip = Join-Path $dllDir $asset.name
        Write-Host "      下载 $($asset.name) ..."
        Invoke-WebRequest $asset.browser_download_url -OutFile $zip
        Expand-Archive -Path $zip -DestinationPath $dllDir -Force
    }
    $dll = Get-ChildItem $dllDir -Recurse -Filter "LeviLamina.dll" | Select-Object -First 1
}
if (-not $dll) { throw "找不到 LeviLamina.dll" }
Write-Host "      DLL = $($dll.FullName) ($($dll.Length) B)"

$libDir = Join-Path $stage "lib"
if (Test-Path $libDir) { Remove-Item -Recurse -Force $libDir }
New-Item -ItemType Directory -Force $libDir | Out-Null
$def = Join-Path $libDir "LeviLamina.def"
$lib = Join-Path $libDir "LeviLamina.lib"

$dump = & $readobj --coff-exports $dll.FullName
$names = @()
foreach ($line in $dump) {
    if ($line -match "^\s*Name:\s*(\S+)\s*$") { $names += $matches[1] }
}
$names = $names | Where-Object { $_ } | Select-Object -Unique
if ($names.Count -lt 100) { throw "导出表解析异常：只得到 $($names.Count) 个符号。" }
Write-Host "      导出符号数 = $($names.Count)"

"LIBRARY $($dll.BaseName)" | Set-Content -Encoding ascii $def
"EXPORTS" | Add-Content -Encoding ascii $def
$names | Add-Content -Encoding ascii $def
& $llvmlib "/def:$def" "/machine:x64" "/out:$lib" | Out-Null
if (-not (Test-Path $lib)) { throw "llvm-lib 生成 $lib 失败" }
Write-Host "      $lib ($((Get-Item $lib).Length) B)"

$readme = @"
LeviLamina SDK (prebuilt for xmake) - $Version / server

include/  upstream source tree of tag v$Version ($Repo), src/ overlaid with src-server/.
lib/      generated from the export table of the official release LeviLamina.dll.
"@
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding utf8

# ---------------------------------------------------------------------------
# 4) 打包
# ---------------------------------------------------------------------------
Write-Host "[4/4] 打包 zip ..."
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$zipOut = Join-Path $OutputDir "levilamina-sdk-$Version-server.zip"
if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipOut -CompressionLevel Optimal
$hash = (Get-FileHash $zipOut -Algorithm SHA256).Hash.ToLower()

Write-Host ""
Write-Host "产物: $zipOut ($((Get-Item $zipOut).Length) B)"
Write-Host "sha256: $hash"
Write-Host ""
Write-Host "下一步："
Write-Host "  1) gh release create sdk-v$Version `"$zipOut`" -R owoser/TerritorySweeper --prerelease --title `"LeviLamina $Version SDK (prebuilt)`""
Write-Host "  2) 把 $Version 与上面的 sha256 写回 xmake-repo-extra/packages/l/levilamina/xmake.lua 的 TS_LL_VERSION / TS_LL_SHA256"
Write-Host "  3) 同步 xmake.lua 的 add_requires / add_requireconfs 版本号"
