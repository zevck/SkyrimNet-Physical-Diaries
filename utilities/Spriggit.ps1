# Spriggit: the ESP's source is text in spriggit\SkyrimNetPhysicalDiaries (Spriggit YAML), never the
# .esp itself.  Dot-sourced by Build_Local.ps1 and utilities\esp_to_spriggit.ps1.
# See docs/PLUGIN.md.

# Pinned like SkyrimNet's external_versions.json.
$SpriggitVersion = "0.41.0"
$SpriggitPackage = "Spriggit.Yaml.Skyrim"
$SpriggitUrl     = "https://github.com/Mutagen-Modding/Spriggit/releases/download/$SpriggitVersion/SpriggitCLI.zip"
$SpriggitSha256  = "9b1088eed09f3b6b9d30a40b3b0545cea4c84c3f821b512590a60819f6916529"

$PluginName      = "SkyrimNet Physical Diaries.esp"
$PluginSourceDir = Join-Path $PSScriptRoot "..\spriggit\SkyrimNetPhysicalDiaries"

# The CLI: $ConfiguredPath (Build_Config_Local.ps1's $spriggitPath) if set, else
# external\SpriggitCLI-<version>, downloaded and hash-checked on first use.
function Get-SpriggitCli {
    param([string]$ConfiguredPath)
    if ($ConfiguredPath) {
        if (-not (Test-Path -LiteralPath $ConfiguredPath)) { throw "Spriggit CLI not found at '$ConfiguredPath' (`$spriggitPath)." }
        return $ConfiguredPath
    }
    $folder = Join-Path $PSScriptRoot "..\external\SpriggitCLI-$SpriggitVersion"
    $cli = Join-Path $folder "Spriggit.CLI.exe"
    if (Test-Path -LiteralPath $cli) { return $cli }

    Write-Host "Downloading Spriggit CLI $SpriggitVersion..." -ForegroundColor Cyan
    $zip = Join-Path ([System.IO.Path]::GetTempPath()) "SpriggitCLI-$SpriggitVersion.zip"
    Invoke-WebRequest -Uri $SpriggitUrl -OutFile $zip
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    if ($hash -ne $SpriggitSha256) {
        Remove-Item -LiteralPath $zip -Force
        throw "Spriggit CLI download has SHA-256 $hash, expected $SpriggitSha256."
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $folder -Force
    Remove-Item -LiteralPath $zip -Force
    if (-not (Test-Path -LiteralPath $cli)) { throw "Spriggit.CLI.exe isn't where the archive was expected to put it ('$cli')." }
    return $cli
}
