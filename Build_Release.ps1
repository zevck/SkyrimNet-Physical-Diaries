# Release build for Physical Diaries: the archive players install (Data layout, no installer options).
#
#   1. Refuses a working tree with uncommitted changes (a release is a commit), unless -allowDirty.
#   2. Builds everything with .\Build_Local.ps1 -noDeploy (plugin, Papyrus, ESP), unless -skipBuild.
#   3. Stages build\release\stage: the DLL, the ESP, Scripts (each script's .pex), Source\Scripts,
#      Interface\Translations, SKSE\Plugins\PhysicalDiaries\Locales and the SkyrimNet plugin.
#   4. Checks what it staged: every translation and locale with English's keys, every MCM key translated,
#      the SkyrimNet manifest naming the ESP, no 1.x file names.
#   5. Zips it: build\release\Physical Diaries <version>.zip (the version from CMakeLists.txt).
#
# Usage:
#   .\Build_Release.ps1                # build, check, zip
#   .\Build_Release.ps1 -skipBuild     # pack the last build as it is
#   .\Build_Release.ps1 -allowDirty    # a test release from uncommitted work
#
# The outcome is printed and written to %TEMP%\snpd-release-result.json. See docs/DEVELOPMENT.md#releases.

#Requires -Version 7

param(
    [string]$config = "Release",
    [switch]$skipBuild,
    [switch]$allowDirty
)
$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot
Add-Type -AssemblyName System.IO.Compression.FileSystem

$modName = "Physical Diaries"
$espName = "Physical Diaries.esp"
$resultFile = Join-Path $env:TEMP "snpd-release-result.json"
function Complete-Release {
    param([string]$Status, [string]$Message, [string]$Archive = "")
    $color = if ($Status -eq 'SUCCESS') { 'Green' } else { 'Red' }
    Write-Host ""
    Write-Host "==================== RELEASE $Status ====================" -ForegroundColor $color
    if ($Archive) { Write-Host "  Archive: $Archive" -ForegroundColor $color }
    if ($Message) { Write-Host "  $Message" -ForegroundColor $color }
    Write-Host "=========================================================" -ForegroundColor $color
    @{ status = $Status; message = $Message; archive = $Archive; finished = (Get-Date -Format o) } |
        ConvertTo-Json | Set-Content -LiteralPath $resultFile -Encoding UTF8
    exit $(if ($Status -eq 'SUCCESS') { 0 } else { 1 })
}
trap { Complete-Release -Status 'FAILURE' -Message $_.Exception.Message }

# --- 1. A commit ---------------------------------------------------------------
$commit = (git rev-parse --short HEAD).Trim()
$dirty = git status --porcelain
if ($dirty) {
    if (-not $allowDirty) { Complete-Release -Status 'FAILURE' -Message "Uncommitted changes: commit first, or -allowDirty for a test release" }
    Write-Host "Uncommitted changes: a test release, not commit $commit's." -ForegroundColor Yellow
    $commit += "-dirty"
}
$version = (Select-String -Path "CMakeLists.txt" -Pattern '^\s*VERSION\s+([\d.]+)').Matches[0].Groups[1].Value
Write-Host "$modName $version ($commit)" -ForegroundColor Cyan

# --- 2. Build ------------------------------------------------------------------
if (-not $skipBuild) {
    & (Join-Path $PSScriptRoot "Build_Local.ps1") -noDeploy -config $config
    if ($LASTEXITCODE -ne 0) { Complete-Release -Status 'FAILURE' -Message "Build_Local.ps1 failed (see above)" }
}

# --- 3. Stage --------------------------------------------------------------------
$release = Join-Path $PSScriptRoot "build\release"
$stage = Join-Path $release "stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$files = [ordered]@{
    "SKSE\Plugins\PhysicalDiaries.dll" = "build\$config\PhysicalDiaries.dll"
    $espName                           = "build\esp\$espName"
}
# Each script's .pex, not the whole folder: a leftover .pex (an old name) must not ship.
foreach ($psc in Get-ChildItem "Source\Scripts\*.psc") {
    $files["Scripts\$($psc.BaseName).pex"] = "Scripts\$($psc.BaseName).pex"
}
$folders = [ordered]@{
    "Source\Scripts"                                    = "Source\Scripts"
    "Interface\Translations"                            = "Interface\Translations"
    "SKSE\Plugins\PhysicalDiaries\Locales"              = "SKSE\Plugins\PhysicalDiaries\Locales"
    "SKSE\Plugins\SkyrimNet\external\zevick.physical-diaries" = "SKSE\Plugins\SkyrimNet\external\zevick.physical-diaries"
}
foreach ($to in $files.Keys) {
    if (-not (Test-Path -LiteralPath $files[$to])) { throw "Missing $($files[$to]): build first" }
    New-Item -ItemType Directory -Force -Path (Split-Path (Join-Path $stage $to)) | Out-Null
    Copy-Item -LiteralPath $files[$to] -Destination (Join-Path $stage $to)
}
foreach ($to in $folders.Keys) {
    if (-not (Test-Path -LiteralPath $folders[$to])) { throw "Missing $($folders[$to])" }
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $to) | Out-Null
    Copy-Item -Path (Join-Path $folders[$to] "*") -Destination (Join-Path $stage $to) -Recurse
}

# --- 4. Check --------------------------------------------------------------------
$problems = @()

# MCM translations: UTF-16 with English's keys, and every key the MCM uses.
function Get-Keys([string]$path) {
    [IO.File]::ReadAllText($path, [Text.Encoding]::Unicode) -split "`r?`n" |
        Where-Object { $_ -match '^\$' } | ForEach-Object { ($_ -split "`t", 2)[0] }
}
$translations = Join-Path $stage "Interface\Translations"
$english = Get-Keys (Join-Path $translations "$($modName)_ENGLISH.txt")
foreach ($file in Get-ChildItem (Join-Path $translations "*.txt")) {
    $bom = [IO.File]::ReadAllBytes($file.FullName)[0..1]
    if ($bom[0] -ne 0xFF -or $bom[1] -ne 0xFE) { $problems += "$($file.Name) isn't UTF-16 LE with a BOM (Skyrim ignores it)" }
    if (-not $file.Name.StartsWith("$($modName)_")) { $problems += "$($file.Name): not named after $espName (Skyrim loads translations by plugin name)" }
    $diff = Compare-Object $english (Get-Keys $file.FullName)
    if ($diff) { $problems += "$($file.Name): keys differ from English ($(($diff | ForEach-Object InputObject) -join ', '))" }
}
# A key with an argument is written "$KEY{" + value + "}" in Papyrus; its translation key is "$KEY{}".
$used = Select-String -Path "Source\Scripts\*.psc" -Pattern '"(\$SNPD_[A-Za-z0-9_{}]+)' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value -replace '\{$', '{}' } | Sort-Object -Unique
foreach ($key in $used) {
    if ($english -notcontains $key) { $problems += "MCM key $key isn't in $($modName)_ENGLISH.txt" }
}

# Locales: every file with English's [section] keys.
function Get-IniKeys([string]$path) {
    $section = ""
    foreach ($line in Get-Content -LiteralPath $path -Encoding UTF8) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $section = $Matches[1]; continue }
        if ($line -match '^\s*([^;=\s][^=]*?)\s*=') { "$section/$($Matches[1])" }
    }
}
$locales = Join-Path $stage "SKSE\Plugins\PhysicalDiaries\Locales"
$englishIni = Get-IniKeys (Join-Path $locales "ENGLISH.ini")
foreach ($file in Get-ChildItem (Join-Path $locales "*.ini")) {
    $diff = Compare-Object $englishIni (Get-IniKeys $file.FullName)
    if ($diff) { $problems += "Locales\$($file.Name): keys differ from English ($(($diff | ForEach-Object InputObject) -join ', '))" }
}

# The SkyrimNet plugin names our ESP (else NPCs never notice thefts).
$manifest = Get-Content -LiteralPath (Join-Path $stage "SKSE\Plugins\SkyrimNet\external\zevick.physical-diaries\manifest.json") -Raw | ConvertFrom-Json
if (-not ($manifest.mods | Where-Object { $_.file -eq $espName })) { $problems += "The SkyrimNet manifest doesn't name $espName" }

# Nothing under 1.x's names (they'd load beside 2.0's).
foreach ($file in Get-ChildItem $stage -Recurse -File) {
    if ($file.Name -match 'SkyrimNetPhysicalDiaries|SkyrimNet Physical Diaries|SkyrimNetDiaries_') { $problems += "1.x name staged: $($file.FullName.Substring($stage.Length + 1))" }
}
if ($problems) { Complete-Release -Status 'FAILURE' -Message ("Checks failed:`n  - " + ($problems -join "`n  - ")) }

# --- 5. Zip ----------------------------------------------------------------------
$archive = Join-Path $release "$modName $version.zip"
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive, [IO.Compression.CompressionLevel]::Optimal, $false)
$count = (Get-ChildItem $stage -Recurse -File).Count
$size = "{0:N0} KB" -f ((Get-Item -LiteralPath $archive).Length / 1KB)
Complete-Release -Status 'SUCCESS' -Archive $archive -Message "$count files, $size, from $commit"
