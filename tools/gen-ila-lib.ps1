# 从服务器上已安装的 iListenAttentively.dll 反向生成导入库（third_party/lib/ILA.lib）
# 用法：pwsh -File tools/gen-ila-lib.ps1 [-Dll <路径>]
param(
    [string]$Dll = 'D:\MCserver\Master-26.51\plugins\iListenAttentively\iListenAttentively.dll',
    [string]$OutDir = 'third_party/lib'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Dll)) { throw "找不到 iListenAttentively.dll: $Dll" }

$studio = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$dumpbin = Get-ChildItem "$studio\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
$llvmlib = Join-Path $studio 'VC\Tools\Llvm\x64\bin\llvm-lib.exe'

if (-not $dumpbin) { throw '找不到 dumpbin.exe（需要 Visual Studio Build Tools）' }
if (-not (Test-Path $llvmlib)) { throw "找不到 llvm-lib.exe: $llvmlib" }

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

Write-Host ("已生成 {0}\ILA.lib（{1} 个导出符号）" -f $OutDir, $names.Count)
