using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using BepInEx;
using Mono.Cecil;

public static class NuclearOptionGraphicsPreloader
{
    public static IEnumerable<string> TargetDLLs { get { return new string[0]; } }
    public static void Patch(AssemblyDefinition unused) { }

    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr reserved, uint flags);
    [DllImport("kernel32", CharSet=CharSet.Ansi, ExactSpelling=true)]
    static extern IntPtr GetProcAddress(IntPtr library, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet=CharSet.Unicode)]
    delegate int PrepareGraphics(string runtime, string logs, int preferFsr);

    public static void Initialize()
    {
        if (IntPtr.Size != 8 || !String.Equals(Path.GetFileName(Paths.ExecutablePath),
                "NuclearOption.exe", StringComparison.OrdinalIgnoreCase)) return;
        string bridge = Path.Combine(Paths.GameRootPath, "NuclearOption_Data", "Plugins", "x86_64", "NuclearDLSSBridge.dll");
        IntPtr module = LoadLibraryEx(bridge, IntPtr.Zero, 0x1100);
        if (module == IntPtr.Zero) return;
        IntPtr entry = GetProcAddress(module, "PrepareGamePresentationV2");
        if (entry == IntPtr.Zero) return;
        var prepare = (PrepareGraphics)Marshal.GetDelegateForFunctionPointer(entry, typeof(PrepareGraphics));
        var runtime = Path.Combine(Paths.PluginPath, "NuclearOptionDLSS", "Runtime");
        var settings = new BepInEx.Configuration.ConfigFile(Path.Combine(Paths.ConfigPath,"local.nuclearoption.dlss.cfg"), false);
        settings.SaveOnConfigSet = false;
        var provider = settings.Bind("Graphics",FrameGenerationProvider.Key,"");
        bool fsr = FrameGenerationProvider.Normalize(provider.Value)=="FSR";
        int result = prepare(runtime, Paths.BepInExRootPath, fsr?1:0);
        Console.WriteLine("[Nuclear Option DLSS] Primary presentation setup " + result);
    }
}

