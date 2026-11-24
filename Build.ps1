param([Parameter(Mandatory)][string]$Game,[Parameter(Mandatory)][string]$Sdk,[Parameter(Mandatory)][string]$UnityHeaders,[Parameter(Mandatory)][string]$MinHook,[Parameter(Mandatory)][string]$FsrSdk)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$Sdk=(Resolve-Path $Sdk).Path
$UnityHeaders=(Resolve-Path $UnityHeaders).Path
$MinHook=(Resolve-Path $MinHook).Path
$fsr=(Resolve-Path (Join-Path $FsrSdk 'Kits/FidelityFX')).Path
$managed=Join-Path $Game 'NuclearOption_Data/Managed'
$core=Join-Path $Game 'BepInEx/core'
$bin=Join-Path $root 'Package/BepInEx/plugins/NuclearOptionDLSS'
$native=Join-Path $root 'Package/NuclearOption_Data/Plugins/x86_64'
New-Item -ItemType Directory -Force $bin,$native | Out-Null
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('NuclearOptionDLSS-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
$vswhere="${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Install Visual Studio x64 C++ build tools'}
$fxc=Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/fxc.exe" | Sort-Object FullName -Descending | Select-Object -First 1
if(!$fxc){throw 'Windows SDK shader compiler fxc.exe required'}
foreach($shader in @(@('Temporal','TemporalInputs'),@('Hud','HudOpacity'),@('Cockpit','CompositeCockpit'))){
    $name=$shader[0]
    & $fxc.FullName /nologo /T cs_5_0 /O3 /E $shader[1] /Fh "$scratch/${name}Bytecode.h" /Vn "${name}Bytecode" "$root/src/$name.hlsl"
    if($LASTEXITCODE){throw "Shader compilation failed for $name"}
}
$batch=Join-Path $scratch 'compile.cmd'
@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /LD /D_WINDOWS /I"$fsr" /I"$Sdk\include" /I"$UnityHeaders" /I"$MinHook\include" /I"$scratch" "$root\src\Bridge.cpp" /Fo"$scratch\Bridge.obj" /link /OUT:"$native\NuclearDLSSBridge.dll" /IMPLIB:"$scratch\Bridge.lib" d3d12.lib dxgi.lib "$Sdk\lib\x64\sl.interposer.lib" /DELAYLOAD:sl.interposer.dll "$MinHook\lib\libMinHook.x64.lib" delayimp.lib user32.lib
"@ | Set-Content $batch -Encoding ascii
& $batch
if($LASTEXITCODE){throw 'Native compilation failed'}
$csc=(Get-ChildItem "$env:ProgramFiles/dotnet/sdk/*/Roslyn/bincore/csc.dll" | Sort-Object FullName -Descending | Select-Object -First 1).FullName
$refs=@('mscorlib','System','System.Core','netstandard','UnityEngine','UnityEngine.CoreModule','UnityEngine.IMGUIModule','UnityEngine.InputLegacyModule','UnityEngine.ScreenCaptureModule','UnityEngine.UIModule','UnityEngine.UI','Unity.TextMeshPro','Assembly-CSharp','Unity.RenderPipelines.Universal.Runtime','Unity.RenderPipelines.Core.Runtime') | ForEach-Object {"/reference:$managed/$_.dll"}
& dotnet $csc /nologo /nostdlib+ /target:library /optimize+ "/out:$bin/NuclearOptionDLSS.dll" @refs "/reference:$core/BepInEx.dll" "/reference:$core/0Harmony.dll" "$root/src/Plugin.cs" "$root/src/GraphicsSettings.cs" "$root/src/FrameGenerationProvider.cs"
if($LASTEXITCODE){throw 'Managed compilation failed'}
$rt=Join-Path $bin 'Runtime'
New-Item -ItemType Directory -Force $rt | Out-Null
Copy-Item "$Sdk/license.txt" "$rt/Streamline-license.txt"
Copy-Item "$root/src/MinHook-license.txt" "$rt/MinHook-license.txt"
foreach($name in 'amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_framegeneration_dx12.dll') {
    $file=Join-Path "$fsr/signedbin" $name
    $signature=Get-AuthenticodeSignature -LiteralPath $file
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Advanced Micro Devices') {throw "AMD signature verification failed for $name"}
    Copy-Item -LiteralPath $file -Destination $rt
}
Copy-Item "$fsr/docs/license.md" "$rt/FidelityFX-license.txt"


$patchers=Join-Path $root 'Package/BepInEx/patchers/NuclearOptionDLSS'
New-Item -ItemType Directory -Force $patchers | Out-Null
& dotnet $csc /nologo /nostdlib+ /target:library /optimize+ "/out:$patchers/NuclearOptionDLSS.Preloader.dll" @refs "/reference:$core/BepInEx.dll" "/reference:$core/Mono.Cecil.dll" "$root/src/EarlyGraphics.cs" "$root/src/FrameGenerationProvider.cs"
if($LASTEXITCODE){throw 'Early graphics plugin compilation failed'}
