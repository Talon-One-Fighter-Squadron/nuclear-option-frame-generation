# Create the player download from the expected files, even if Package was used for local DLSS testing.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$package = Join-Path $PSScriptRoot 'Package'
$plugin = 'BepInEx/plugins/NuclearOptionDLSS'
# SHA-256 of the signedbin files from AMD's v2.4.0 tag.
# https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX/signedbin
$amdRuntime = [ordered]@{
    'amd_fidelityfx_upscaler_vk.dll' = '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'
    'amd_fidelityfx_upscaler_dx12.dll' = 'aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace'
    'amd_fidelityfx_loader_vk.dll' = '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'
    'amd_fidelityfx_framegeneration_vk.dll' = 'd002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4'
    'amd_fidelityfx_loader_dx12.dll' = 'e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb'
    'amd_fidelityfx_framegeneration_dx12.dll' = 'e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6'
}
$files = [ordered]@{}
foreach ($relative in @(
    'BepInEx/patchers/NuclearOptionDLSS/NuclearOptionDLSS.Preloader.dll'
    'NuclearOption_Data/Plugins/x86_64/GfxPluginNuclearFSR.dll'
    'NuclearOption_Data/Plugins/x86_64/NuclearDLSSBridge.dll'
    "$plugin/NuclearOptionDLSS.dll"
    "$plugin/FrameGeneration.Credits.dll"
    foreach ($name in $amdRuntime.Keys) { "$plugin/Runtime/$name" }
    "$plugin/Runtime/FidelityFX-license.txt"
    "$plugin/Runtime/MinHook-license.txt"
    "$plugin/Runtime/Streamline-license.txt"
)) {
    $files[$relative] = Join-Path $package $relative
}
$files["$plugin/README.md"] = Join-Path $PSScriptRoot 'README.md'
$files["$plugin/LICENSE"] = Join-Path $PSScriptRoot 'LICENSE'
foreach ($name in 'README.md','FidelityFX-third-party.md','Streamline-third-party.md','Streamline-headers.txt','Unity-Companion.txt','Vulkan-Headers.txt') {
    $files["$plugin/Notices/$name"] = Join-Path $PSScriptRoot "notices/$name"
}
foreach ($path in $files.Values) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing release file: $path" }
}

$destination = Join-Path $PSScriptRoot 'NuclearOption-FrameGeneration-0.5.2-win-x64.zip'
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
    $archive = [IO.Compression.ZipFile]::OpenRead($temporary)
    try {
        foreach ($name in $amdRuntime.Keys) {
            $stream = $archive.GetEntry("$plugin/Runtime/$name").Open()
            try {
                $digest = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash
                if ($digest -ne $amdRuntime[$name]) { throw "Packaged $name differs from the official AMD FSR SDK 2.4.0 binary." }
            } finally {
                $stream.Dispose()
            }
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
