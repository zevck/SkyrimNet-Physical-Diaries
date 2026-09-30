# Converts the ESP (newest copy in the deploy folders, or -EspPath <file>) back into its Spriggit source after a CK
# or xEdit edit.  Commit the source, not the .esp.  See docs/PLUGIN.md.

#Requires -Version 7

param([string]$EspPath)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "Spriggit.ps1")

$defaultOutputPath = ""
$additionalOutputPaths = @()
$spriggitPath = ""
if (Test-Path (Join-Path $root "Build_Config_Local.ps1")) { . (Join-Path $root "Build_Config_Local.ps1") }

if (-not $EspPath) {
    # The CK saves where the plugin was loaded from, so the edited copy is the newest one.
    $candidates = @($defaultOutputPath) + $additionalOutputPaths |
        Where-Object { $_ } |
        ForEach-Object { Join-Path $_ $PluginName } |
        Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { Get-Item -LiteralPath $_ } |
        Sort-Object LastWriteTime -Descending
    if (-not $candidates) { throw "No '$PluginName' in the deploy folders; pass -EspPath." }
    $EspPath = $candidates[0].FullName
}
if (-not (Test-Path -LiteralPath $EspPath)) { throw "'$EspPath' not found." }

$cli = Get-SpriggitCli $spriggitPath
Write-Host "Converting $EspPath"
Write-Host "  ($(Get-Item -LiteralPath $EspPath | ForEach-Object LastWriteTime)) -> spriggit\SkyrimNetPhysicalDiaries" -ForegroundColor DarkGray
& $cli convert-from-plugin -i $EspPath -o $PluginSourceDir -g SkyrimSE -p $SpriggitPackage -v $SpriggitVersion
if ($LASTEXITCODE -ne 0) { throw "Spriggit convert-from-plugin failed ($LASTEXITCODE)." }
Write-Host "Done. Review 'git diff spriggit' and commit the source." -ForegroundColor Green
