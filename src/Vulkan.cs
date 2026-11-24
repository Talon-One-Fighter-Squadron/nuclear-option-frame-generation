using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

public sealed partial class NuclearDLSS
{
    const string VulkanLibrary="GfxPluginNuclearFSR";
    [DllImport(VulkanLibrary)] static extern IntPtr VulkanSetup(out int first);
    [DllImport(VulkanLibrary)] static extern int VulkanUpscaleAvailable();
    [DllImport(VulkanLibrary)] static extern IntPtr VulkanPack(ref VulkanInputs inputs);
    [DllImport(VulkanLibrary,CharSet=CharSet.Ansi)] static extern int VulkanStatus(out ulong total,out ulong generated,StringBuilder text,int capacity);
    [StructLayout(LayoutKind.Sequential)]
    struct VulkanInputs {
        public IntPtr depth,motion,hudless;
        public int width,height,reset;
        public float near,far,fov,deltaMs;
        public Vector3 position,up,right,forward;
        public IntPtr color,output;public int outWidth,outHeight;public float jitterX,jitterY;
    }
    bool vulkan;
    ulong vkTotal,vkGenerated;
    string vulkanStartupNote;
    int vkWarmWidth,vkWarmHeight;
    // Unity reads this setting before it creates the graphics device. Append to
    // the original bytes so the game's other boot options and encoding survive.
    string ConfigureNextVulkanLaunch() {
        const string key="xrsdk-pre-init-library";
        string path=Path.Combine(Application.dataPath,"boot.config");
        string tmp=path+"."+Guid.NewGuid().ToString("N")+".tmp";
        try {
            foreach(string line in File.ReadAllLines(path)) {
                var pair=line.Split(new[]{'='},2);
                if(pair.Length==2 && pair[0].Trim().Equals(key,StringComparison.OrdinalIgnoreCase))
                    return pair[1].Trim()==VulkanLibrary?null:"Vulkan startup is in use by another graphics plug-in.";
            }
            File.Copy(path,tmp);
            File.AppendAllText(tmp,Environment.NewLine+key+"="+VulkanLibrary+Environment.NewLine,new UTF8Encoding(false));
            File.Replace(tmp,path,null);
            return "Restart once using Steam's Vulkan option to enable FSR frame generation.";
        } catch(IOException){return "Could not update NuclearOption_Data/boot.config for Vulkan frame generation.";}
          catch(UnauthorizedAccessException){return "Vulkan setup needs write access to NuclearOption_Data/boot.config.";}
        finally {if(File.Exists(tmp))File.Delete(tmp);}
    }
    bool StartVulkan(){
        vulkan=true;usingFsr=true;
        vulkanStartupNote=ConfigureNextVulkanLaunch();
        if(vulkanStartupNote!=null)Logger.LogInfo(vulkanStartupNote);
        callback=VulkanSetup(out first);
        if(callback!=IntPtr.Zero)return true;
        status=vulkanStartupNote??"The Unity Vulkan plug-in interface is unavailable.";Logger.LogWarning(status);return false;
    }
    void VulkanUpdate(){
        if(callback==IntPtr.Zero)return;
        max=VulkanStatus(out vkTotal,out vkGenerated,statusText,statusText.Capacity);
        string next=max==0 && vulkanStartupNote!=null?vulkanStartupNote:statusText.ToString();
        if(status!=next){Logger.LogInfo(next);status=next;}
        state=1;features=VulkanUpscaleAvailable()!=0?AdapterFeatures.FsrScaling:0;
        gameForeground=Application.isFocused;
        displayW=Screen.width;displayH=Screen.height;
        sizedMode=ActiveQuality;sizeReady=FsrSize(sizedMode,displayW,displayH,out optimalW,out optimalH);
        if(fg.Value && max>0 && (displayW!=vkWarmWidth || displayH!=vkWarmHeight)){
            vkWarmWidth=displayW;vkWarmHeight=displayH;Issue(1,IntPtr.Zero);
        }
        if(fpsOverlay.Value)UpdateRates();else rateStart=0;
    }
    IntPtr PackVulkan(ref Frame f){
        var v=new VulkanInputs{color=f.color,output=f.output,depth=f.depth,motion=f.motion,hudless=f.hudless,
            width=f.w,height=f.h,outWidth=f.ow,outHeight=f.oh,near=f.near,far=f.far,fov=f.fov,reset=f.reset,
            deltaMs=f.frameTimeMs,jitterX=f.jx,jitterY=f.jy,position=f.pos,up=f.up,right=f.right,forward=f.forward};
        return VulkanPack(ref v);
    }
}