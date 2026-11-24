# Frame Generation for Nuclear Option

Adds FSR 3.1 temporal upscaling and 2× frame generation to Nuclear Option on Vulkan and DirectX 12. You can change each in **Settings → Graphics**. DirectX 12 also supports optional DLSS and Reflex on compatible GeForce cards.

## Install

1. Close Nuclear Option. In your Steam library, right-click the game and choose **Manage → Browse local files**.
2. If this is a fresh install, extract **BepInEx 5.4.23.5 for Windows x64** here first. You should see `winhttp.dll` beside `NuclearOption.exe`. [Download BepInEx for Windows x64](https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.5/BepInEx_win_x64_5.4.23.5.zip)
3. Extract the mod ZIP directly beside `NuclearOption.exe`. Allow the `BepInEx` and `NuclearOption_Data` folders to merge. FSR 2× is included. For DLSS, Reflex or Multi Frame Generation, add NVIDIA’s runtime as described below before starting the game. [Public releases](https://github.com/Talon-One-Fighter-Squadron/nuclear-option-frame-generation/releases)
4. In your **Steam Library**, right-click **Nuclear Option** and choose **Properties → General**. Under **Launch Options**, open the **Selected Launch Option** dropdown. Steam supplies the graphics launch flag for you. If you previously typed a graphics flag such as `-dx12`, `-force-d3d12`, or `-force-vulkan` in the text box below, remove that flag before switching renderers.

   - **DLSS** — select **Play Nuclear Option (DirectX 12)**. DLSS Super Resolution works on GeForce RTX 20 series and newer. **NVIDIA DLSS Frame Generation** requires an **RTX 40 or 50 series** GPU. **Multi Frame Generation** requires an **RTX 50 series** GPU.
   - **FSR 3.1 and FSR 2x Frame Generation** — select **Play Nuclear Option (Vulkan)**. FSR 2x also works on compatible Radeon cards and GeForce RTX 20 and 30 series cards.

5. Close Properties and click **Play**. On your first Vulkan launch, reach the main menu, exit the game, and click **Play** again with Vulkan selected. This activates the mod's frame-generation startup plug-in.
6. Open **Settings → Graphics** (or press **F8** in flight). Under **Upscaler**, select **NVIDIA DLSS** or **AMD FSR 3.1** and choose a quality preset. Enable **Frame Generation** separately if you want it.

Vulkan uses the AMD upscaler and frame-generation runtime on supported GPUs. The native-resolution cockpit option, NVIDIA features, and Reflex-based refresh cap use DirectX 12. If another plug-in already owns the Vulkan startup entry, the mod keeps that entry and reports the conflict in Graphics.

### Add DLSS and Reflex

1. Close the game. Download NVIDIA’s **Streamline 2.14.16 Windows x64 ZIP**. The tested archive is named `streamline-sdk-v2.14.16.zip`. [Download from NVIDIA](https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip) · [Release and checksum](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16)
2. Open the archive’s `bin/x64/` folder. Copy these **eight DLLs** together, along with the accompanying `nvngx_dlss.license.txt` and `reflex.license.txt`. Use the production files directly in `bin/x64/`.

   ```text
   nvngx_dlss.dll
   nvngx_dlssg.dll
   sl.common.dll
   sl.dlss.dll
   sl.dlss_g.dll
   sl.interposer.dll
   sl.pcl.dll
   sl.reflex.dll
   ```

3. Paste the files **directly inside this existing folder** under the game installation. For example, you should see `Runtime/sl.interposer.dll` alongside the AMD DLLs.

   ```text
   Nuclear Option/BepInEx/plugins/NuclearOptionDLSS/Runtime/
   ```

4. Start the game in DirectX 12. Open **Settings → Graphics** to select the DLSS quality preset or frame-generation provider supported by your GPU. Restart after changing providers. If DLSS is unavailable on a supported card, check that all eight files are in `Runtime` and came from the same archive.

### Which mode to use

| GPU | Frame generation |
| --- | --- |
| GeForce RTX 20 or 30 series | FSR 2×. Add the NVIDIA runtime for DLSS Super Resolution or DLAA. |
| GeForce RTX 40 series | FSR 2×. Add the NVIDIA runtime for DLSS 2×. |
| GeForce RTX 50 series | FSR 2×. Add the NVIDIA runtime for DLSS 2× and Multi Frame Generation. |
| Compatible Radeon | FSR 2× with native rendering or FSR 3.1 upscaling |

The mod picks a provider on first launch. You can toggle frame generation during flight. Switching between DLSS and FSR takes a restart because the provider owns the game's presentation buffers. On NVIDIA, the available multipliers depend on the GPU and driver.

DLSS includes DLAA, Quality, Balanced, Performance and Ultra Performance. With NVIDIA frame generation active, you can choose **Reflex On** or **On + Boost**, or limit output to your monitor's refresh rate. Game settings are saved in `BepInEx/config/local.nuclearoption.dlss.cfg`.

### Uninstall

Close the game and remove these four paths. You can keep your preferences for a later install.

```text
BepInEx/plugins/NuclearOptionDLSS/
BepInEx/patchers/NuclearOptionDLSS/
NuclearOption_Data/Plugins/x86_64/NuclearDLSSBridge.dll
NuclearOption_Data/Plugins/x86_64/GfxPluginNuclearFSR.dll
```

If you used Vulkan, remove the line `xrsdk-pre-init-library=GfxPluginNuclearFSR` from `NuclearOption_Data/boot.config`.

## Build

Use a Windows x64 installation of Nuclear Option with BepInEx 5, Visual Studio C++ Build Tools, the Windows SDK shader compiler and a .NET SDK. The native bridge uses Streamline 2.14.16, AMD FSR SDK 2.4.0, MinHook 1.3.4 (x64 static library) Unity’s native rendering plugin headers and Vulkan-Headers 1.3.239. Set `$vulkanHeaders` to the Vulkan-Headers `include` directory. The build scripts write to `Package/`. The packaging script selects the public release files and adds the README and notices.

```powershell
.\Build.ps1 -Game $game -Sdk $streamline -UnityHeaders $unityHeaders -MinHook $minHook -FsrSdk $fsrSdk -VulkanHeaders $vulkanHeaders
.\credits\Build.ps1 -Game $game
.\Pack.ps1
```

The [dependency notes](notices/README.md) link to the exact SDK releases. This build targets Nuclear Option 0.34.6. Game updates can change the rendering hooks.

## License

The original mod code is available under the [MIT License](LICENSE). You can fork, modify, and distribute it, including commercially. Keep the copyright and license notice with copies or substantial portions of the code.

Nuclear Option and its assets belong to Shockfront Studios. Third-party components are covered by their [own licenses and notices](notices/README.md).

## Game credit

This project is an unofficial community modification and is not affiliated with, sponsored by, or endorsed by Shockfront Studios Pty Ltd. Original Nuclear Option assets, vehicle designs, audio, and code are Copyright (c) 2026 Shockfront Studios Pty Ltd. All rights reserved. Nuclear Option and Shockfront Studios are trademarks or registered trademarks of Shockfront Studios Pty Ltd. Original mod content and all other trademarks belong to their respective owners.
