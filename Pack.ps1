# Create the player download from the expected files, even if Package was used for local DLSS testing.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$package = Join-Path $PSScriptRoot 'Package'
$plugin = 'BepInEx/plugins/NuclearOptionDLSS'
$files = [ordered]@{}
foreach ($relative in @(
    'BepInEx/patchers/NuclearOptionDLSS/NuclearOptionDLSS.Preloader.dll'
    'NuclearOption_Data/Plugins/x86_64/NuclearDLSSBridge.dll'
    "$plugin/NuclearOptionDLSS.dll"
    "$plugin/FrameGeneration.Credits.dll"
    "$plugin/Runtime/amd_fidelityfx_loader_dx12.dll"
    "$plugin/Runtime/amd_fidelityfx_framegeneration_dx12.dll"
    "$plugin/Runtime/FidelityFX-license.txt"
    "$plugin/Runtime/MinHook-license.txt"
    "$plugin/Runtime/Streamline-license.txt"
)) {
    $files[$relative] = Join-Path $package $relative
}
$files["$plugin/README.md"] = Join-Path $PSScriptRoot 'README.md'
foreach ($name in 'README.md','FidelityFX-third-party.md','Streamline-third-party.md','Streamline-headers.txt','Unity-Companion.txt') {
    $files["$plugin/Notices/$name"] = Join-Path $PSScriptRoot "notices/$name"
}
foreach ($path in $files.Values) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing release file: $path" }
}

$destination = Join-Path $PSScriptRoot 'NuclearOption-FrameGeneration-0.4.2-win-x64.zip'
$temporary = Join-Path $PSScriptRoot ([Guid]::NewGuid().ToString('N') + '.tmp')
try {
    $archive = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entry in $files.GetEnumerator()) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $entry.Value, $entry.Key, [IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
    if ([IO.File]::Exists($destination)) {
        [IO.File]::Replace($temporary, $destination, [System.Management.Automation.Language.NullString]::Value)
    } else {
        [IO.File]::Move($temporary, $destination)
    }
} finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
Get-FileHash -LiteralPath $destination -Algorithm SHA256
