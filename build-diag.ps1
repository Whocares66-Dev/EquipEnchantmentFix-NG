# Build EquipEnchantmentFix.dll (Release, Ninja) against C:\Modding\CommonLibSSE-NG and
# C:\vcpkg, then copy the mod layout into .\dist. Imports vcvars64 itself, and restores
# VCPKG_ROOT afterwards (vcvars64 overwrites it with the vcpkg bundled in Visual Studio).
param([switch]$Fresh)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$userVcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }

$output = & ${env:ComSpec} /s /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $output) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$env:VCPKG_ROOT = $userVcpkg

$build = Join-Path $root 'build'
if ($Fresh -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe `
    "-DCMAKE_TOOLCHAIN_FILE=$userVcpkg/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DVCPKG_HOST_TRIPLET=x64-windows-static-md `
    -DCOMMONLIB_SSE_FOLDER=C:/Modding/CommonLibSSE-NG
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$dll = Get-ChildItem $build -Recurse -Filter EquipEnchantmentFix.dll | Select-Object -First 1
if (-not $dll) { throw 'no EquipEnchantmentFix.dll produced' }
$dist = Join-Path $root 'dist\Equip Enchantment Fix NG\SKSE\Plugins'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $dll.FullName $dist -Force
$pdb = Join-Path $dll.DirectoryName 'EquipEnchantmentFix.pdb'
if (Test-Path $pdb) { Copy-Item $pdb $dist -Force }
Write-Host "dist: $dist"
Get-ChildItem $dist | Format-Table Name, Length, LastWriteTime -AutoSize
