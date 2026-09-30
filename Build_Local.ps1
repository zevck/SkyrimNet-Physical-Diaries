# Incremental plugin build (never /t:Rebuild: it rebuilds all of CommonLib), Pyro, SWFs, ESP, deploy to every instance.
# Switches and config: docs/DEVELOPMENT.md#build.  PASS/FAIL also goes to %TEMP%\snpd-build-result.json.

#Requires -Version 7
# (Pyro logs to stderr, which Windows PowerShell 5.1 turns into a terminating error under "Stop".)

param(
    [string]$preset = "vs2022-windows",
    [string]$config = "Release",
    [int]$threads,
    [switch]$noDeploy,
    [switch]$skipScripts,
    [switch]$skipSwf,
    [switch]$skipEsp,
    [switch]$fresh
)
$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

$target   = "SkyrimNetPhysicalDiaries"
$builtDll = Join-Path $PSScriptRoot "build\$config\$target.dll"
. (Join-Path $PSScriptRoot "utilities\Spriggit.ps1")
$espName  = $PluginName
$builtEsp = Join-Path $PSScriptRoot "build\esp\$espName"

# Folders SNPD owns inside a deployed mod.  Mirrored (/MIR), so a file removed from the repo leaves the deploy too
# (a stale .pex lingering in a mod folder is the kind of bug that goes unnoticed).
$mirroredFolders = @(
    "Scripts",
    "Source\Scripts",
    "SKSE\Plugins\SkyrimNetPhysicalDiaries\Locales",
    "SKSE\Plugins\SkyrimNet\external\zevick.physical-diaries",
    "Interface\Translations"
)

# --- Build-result reporting -------------------------------------------------
$scriptStartTime = Get-Date
$buildResultFile = Join-Path $env:TEMP "snpd-build-result.json"
$script:deployed     = @()
$script:deployFailed = @()

function Write-BuildSummary {
    param([string]$Status, [string]$Stage, [int]$Code = 0, [string]$Message = "")
    $finish  = Get-Date
    $elapsed = [math]::Round(($finish - $scriptStartTime).TotalSeconds, 1)
    $color   = if ($Status -eq 'SUCCESS') { 'Green' } else { 'Red' }

    Write-Host ""
    Write-Host "==================== BUILD $Status ====================" -ForegroundColor $color
    Write-Host ("  Finished: {0}" -f $finish.ToString("yyyy-MM-dd HH:mm:ss")) -ForegroundColor $color
    Write-Host ("  Elapsed:  {0}s" -f $elapsed) -ForegroundColor $color
    if ($Stage)      { Write-Host ("  Stage:    {0}" -f $Stage) -ForegroundColor $color }
    if ($Code -ne 0) { Write-Host ("  ExitCode: {0}" -f $Code) -ForegroundColor $color }
    foreach ($d in $script:deployed)     { Write-Host ("  Deployed: {0}" -f $d) -ForegroundColor $color }
    foreach ($d in $script:deployFailed) { Write-Host ("  FAILED:   {0}" -f $d) -ForegroundColor Red }
    if ($Message)    { Write-Host ("  Detail:   {0}" -f $Message) -ForegroundColor $color }
    Write-Host "=======================================================" -ForegroundColor $color

    $payload = [ordered]@{
        status         = $Status
        stage          = $Stage
        exitCode       = $Code
        message        = $Message
        deployedTo     = $script:deployed
        deployFailed   = $script:deployFailed
        finishedAt     = $finish.ToString("o")
        elapsedSeconds = $elapsed
    }
    try { $payload | ConvertTo-Json | Set-Content -LiteralPath $buildResultFile -Encoding UTF8 } catch { }
}

function Complete-Build {
    param([string]$Status, [string]$Stage, [int]$Code = 0, [string]$Message = "")
    Write-BuildSummary -Status $Status -Stage $Stage -Code $Code -Message $Message
    if ($Status -eq 'SUCCESS') { exit 0 }
    exit ($(if ($Code -ne 0) { $Code } else { 1 }))
}

trap {
    Write-BuildSummary -Status 'FAILURE' -Stage 'exception' -Code 1 -Message $_.Exception.Message
    break
}

# --- Configuration ------------------------------------------------------------
# Machine-specific settings live in the gitignored Build_Config_Local.ps1 (docs/DEVELOPMENT.md#build).
$defaultOutputPath     = ""
$additionalOutputPaths = @()
$ckPath                = ""
$pyroPath              = ""
$ffdecPath             = ""
$swfVariant            = ""
$spriggitPath          = ""
$defaultThreads        = 16
if (Test-Path .\Build_Config_Local.ps1) { . .\Build_Config_Local.ps1 }
if ($env:SNPD_OUTPUT_PATH) { $defaultOutputPath = $env:SNPD_OUTPUT_PATH }
if ($env:SNPD_CK_PATH)     { $ckPath = $env:SNPD_CK_PATH }
if (-not $threads)         { $threads = $defaultThreads }

# --- Configure (only when needed) ---------------------------------------------
# The VS generator's ZERO_CHECK reconfigures itself when CMakeLists changes; configure only on a first build or -fresh.
if ($fresh -or -not (Test-Path "build\CMakeCache.txt")) {
    $cmakeArgs = @("--preset", $preset, "-Wno-dev")
    if ($fresh) { $cmakeArgs += "--fresh" }
    Write-Host "Configuring ($preset)..." -ForegroundColor Cyan
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { Complete-Build -Status 'FAILURE' -Stage 'configure' -Code $LASTEXITCODE }
}

# --- Build (plugin target only, incremental) -----------------------------------
Write-Host "Building $target ($config, $threads threads)..." -ForegroundColor Cyan
& cmake --build build --config $config --target $target --parallel $threads
if ($LASTEXITCODE -ne 0) { Complete-Build -Status 'FAILURE' -Stage 'build' -Code $LASTEXITCODE }
if (-not (Test-Path -LiteralPath $builtDll)) { Complete-Build -Status 'FAILURE' -Stage 'build' -Message "Build reported success but $builtDll is missing." }

# --- Papyrus (Pyro, same project as the VS Code task) ---------------------------
if (-not $skipScripts) {
    if (-not $pyroPath) {
        # Newest papyrus-lang extension install wins (the folder name carries the version).
        $pyroPath = Get-ChildItem "$env:USERPROFILE\.vscode\extensions\joelday.papyrus-lang-vscode-*\pyro\pyro.exe" -ErrorAction SilentlyContinue |
                    Sort-Object { $_.Directory.Parent.LastWriteTime } -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $pyroPath -or -not (Test-Path -LiteralPath $pyroPath)) {
        Complete-Build -Status 'FAILURE' -Stage 'scripts' -Message "pyro.exe not found. Install the papyrus-lang VS Code extension, set `$pyroPath in Build_Config_Local.ps1, or pass -skipScripts."
    }
    if (-not $ckPath -or -not (Test-Path -LiteralPath $ckPath)) {
        Complete-Build -Status 'FAILURE' -Stage 'scripts' -Message "Creation Kit path not set or missing ('$ckPath'). Set `$ckPath in Build_Config_Local.ps1 or SNPD_CK_PATH."
    }
    if (-not (Test-Path skyrimse.ppj)) {
        Complete-Build -Status 'FAILURE' -Stage 'scripts' -Message "skyrimse.ppj not found (it is gitignored - regenerate it with the papyrus-lang 'Generate Project' command)."
    }

    Write-Host "Compiling Papyrus (Pyro, skyrimse.ppj)..." -ForegroundColor Cyan
    $pyroOut = & $pyroPath "skyrimse.ppj" "--game-path" $ckPath 2>&1
    $pyroExit = $LASTEXITCODE
    $pyroErrors = $pyroOut | Where-Object { "$_" -match '(?i)\berror\b|failed' -and "$_" -notmatch '\b0 (error|failed)' }
    if ($pyroExit -ne 0 -or $pyroErrors) {
        $pyroOut | ForEach-Object { Write-Host "  $_" }
        Complete-Build -Status 'FAILURE' -Stage 'scripts' -Code $pyroExit -Message (($pyroErrors | Select-Object -First 3) -join ' | ')
    }
    # Pyro is incremental: "No scripts were compiled." just means nothing changed.
    $pyroSummary = $pyroOut | Where-Object { "$_" -match 'scripts were compiled|Compil\w+ \d+|succeeded' } | Select-Object -Last 1
    if ($pyroSummary) { Write-Host ("  " + ("$pyroSummary" -replace '^.*\[INFO\]\s*', '')) -ForegroundColor DarkGray }

    # Every .psc must have a .pex (the check that would have caught the missing
    # SkyrimNetDiaries_API.pex before v1.0.0 shipped).
    $missing = Get-ChildItem "Source\Scripts\*.psc" | Where-Object { -not (Test-Path -LiteralPath (Join-Path "Scripts" ($_.BaseName + ".pex"))) }
    if ($missing) { Complete-Build -Status 'FAILURE' -Stage 'scripts' -Message ("No .pex for: " + (($missing | ForEach-Object BaseName) -join ', ')) }
}

# --- SWF (JPEXS ffdec-cli): each base swf\<name>\<name>[.<variant>].xml plus our scripts\ -----------------------
# Built only when the base or a script is newer than the output.  See docs/DEVELOPMENT.md#swf.
if (-not $skipSwf -and (Test-Path swf)) {
    foreach ($dir in Get-ChildItem swf -Directory) {
        $scriptsNewest = Get-ChildItem (Join-Path $dir.FullName "scripts") -Recurse -File -ErrorAction SilentlyContinue |
                         Sort-Object LastWriteTime -Descending | Select-Object -First 1
        foreach ($xml in Get-ChildItem $dir.FullName -Filter "$($dir.Name)*.xml" -File) {
            $variant = $xml.BaseName.Substring($dir.Name.Length).TrimStart('.')
            $output  = if ($variant) { "build\variants\$variant\Interface\$($dir.Name).swf" } else { "Interface\$($dir.Name).swf" }
            $sourceTime = @($xml.LastWriteTime, $scriptsNewest.LastWriteTime) | Sort-Object -Descending | Select-Object -First 1
            if ((Test-Path -LiteralPath $output) -and (Get-Item $output).LastWriteTime -ge $sourceTime) { continue }

            if (-not $ffdecPath -or -not (Test-Path -LiteralPath $ffdecPath)) {
                Complete-Build -Status 'FAILURE' -Stage 'swf' -Message "ffdec-cli.exe not found ('$ffdecPath'). Set `$ffdecPath in Build_Config_Local.ps1, or pass -skipSwf."
            }
            Write-Host "Building $output (JPEXS)..." -ForegroundColor Cyan
            $baseSwf = Join-Path $PSScriptRoot "build\swf\$($xml.BaseName)_base.swf"
            New-Item -ItemType Directory -Force -Path (Split-Path $baseSwf), (Split-Path (Join-Path $PSScriptRoot $output)) | Out-Null
            $log = & $ffdecPath -xml2swf $xml.FullName $baseSwf 2>&1
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $baseSwf)) {
                $log | ForEach-Object { Write-Host "  $_" }
                Complete-Build -Status 'FAILURE' -Stage 'swf' -Code $LASTEXITCODE -Message "xml2swf failed for $($xml.Name)"
            }
            $scriptedSwf = Join-Path $PSScriptRoot "build\swf\$($xml.BaseName)_scripted.swf"
            $log = & $ffdecPath -importScript $baseSwf $scriptedSwf $dir.FullName 2>&1
            if ($LASTEXITCODE -ne 0 -or ($log | Where-Object { "$_" -match '(?i)error|exception' })) {
                $log | ForEach-Object { Write-Host "  $_" }
                Complete-Build -Status 'FAILURE' -Stage 'swf' -Code $LASTEXITCODE -Message "importScript failed for $($xml.Name)"
            }
            # Shipped uncompressed: the plugin finds book.swf's marker as plain text (WritingMode.cpp).
            $log = & $ffdecPath -decompress $scriptedSwf (Join-Path $PSScriptRoot $output) 2>&1
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $output)) {
                $log | ForEach-Object { Write-Host "  $_" }
                Complete-Build -Status 'FAILURE' -Stage 'swf' -Code $LASTEXITCODE -Message "decompress failed for $($xml.Name)"
            }
            # What the plugin checks: uncompressed, with the marker.  A stale output passes the steps above.
            $bytes = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $output))
            $text = [System.Text.Encoding]::ASCII.GetString($bytes)
            if (-not $text.StartsWith('FWS') -or -not $text.Contains('BOOKMENU_WRITING_INTERFACE=')) {
                Complete-Build -Status 'FAILURE' -Stage 'swf' -Message "$output isn't uncompressed with the writing marker"
            }
        }
    }
}

# --- ESP (Spriggit) ----------------------------------------------------------------
# The .esp is built from its text source when any source file is newer than it.
if (-not $skipEsp -and (Test-Path -LiteralPath $PluginSourceDir)) {
    $sourceNewest = Get-ChildItem -LiteralPath $PluginSourceDir -Recurse -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not (Test-Path -LiteralPath $builtEsp) -or (Get-Item -LiteralPath $builtEsp).LastWriteTime -lt $sourceNewest.LastWriteTime) {
        try { $spriggitCli = Get-SpriggitCli $spriggitPath }
        catch { Complete-Build -Status 'FAILURE' -Stage 'esp' -Message $_.Exception.Message }
        Write-Host "Building $espName (Spriggit)..." -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path (Split-Path $builtEsp) | Out-Null
        $log = & $spriggitCli convert-to-plugin -i $PluginSourceDir -o $builtEsp 2>&1
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $builtEsp)) {
            $log | ForEach-Object { Write-Host "  $_" }
            Complete-Build -Status 'FAILURE' -Stage 'esp' -Code $LASTEXITCODE -Message "Spriggit convert-to-plugin failed"
        }
        # Spriggit may keep the old timestamp on an unchanged file; the build is current now.
        (Get-Item -LiteralPath $builtEsp).LastWriteTime = Get-Date
    }
}

# --- Deploy ------------------------------------------------------------------
function Deploy-To {
    param([string]$dest)
    Write-Host "Deploying to: $dest" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path (Join-Path $dest "SKSE\Plugins") | Out-Null

    # The DLL is locked while that instance's game is running - report it, but still
    # copy everything else: a SWF change can be tested without restarting.
    $dllLocked = $false
    try {
        Copy-Item -LiteralPath $builtDll -Destination (Join-Path $dest "SKSE\Plugins\$target.dll") -Force
    } catch {
        $dllLocked = $true
    }
    if (-not $skipEsp -and (Test-Path -LiteralPath $builtEsp)) {
        # A deployed .esp newer than the build was edited there (CK, xEdit): overwriting it
        # would lose the edits.  They go back into the source first (esp_to_spriggit.ps1).
        $deployedEsp = Join-Path $dest $espName
        if ((Test-Path -LiteralPath $deployedEsp) -and
            (Get-Item -LiteralPath $deployedEsp).LastWriteTime -gt (Get-Item -LiteralPath $builtEsp).LastWriteTime) {
            $script:deployFailed += "$dest ($espName was edited there; run utilities\esp_to_spriggit.ps1, or delete it to take the build's)"
        } else {
            try { Copy-Item -LiteralPath $builtEsp -Destination $deployedEsp -Force } catch { $dllLocked = $true }
        }
    }
    # Description Framework's config for the partly used inkwells (it reads Data\*_DESC.ini).
    Copy-Item -LiteralPath "SkyrimNet Physical Diaries_DESC.ini" -Destination $dest -Force
    # The shipped SWFs, then the chosen variant's on top ($swfVariant).
    $swfSources = @("Interface")
    if ($swfVariant) { $swfSources += "build\variants\$swfVariant\Interface" }
    foreach ($swfFile in $swfSources | ForEach-Object { Get-ChildItem "$_\*.swf" -ErrorAction SilentlyContinue }) {
        New-Item -ItemType Directory -Force -Path (Join-Path $dest "Interface") | Out-Null
        Copy-Item -LiteralPath $swfFile.FullName -Destination (Join-Path $dest "Interface\$($swfFile.Name)") -Force
    }

    foreach ($folder in $mirroredFolders) {
        robocopy $folder (Join-Path $dest $folder) /MIR /XF meta.ini /NFL /NDL /NJH /NJS /NP /R:2 /W:2 | Out-Null
        # robocopy exit codes 0-7 are success; 8+ is a real failure.
        if ($LASTEXITCODE -ge 8) {
            $script:deployFailed += "$dest (robocopy $folder exit $LASTEXITCODE)"
            return
        }
    }
    if ($dllLocked) {
        $script:deployFailed += "$dest (DLL/ESP locked - is that instance's game running? Other files were updated)"
        return
    }
    $script:deployed += $dest
}

if ($noDeploy) {
    Write-Host "-noDeploy set - skipping deploy." -ForegroundColor DarkGray
} else {
    $targets = @($defaultOutputPath) + $additionalOutputPaths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique
    if (-not $targets) {
        Write-Host "No output paths configured (Build_Config_Local.ps1) - nothing to deploy." -ForegroundColor Yellow
    }
    foreach ($dest in $targets) { Deploy-To $dest }
    if ($script:deployFailed) {
        Complete-Build -Status 'FAILURE' -Stage 'deploy' -Message "$($script:deployFailed.Count) instance(s) not updated."
    }
}

Complete-Build -Status 'SUCCESS' -Stage 'complete'
