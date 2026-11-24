param(
    [Parameter(Mandatory)][string]$Game,
    [string]$Destination = "$PSScriptRoot/../Package/BepInEx/plugins/NuclearOptionDLSS"
)

$ErrorActionPreference = 'Stop'
$managed = Join-Path $Game 'NuclearOption_Data/Managed'
$core = Join-Path $Game 'BepInEx/core'
$compiler = Get-ChildItem "$env:ProgramFiles/dotnet/sdk/*/Roslyn/bincore/csc.dll" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$compiler) { throw 'Install a .NET SDK with Roslyn.' }
$references = foreach ($name in 'mscorlib','netstandard','System','System.Core','UnityEngine','UnityEngine.CoreModule','UnityEngine.UI','UnityEngine.UIModule','UnityEngine.TextRenderingModule','Unity.TextMeshPro','Assembly-CSharp') {
    "/reference:$managed/$name.dll"
}
New-Item -Path $Destination -ItemType Directory -Force | Out-Null
& dotnet $compiler.FullName /nologo /nostdlib+ /target:library /optimize+ @references "/reference:$core/BepInEx.dll" "/reference:$core/0Harmony.dll" "/out:$Destination/FrameGeneration.Credits.dll" "$PSScriptRoot/Credits.cs"
if ($LASTEXITCODE) { throw 'Could not build the credits plugin.' }
