using System;

// Both the early swap-chain setup and the Graphics menu read this setting.
internal static class FrameGenerationProvider
{
    internal const string Key="FrameGenerationProvider";
    internal static readonly string[] Names={"DLSS","FSR"};

    internal static string Normalize(string preference)
    {
        if(String.Equals(preference,"DLSS",StringComparison.OrdinalIgnoreCase)) return Names[0];
        if(String.Equals(preference,"FSR",StringComparison.OrdinalIgnoreCase) ||
           String.Equals(preference,"AMD_FSR",StringComparison.OrdinalIgnoreCase)) return Names[1];
        return "";
    }

    internal static int Index(string preference,bool supportsDlssGeneration)
    {
        return supportsDlssGeneration && Normalize(preference)!=Names[1]?0:1;
    }
}
