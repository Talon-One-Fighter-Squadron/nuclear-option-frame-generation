using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.LowLevel;
using UnityEngine.Experimental.Rendering;

[BepInPlugin("local.nuclearoption.dlss","Nuclear Option DLSS","0.5.2")]
[BepInProcess("NuclearOption.exe")]
[DefaultExecutionOrder(-31000)]
public sealed partial class NuclearDLSS : BaseUnityPlugin
{
    const string Native="NuclearDLSSBridge";
    [DllImport(Native,CharSet=CharSet.Unicode)] static extern IntPtr Setup(string lib,string logs,out int first);
    [DllImport(Native)] static extern IntPtr Pack(ref Frame f);
    [DllImport(Native)] static extern int OptimalSize(int mode,int ow,int oh,out int w,out int h);
    [DllImport(Native,CharSet=CharSet.Ansi)] static extern void Status(out int state,out int max,out int dyn,out int frames,out int sr,StringBuilder text,int length);
    [DllImport(Native)] static extern ulong TotalPresents();
    [DllImport(Native)] static extern int UsingFsr();
    [DllImport(Native)] static extern int GetAdapterFeatures();
    [DllImport(Native)] static extern int HasReflex();
    [DllImport(Native)] static extern int GameIsForeground();
    [DllImport(Native)] static extern void Marker(int frame,int phase);
    [DllImport(Native)] static extern int BeginReflexFrame(int frame,int boost,uint frameLimitUs);
    [DllImport(Native)] static extern void PauseReflexInput();
    [DllImport(Native)] static extern void ReleaseReflexInput();
    [StructLayout(LayoutKind.Sequential)]
    struct Frame
    {
        public int index,mode,generated,reset,w,h,ow,oh;
        public float jx,jy,near,far,fov,aspect;
        // Unity stores columns contiguously. These bytes represent the transposed,
        // row-vector matrices required by Streamline.
        public Matrix4x4 proj,inverse,clipToPrev,prevToClip;
        public Vector3 pos,up,right,forward;
        public IntPtr color,depth,motion,output,hudless,final;
        public IntPtr prevColor,beforeCockpit,afterCockpit,cockpitDepth,cockpitMotion,mergedDepth,backwardMotion,ui;
        public float cockpitNear,cockpitFar,deltaJitterX,deltaJitterY;
        public int hasCockpit,flags;
        public IntPtr nativeCockpit,detailed,coverage,worldInput,worldMotion;
        public float frameTimeMs;
    }
    enum Quality { Off=0, Performance=1, Balanced=2, Quality=3, UltraPerformance=4, DLAA=6 }
    static readonly Quality[] modes={Quality.Off,Quality.DLAA,Quality.Quality,Quality.Balanced,Quality.Performance,Quality.UltraPerformance};
    static NuclearDLSS self;
    enum FsrPreset {Off=0,Performance=1,Balanced=2,Quality=3,UltraPerformance=4,NativeAA=6}
    ConfigEntry<Quality> quality;
    ConfigEntry<FsrPreset> fsrQuality;
    ConfigEntry<bool> preferFsrScaling;
    bool HasDlssScaling {get{return (features&AdapterFeatures.DLSSSuperResolution)!=0;}}
    bool HasFsrScaling {get{return (features&AdapterFeatures.FsrScaling)!=0;}}
    bool FsrScaling {get{return HasFsrScaling && (!HasDlssScaling || preferFsrScaling.Value);}}
    bool NativePanelEnabled {get{return !vulkan && nativePanel.Value;}}
    void SelectQuality(int index){
        if(index<0 || index>=modes.Length)return;
        if(FsrScaling)fsrQuality.Value=(FsrPreset)modes[index];else quality.Value=modes[index];
    }
    static bool FsrSize(Quality mode,int width,int height,out int w,out int h){
        float scale=mode==Quality.Quality?1.5f:mode==Quality.Balanced?1.7f:mode==Quality.Performance?2f:mode==Quality.UltraPerformance?3f:1f;
        w=Math.Max(1,(int)(width/scale));h=Math.Max(1,(int)(height/scale));
        return mode!=Quality.Off && width>0 && height>0;
    }
    ConfigEntry<bool> fg,mfg,dynamic,nativePanel,forceSecondGen,reflexBoost;
    ConfigEntry<bool> capToRefresh;
    // Bits shared with Bridge.cpp, queried from Unity's actual D3D12 adapter.
    [Flags] enum AdapterFeatures {Detected=1, DLSSSuperResolution=2, DLSSFrameGeneration=4,FsrScaling=32}
    AdapterFeatures features;
    ConfigEntry<string> provider;
    string startupPreference;
    bool CanUpscale {get{return HasDlssScaling || HasFsrScaling;}}
    bool CanSelectDLSS {get{return (features&AdapterFeatures.DLSSFrameGeneration)!=0;}}
    Quality ActiveQuality {get{return FsrScaling?(Quality)fsrQuality.Value:HasDlssScaling?quality.Value:Quality.Off;}}
    int ProviderIndex(string preference) {return FrameGenerationProvider.Index(preference,CanSelectDLSS);}
    bool usingFsr,hasReflex,fsrWarmRequested;
    static readonly string[] providers=FrameGenerationProvider.Names;
    string ProviderNote {get{
        if(vulkan)return status;
        if(state!=1) return null;
        if(ProviderIndex(provider.Value)!=ProviderIndex(startupPreference)) return "Restart Nuclear Option to use "+providers[ProviderIndex(provider.Value)]+".";
        if(max<1) return usingFsr?status:"Frame generation unavailable on this GPU or driver";
        return usingFsr?"Active provider: AMD FSR 3.1.6":"Active provider: NVIDIA DLSS";
    }}
    const string RefreshCapTitle="Limit FPS to Monitor Refresh";
    string refreshCapLabel=RefreshCapTitle;
    double nextRefreshCheck,monitorHz;
    uint displayPeriodUs;
    int reflexFrame=-1,appliedReflex;
    bool earlyReflex,reflexPacing;
    int displayW,displayH,optimalW,optimalH,postFrame=-1,postCamera,lastFlags;
    Quality sizedMode;
    bool sizeReady;
    RTHandle postOutput;
    RenderTexture postColor;
    ConfigEntry<int> multiplier;
    ConfigEntry<bool> fpsOverlay;
    double rateStart;
    int rateFrame,rateMode;
    Quality rateQuality;
    ulong ratePresents;
    string fpsText="";
    Harmony patches;
    IntPtr callback;
    int first,state,max,dyn,presented,evals,baseFrame=-1,resolved=-1,previousFrame=-2,previousId,previousMode=-1;
    bool loggedPrimary;
    int lastSubmitFrame=-1;
    bool show,oldCursorVisible,gameForeground;
    CursorLockMode oldCursorLock;
    string status="Initializing";
    readonly StringBuilder statusText=new StringBuilder(512);
    Rect menu=new Rect(32,48,345,0);
    Camera baseCamera;
    Matrix4x4 projection,viewRotation,previousProjection,previousViewRotation;
    Vector2 previousJitter,frameJitter;
    int sampleFrame=-1,sampleCamera;
    int cockpitFrame=-1,cockpitStart=-1;
    float cockpitNear,cockpitFar,previousFov;
    int nativeFrame=-1;
    RenderTexture nativeDraw,nativeCockpit,detailed,coverage,worldInput,worldMotion;
    Vector3 prevPos,prevForward;
    RenderTexture color,depth,motion,output,urpOutput,hudless,final;
    static readonly Vector2 FlipY=new Vector2(1,-1), TopLeft=new Vector2(0,1);
    RTHandle outputHandle;
    RenderTexture alternateHud; IntPtr alternateHudPtr; int hudCaptureFrame=-1;
    RenderTexture prevColor,beforeCockpit,afterCockpit,cockpitDepth,cockpitMotion,mergedDepth,backwardMotion,ui;
    Frame frame,geometry;
    IntPtr colorPtr,depthPtr,motionPtr,outputPtr,hudPtr,finalPtr;
    [ThreadStatic] static bool sizing;
    readonly Inputs pass=new Inputs();
    readonly CockpitCapture beforePass=new CockpitCapture(true),afterPass=new CockpitCapture(false);
    readonly RequireFinalBlit finalPass=new RequireFinalBlit();
    readonly NativePanel panelPass=new NativePanel();
    static readonly FieldInfo bufferField=AccessTools.Field(typeof(RenderingData),"commandBuffer");
    static readonly FieldInfo depthField=AccessTools.Field(typeof(UniversalRenderer),"m_DepthTexture");
    static readonly FieldInfo motionField=AccessTools.Field(typeof(UniversalRenderer),"m_MotionVectorColor");
    static bool Active {get{return self!=null && self.state==1 && (self.ActiveQuality==Quality.Off ? self.Count!=0 : self.sizeReady && self.ActiveQuality==self.sizedMode);}}
    int Count {get{return fg.Value && max>0 && !(nativeMenu && nativeMenu.isActiveAndEnabled) && Time.timeScale>0 && gameForeground && Application.isFocused ? (mfg.Value && max>1 ? (dynamic.Value && dyn!=0 ? -1 : Mathf.Clamp(multiplier.Value-1,1,max)):1):0;}}
    static bool Scene(Camera c) {return c && c.cameraType==CameraType.Game && !c.targetTexture && !c.orthographic && !c.stereoEnabled && c.rect==new Rect(0,0,1,1);}
    void Awake()
    {
        self=this;
        quality=Config.Bind("Graphics","SuperResolution",Quality.DLAA,"DLSS Super Resolution quality");
        preferFsrScaling=Config.Bind("Graphics","PreferFSRUpscaler",false,"Use AMD FSR 3.1 when both upscalers are available");
        fsrQuality=Config.Bind("Graphics","FSRSuperResolution",FsrPreset.Off,"FSR 3.1 temporal upscaling quality");
        fpsOverlay=Config.Bind("Display","ShowFPS",false,"Show Unity rendered FPS and the active provider's presentation rate during gameplay");
        forceSecondGen=Config.Bind("Graphics","ForceSecondGenerationModel",false,"Request model M for DLAA through Performance and model L for Ultra Performance");
        nativePanel=Config.Bind("Graphics","NativeInstruments",true,"Draw cockpit geometry and instrument markings at display resolution");
        fg=Config.Bind("Graphics","FrameGeneration",false,"Present interpolated frames in the game client area");
        provider=Config.Bind("Graphics",FrameGenerationProvider.Key,"","DLSS or FSR. An empty preference uses the active GPU's default. Restart after changing providers.");
        string normalized=FrameGenerationProvider.Normalize(provider.Value);
        if(normalized!=provider.Value) provider.Value=normalized;
        startupPreference=provider.Value;
        reflexBoost=Config.Bind("Graphics","ReflexBoost",false,"Use NVIDIA Reflex On + Boost during frame generation");
        capToRefresh=Config.Bind("Graphics","LimitFrameGenerationToMonitorRefresh",false,"Limit displayed FPS during frame generation to the active refresh rate of the monitor containing the game window");
        mfg=Config.Bind("Graphics","MultiFrameGeneration",false,"Use more than one generated frame when supported");
        dynamic=Config.Bind("Graphics","DynamicMultiplier",false,"Let NVIDIA choose the multiplier");
        multiplier=Config.Bind("Graphics","Multiplier",3,new ConfigDescription("Total displayed frames per rendered frame",new AcceptableValueRange<int>(3,6)));
        InstallGraphicsMenu();
        vulkan=SystemInfo.graphicsDeviceType==GraphicsDeviceType.Vulkan;
        if(!vulkan && SystemInfo.graphicsDeviceType!=GraphicsDeviceType.Direct3D12) {
            status=SystemInfo.graphicsDeviceType+" active. Select Vulkan or DirectX 12 in Steam.";Logger.LogInfo(status);return;
        }
        try {
            if(vulkan){if(!StartVulkan())return;}
            else{
                string dir=Path.Combine(Path.GetDirectoryName(Info.Location),"Runtime");
                callback=Setup(dir,Paths.BepInExRootPath,out first);
                if(callback==IntPtr.Zero)throw new InvalidOperationException("Unity D3D12 v7 plug-in interface unavailable");
            }
            patches=new Harmony("local.nuclearoption.dlss");
            Hook(typeof(UniversalRenderPipeline),"InitializeCameraData","SizeBegin","SizeDescriptor","SizeEnd");
            Hook(typeof(UniversalRenderPipelineAsset),"get_renderScale",null,"Scale");
            Hook(typeof(UniversalRenderPipelineAsset),"get_upscalingFilter",null,"Filter");
            Hook(AccessTools.TypeByName("UnityEngine.Rendering.Universal.TemporalAA"),"CalculateJitterMatrix",null,"Jitter");
            Hook(typeof(UniversalRenderer),"Setup","Enqueue");
            var shaderVariables=AccessTools.Method(typeof(ScriptableRenderer),"SetPerCameraShaderVariables",new[]{typeof(CommandBuffer),typeof(CameraData).MakeByRefType(),typeof(bool)});
            patches.Patch(shaderVariables,postfix:HM("WorldMipBias"));
            Hook(AccessTools.TypeByName("UnityEngine.Rendering.Universal.PostProcessPass"),"Render","BeforePost",null,"RestorePost");
            Hook(AccessTools.TypeByName("UnityEngine.Rendering.Universal.Internal.FinalBlitPass"),"ExecutePass","Resolve");
            if(!vulkan){
                RenderPipelineManager.beginFrameRendering+=RenderStart;
                earlyReflex=SetReflexLoop(true);
                Issue(0,IntPtr.Zero);
            }
            StartCoroutine(EndFrames());
        } catch(Exception e) {status=e.GetBaseException().Message; Logger.LogError(e); callback=IntPtr.Zero; if(patches!=null) patches.UnpatchSelf();}
    }
    void Hook(Type type,string method,string pre=null,string post=null,string cleanup=null) {
        patches.Patch(AccessTools.Method(type,method),HM(pre),HM(post),null,HM(cleanup),null);
    }
    static HarmonyMethod HM(string n) {return n==null?null:new HarmonyMethod(typeof(NuclearDLSS),n);}
    static void SizeBegin(Camera camera,out bool __state) {__state=sizing; sizing=Active && Scene(camera) && self.ActiveQuality!=Quality.Off;}
    static void SizeEnd(bool __state) {sizing=__state;}
    static void SizeDescriptor(ref CameraData cameraData) {
        if(!sizing) return;
        cameraData.cameraTargetDescriptor.width=self.optimalW;
        cameraData.cameraTargetDescriptor.height=self.optimalH;
    }
    static void WorldMipBias(CommandBuffer cmd,ref CameraData cameraData) {
        if(!Active || self.ActiveQuality==Quality.Off || cameraData.renderType!=CameraRenderType.Base || !Scene(cameraData.camera)) return;
        float ratio=cameraData.cameraTargetDescriptor.width/(float)Mathf.Max(1,cameraData.camera.pixelWidth);
        float bias=Mathf.Log(Mathf.Max(0.25f,ratio),2)-0.9f;
        cmd.SetGlobalVector("_GlobalMipBias",new Vector4(bias,Mathf.Pow(2,bias),0,0));
    }
    static void Scale(ref float __result) {
        if(!sizing) return;
        __result=self.optimalW/(float)self.displayW;
    }
    static void Filter(ref UpscalingFilterSelection __result) {if(sizing) __result=UpscalingFilterSelection.Linear;}
    static float Halton(int n,int b) {float x=0,s=1; while(n>0){s/=b;x+=s*(n%b);n/=b;}return x;}
    Vector2 WorldSample(Camera camera) {
        int frame=Time.frameCount;
        if(sampleFrame==frame && sampleCamera==camera.GetInstanceID()) return frameJitter;
        sampleCamera=camera.GetInstanceID();sampleFrame=frame;
        float scale=displayW/(float)Mathf.Max(1,optimalW);
        int phases=Mathf.Max(8,Mathf.CeilToInt(8*scale*scale));
        int i=(frame%phases)+1;
        frameJitter=new Vector2(Halton(i,2)-0.5f,Halton(i,3)-0.5f);
        return frameJitter;
    }
    static void Jitter(ref CameraData cameraData,ref Matrix4x4 __result) {
        if(!Active || self.ActiveQuality==Quality.Off || !Scene(cameraData.camera)) return;
        cameraData.antialiasing=AntialiasingMode.None;
        if(cameraData.renderType!=CameraRenderType.Base) {__result=Matrix4x4.identity;return;}
        var d=cameraData.cameraTargetDescriptor; var j=self.WorldSample(cameraData.camera);
        __result=Matrix4x4.Translate(new Vector3(2*j.x/Mathf.Max(1,d.width),2*j.y/Mathf.Max(1,d.height),0));
    }
    static void Enqueue(UniversalRenderer __instance,ref RenderingData renderingData) {
        if(!Active || renderingData.cameraData.camera.targetTexture || renderingData.cameraData.camera.cameraType!=CameraType.Game) return;
        if(renderingData.cameraData.renderType==CameraRenderType.Base && Scene(renderingData.cameraData.camera))
            __instance.EnqueuePass(self.pass);
        else if(!self.vulkan && renderingData.cameraData.renderType==CameraRenderType.Overlay && renderingData.cameraData.camera.name=="cockpitRenderer" && self.baseFrame==Time.frameCount) {
            __instance.EnqueuePass(self.beforePass); __instance.EnqueuePass(self.afterPass);
            if(self.NativePanelEnabled && self.ActiveQuality!=Quality.Off) __instance.EnqueuePass(self.panelPass);
        }
        if(renderingData.cameraData.renderType==CameraRenderType.Overlay && renderingData.cameraData.resolveFinalTarget && self.baseFrame==Time.frameCount)
            __instance.EnqueuePass(self.finalPass);
        if(renderingData.cameraData.resolveFinalTarget && self.ActiveQuality!=Quality.Off)
            renderingData.cameraData.antialiasing=AntialiasingMode.None;
    }
    sealed class RequireFinalBlit : ScriptableRenderPass {
        // URP folds the last overlay's postprocessing into the display when no
        // pass is registered at this event, bypassing FinalBlitPass entirely.
        public RequireFinalBlit(){renderPassEvent=RenderPassEvent.AfterRenderingPostProcessing;}
        public override void Execute(ScriptableRenderContext context,ref RenderingData data){}
    }
    sealed class CockpitCapture : ScriptableRenderPass {
        readonly bool before;
        public CockpitCapture(bool captureBefore) {
            before=captureBefore;
            renderPassEvent=before?(RenderPassEvent)((int)RenderPassEvent.BeforeRenderingOpaques-1):RenderPassEvent.AfterRenderingTransparents;
            if(!before) ConfigureInput(ScriptableRenderPassInput.Depth|ScriptableRenderPassInput.Motion);
        }
        public override void Execute(ScriptableRenderContext context,ref RenderingData data) {
            var p=self; var renderer=data.cameraData.renderer;
            var source=renderer.cameraColorTargetHandle;
            if(p.baseFrame!=Time.frameCount || source==null || !source.rt) return;
            var cmd=CommandBufferPool.Get("DLSS cockpit coverage");
            if(before) {cmd.Blit(source.nameID,p.beforeCockpit,FlipY,TopLeft); p.cockpitStart=Time.frameCount;}
            else if(p.cockpitStart==Time.frameCount) {
                var z=(RTHandle)depthField.GetValue(renderer); var mv=(RTHandle)motionField.GetValue(renderer);
                if(z!=null && z.rt && mv!=null && mv.rt) {
                    cmd.Blit(source.nameID,p.afterCockpit,FlipY,TopLeft);
                    cmd.Blit(z.rt,p.cockpitDepth,FlipY,TopLeft);
                    cmd.Blit(mv.rt,p.cockpitMotion,FlipY,TopLeft);
                    p.cockpitNear=data.cameraData.camera.nearClipPlane;p.cockpitFar=data.cameraData.camera.farClipPlane;
                    p.cockpitFrame=Time.frameCount;
                }
            }
            context.ExecuteCommandBuffer(cmd); CommandBufferPool.Release(cmd);
        }
    }
    sealed class NativePanel : ScriptableRenderPass
    {
        readonly List<ShaderTagId> tags=new List<ShaderTagId>{new ShaderTagId("UniversalForward"),
            new ShaderTagId("UniversalForwardOnly"),new ShaderTagId("SRPDefaultUnlit")};
        static readonly int scaled=Shader.PropertyToID("_ScaledScreenParams");
        public NativePanel(){renderPassEvent=(RenderPassEvent)((int)RenderPassEvent.AfterRenderingTransparents+1);}
        static Vector4 ScreenParams(int w,int h){return new Vector4(w,h,1f+1f/w,1f+1f/h);}
        public override void Execute(ScriptableRenderContext context,ref RenderingData data) {
            var p=self;
            if(p.cockpitFrame!=Time.frameCount) return;
            var renderer=data.cameraData.renderer;
            var cmd=CommandBufferPool.Get("Native-resolution cockpit");
            cmd.SetRenderTarget(p.nativeDraw);
            cmd.SetViewport(new Rect(0,0,p.output.width,p.output.height));
            cmd.SetGlobalVector(scaled,ScreenParams(p.output.width,p.output.height));
            cmd.ClearRenderTarget(true,true,Color.clear);
            context.ExecuteCommandBuffer(cmd);cmd.Clear();
            var opaque=CreateDrawingSettings(tags,ref data,SortingCriteria.CommonOpaque);
            var transparent=CreateDrawingSettings(tags,ref data,SortingCriteria.CommonTransparent);
            var opaqueFilter=new FilteringSettings(RenderQueueRange.opaque,data.cameraData.camera.cullingMask);
            var transparentFilter=new FilteringSettings(RenderQueueRange.transparent,data.cameraData.camera.cullingMask);
            context.DrawRenderers(data.cullResults,ref opaque,ref opaqueFilter);
            // Capture solid coverage before the canopy glass adds alpha across the entire windscreen.
            cmd.Blit(p.nativeDraw,p.coverage,FlipY,TopLeft);
            cmd.SetRenderTarget(p.nativeDraw);
            cmd.SetViewport(new Rect(0,0,p.output.width,p.output.height));
            context.ExecuteCommandBuffer(cmd);cmd.Clear();
            context.DrawRenderers(data.cullResults,ref transparent,ref transparentFilter);
            cmd.Blit(p.nativeDraw,p.nativeCockpit,FlipY,TopLeft);
            cmd.SetRenderTarget(renderer.cameraColorTargetHandle.nameID,renderer.cameraDepthTargetHandle.nameID);
            cmd.SetViewport(new Rect(0,0,p.color.width,p.color.height));
            cmd.SetGlobalVector(scaled,ScreenParams(p.color.width,p.color.height));
            context.ExecuteCommandBuffer(cmd);CommandBufferPool.Release(cmd);
            p.nativeFrame=Time.frameCount;
        }
    }
    sealed class Inputs : ScriptableRenderPass
    {
        public Inputs() {renderPassEvent=(RenderPassEvent)((int)RenderPassEvent.BeforeRenderingPostProcessing-1); ConfigureInput(ScriptableRenderPassInput.Depth|ScriptableRenderPassInput.Motion|ScriptableRenderPassInput.Color);}
        public override void Execute(ScriptableRenderContext context,ref RenderingData data) {
            var p=self; var d=data.cameraData.cameraTargetDescriptor;
            if(d.width<16 || d.height<16) return;
            var sceneDepth=(RTHandle)depthField.GetValue(data.cameraData.renderer);
            var velocity=(RTHandle)motionField.GetValue(data.cameraData.renderer);
            if(sceneDepth==null || !sceneDepth.rt || velocity==null || !velocity.rt) return;
            p.baseFrame=Time.frameCount; p.baseCamera=data.cameraData.camera;
            p.projection=data.cameraData.GetGPUProjectionMatrixNoJitter();
            if(data.cameraData.IsCameraProjectionMatrixFlipped()) p.projection=Matrix4x4.Scale(new Vector3(1,-1,1))*p.projection;
            p.viewRotation=data.cameraData.GetViewMatrix();p.viewRotation.SetColumn(3,new Vector4(0,0,0,1));
            p.Targets(d.width,d.height,p.baseCamera.pixelWidth,p.baseCamera.pixelHeight);
            var cmd=CommandBufferPool.Get("DLSS camera inputs");
            cmd.Blit(sceneDepth.rt,p.depth,FlipY,TopLeft);
            cmd.Blit(velocity.rt,p.motion,FlipY,TopLeft);
            context.ExecuteCommandBuffer(cmd); CommandBufferPool.Release(cmd);
        }
    }
    static RenderTexture Tex(int w,int h,GraphicsFormat format,string name,bool uav=false) {
        var d=new RenderTextureDescriptor(w,h,format,0){msaaSamples=1,enableRandomWrite=uav};
        var t=new RenderTexture(d){name="Nuclear DLSS "+name,filterMode=FilterMode.Bilinear};
        if(!t.Create()) throw new InvalidOperationException("Allocating "+name); return t;
    }
    void Targets(int w,int h,int ow,int oh) {
        if(color && color.width==w && color.height==h && output.width==ow && output.height==oh) {
            if(!nativeDraw && ActiveQuality!=Quality.Off && NativePanelEnabled) CreatePanelTargets(w,h,ow,oh);
            return;
        }
        Release();
        color=Tex(w,h,GraphicsFormat.R16G16B16A16_SFloat,"scene");
        depth=Tex(w,h,GraphicsFormat.R32_SFloat,"depth");
        motion=Tex(w,h,GraphicsFormat.R16G16_SFloat,"velocity");
        output=Tex(ow,oh,GraphicsFormat.R16G16B16A16_SFloat,"resolve",true);
        urpOutput=Tex(ow,oh,GraphicsFormat.R16G16B16A16_SFloat,"URP display resolve");
        outputHandle=RTHandles.Alloc(urpOutput);
        postColor=Tex(ow,oh,GraphicsFormat.R16G16B16A16_SFloat,"display post-process");
        postOutput=RTHandles.Alloc(postColor);
        hudless=Tex(ow,oh,GraphicsFormat.R8G8B8A8_SRGB,"HUD-less SDR");
        if(vulkan)alternateHud=Tex(ow,oh,GraphicsFormat.R8G8B8A8_SRGB,"second HUD-less frame");
        else final=Tex(ow,oh,GraphicsFormat.R8G8B8A8_SRGB,"display SDR");
        colorPtr=color.GetNativeTexturePtr(); depthPtr=depth.GetNativeTexturePtr(); motionPtr=motion.GetNativeTexturePtr();
        outputPtr=output.GetNativeTexturePtr(); hudPtr=hudless.GetNativeTexturePtr(); finalPtr=final?final.GetNativeTexturePtr():IntPtr.Zero;
        if(vulkan){alternateHudPtr=alternateHud.GetNativeTexturePtr();previousFrame=-2;return;}
        var rgba=GraphicsFormat.R16G16B16A16_SFloat; var rg=GraphicsFormat.R16G16_SFloat; var r=GraphicsFormat.R32_SFloat;
        prevColor=Tex(w,h,rgba,"previous color");
        beforeCockpit=Tex(w,h,rgba,"world before cockpit"); afterCockpit=Tex(w,h,rgba,"world with cockpit");
        cockpitDepth=Tex(w,h,r,"cockpit depth"); cockpitMotion=Tex(w,h,rg,"cockpit velocity");
        mergedDepth=Tex(w,h,r,"composed depth",true); backwardMotion=Tex(w,h,rg,"composed velocity",true);
        ui=Tex(ow,oh,GraphicsFormat.R8G8B8A8_UNorm,"premultiplied HUD",true);
        geometry=new Frame{prevColor=prevColor.GetNativeTexturePtr(),
            beforeCockpit=beforeCockpit.GetNativeTexturePtr(),afterCockpit=afterCockpit.GetNativeTexturePtr(),
            cockpitDepth=cockpitDepth.GetNativeTexturePtr(),cockpitMotion=cockpitMotion.GetNativeTexturePtr(),
            mergedDepth=mergedDepth.GetNativeTexturePtr(),backwardMotion=backwardMotion.GetNativeTexturePtr(),
            ui=ui.GetNativeTexturePtr()};
        if(ActiveQuality!=Quality.Off && NativePanelEnabled) CreatePanelTargets(w,h,ow,oh);
        previousFrame=-2;
    }
    void CreatePanelTargets(int w,int h,int ow,int oh) {
        var rgba=GraphicsFormat.R16G16B16A16_SFloat;
        var cockpitDesc=new RenderTextureDescriptor(ow,oh,rgba,24){msaaSamples=2};
        cockpitDesc.msaaSamples=SystemInfo.GetRenderTextureSupportedMSAASampleCount(cockpitDesc);
        nativeDraw=new RenderTexture(cockpitDesc){name="Nuclear DLSS native instrument geometry"};
        if(!nativeDraw.Create()) throw new InvalidOperationException("Allocating native cockpit color and depth");
        nativeCockpit=Tex(ow,oh,rgba,"native cockpit foreground");
        detailed=Tex(ow,oh,rgba,"native cockpit composite",true);
        coverage=Tex(ow,oh,GraphicsFormat.R8G8B8A8_UNorm,"solid cockpit coverage");
        worldInput=Tex(w,h,rgba,"world color without cockpit",true);
        worldMotion=Tex(w,h,GraphicsFormat.R16G16_SFloat,"world backward motion",true);
        geometry.nativeCockpit=nativeCockpit.GetNativeTexturePtr();geometry.detailed=detailed.GetNativeTexturePtr();
        geometry.coverage=coverage.GetNativeTexturePtr();geometry.worldInput=worldInput.GetNativeTexturePtr();
        geometry.worldMotion=worldMotion.GetNativeTexturePtr();
    }
    void Release() {
        if(postOutput!=null){postOutput.Release();postOutput=null;}
        if(postColor) Destroy(postColor);
        if(outputHandle!=null){outputHandle.Release();outputHandle=null;}
        foreach(var t in new[]{alternateHud,color,depth,motion,output,urpOutput,hudless,final,prevColor,beforeCockpit,afterCockpit,cockpitDepth,cockpitMotion,mergedDepth,backwardMotion,ui,nativeDraw,nativeCockpit,detailed,coverage,worldInput,worldMotion}) if(t) Destroy(t);
        alternateHud=color=depth=motion=output=urpOutput=hudless=final=null;
        nativeDraw=nativeCockpit=detailed=coverage=worldInput=worldMotion=null;
    }
    // Run the game's own post-process using its explicit source/destination path.
    // Restore pass-owned fields because URP reuses the same pass for other cameras.
    struct PostScope {
        public bool active,swap,hasFinal,screen;
        public CameraData camera;
        public RenderTextureDescriptor descriptor;
        public RTHandle src,dst;
    }
    static void BeforePost(ref RenderingData renderingData,ref RTHandle ___m_Source,ref RTHandle ___m_Destination,
        ref bool ___m_UseSwapBuffer,ref bool ___m_HasFinalPass,ref bool ___m_ResolveToScreen,
        ref RenderTextureDescriptor ___m_Descriptor,out PostScope __state) {
        __state=default(PostScope);
        var p=self;
        if(!Active || p.ActiveQuality==Quality.Off || !renderingData.cameraData.resolveFinalTarget ||
           p.baseFrame!=Time.frameCount || p.resolved==Time.frameCount) return;
        var input=___m_UseSwapBuffer?renderingData.cameraData.renderer.cameraColorTargetHandle:___m_Source;
        if(input==null || !input.rt) return;
        BuildScene(ref renderingData,ref input,true);
        if(p.resolved!=Time.frameCount) return;
        __state=new PostScope{active=true,swap=___m_UseSwapBuffer,hasFinal=___m_HasFinalPass,screen=___m_ResolveToScreen,
            camera=renderingData.cameraData,descriptor=___m_Descriptor,src=___m_Source,dst=___m_Destination};
        ___m_UseSwapBuffer=false;___m_HasFinalPass=false;___m_Source=input;___m_Destination=p.postOutput;
        ___m_Descriptor=p.urpOutput.descriptor;
        renderingData.cameraData.cameraTargetDescriptor=___m_Descriptor;
        renderingData.cameraData.renderScale=1;
        renderingData.cameraData.resolveFinalTarget=false;
        var cmd=(CommandBuffer)bufferField.GetValue(renderingData);
        cmd.SetGlobalVector("_ScaledScreenParams",new Vector4(p.output.width,p.output.height,1f+1f/p.output.width,1f+1f/p.output.height));
        p.postFrame=Time.frameCount;p.postCamera=renderingData.cameraData.camera.GetInstanceID();
    }
    static void RestorePost(ref RenderingData renderingData,ref RTHandle ___m_Source,ref RTHandle ___m_Destination,
        ref bool ___m_UseSwapBuffer,ref bool ___m_HasFinalPass,ref bool ___m_ResolveToScreen,
        ref RenderTextureDescriptor ___m_Descriptor,PostScope __state) {
        if(!__state.active) return;
        renderingData.cameraData=__state.camera;___m_Source=__state.src;___m_Destination=__state.dst;
        ___m_UseSwapBuffer=__state.swap;___m_HasFinalPass=__state.hasFinal;___m_ResolveToScreen=__state.screen;___m_Descriptor=__state.descriptor;
    }
    static void Resolve(ref RenderingData renderingData,ref RTHandle source) {
        var p=self;
        if(!Active || !renderingData.cameraData.resolveFinalTarget || renderingData.cameraData.camera.targetTexture) return;
        if(p.postFrame==Time.frameCount && p.postCamera==renderingData.cameraData.camera.GetInstanceID()) {
            source=p.postOutput;
            if(p.frame.generated!=0) {
                var cmd=(CommandBuffer)bufferField.GetValue(renderingData);
                cmd.Blit(source.nameID,p.hudless,FlipY,TopLeft);p.hudCaptureFrame=Time.frameCount;
            }
        } else BuildScene(ref renderingData,ref source,false);
    }
    static void BuildScene(ref RenderingData renderingData,ref RTHandle source,bool early)
    {
        var p=self; int index=Time.frameCount;
        if(!Active || p.baseFrame!=index || p.resolved==index || source==null || !source.rt ||
           renderingData.cameraData.camera.targetTexture || !renderingData.cameraData.resolveFinalTarget) return;
        var cmd=(CommandBuffer)bufferField.GetValue(renderingData);
        var cam=p.baseCamera; var tr=cam.transform;
        var q=p.ActiveQuality;
        bool reset=p.previousFrame!=index-1 || cam.GetInstanceID()!=p.previousId || (int)q!=p.previousMode ||
            Vector3.Distance(tr.position,p.prevPos)>100 || Vector3.Dot(tr.forward,p.prevForward)<0.7f || Mathf.Abs(cam.fieldOfView-p.previousFov)>3;
        int flags=(p.forceSecondGen.Value?1:0)|(early?2:0)|(QualitySettings.vSyncCount>0?4:0);
        if(p.nativeFrame==index) flags|=8;
        if(p.FsrScaling)flags|=16;
        reset|=(flags&27)!=(p.lastFlags&27);
        // Relative translation avoids subtracting two large world-space projection matrices near a floating-origin shift.
        var delta=reset?Matrix4x4.identity:p.previousProjection*p.previousViewRotation*Matrix4x4.Translate(tr.position-p.prevPos)*p.viewRotation.inverse*p.projection.inverse;
        var j=q!=Quality.Off && p.sampleFrame==index && p.sampleCamera==cam.GetInstanceID()?p.frameJitter:Vector2.zero;
        p.lastFlags=flags;
        p.frame=new Frame{index=index,mode=(int)q,generated=cam.name=="Menu Camera"?0:p.Count,reset=reset?1:0,flags=flags,
            w=p.color.width,h=p.color.height,ow=p.output.width,oh=p.output.height,
            // The projected image moves right/down by (+x,-y) in the top-left DXGI texture.
            // History UV lookup below maps in the opposite direction.
            jx=j.x,jy=-j.y,near=cam.nearClipPlane,far=cam.farClipPlane,fov=cam.fieldOfView*Mathf.Deg2Rad,aspect=cam.aspect,
            proj=p.projection,inverse=p.projection.inverse,clipToPrev=delta,prevToClip=delta.inverse,
            pos=tr.position,up=tr.up,right=tr.right,forward=tr.forward,
            color=p.colorPtr,depth=p.depthPtr,motion=p.motionPtr,output=p.outputPtr,
            prevColor=p.geometry.prevColor,beforeCockpit=p.geometry.beforeCockpit,afterCockpit=p.geometry.afterCockpit,
            cockpitDepth=p.geometry.cockpitDepth,cockpitMotion=p.geometry.cockpitMotion,mergedDepth=p.geometry.mergedDepth,
            backwardMotion=p.geometry.backwardMotion,ui=p.geometry.ui,
            cockpitNear=p.cockpitNear,cockpitFar=p.cockpitFar,hasCockpit=p.cockpitFrame==index?1:0,
            nativeCockpit=p.nativeFrame==index?p.geometry.nativeCockpit:IntPtr.Zero,detailed=p.geometry.detailed,
            coverage=p.geometry.coverage,worldInput=p.geometry.worldInput,worldMotion=p.geometry.worldMotion,
            deltaJitterX=-j.x-p.previousJitter.x,deltaJitterY=j.y-p.previousJitter.y,frameTimeMs=Time.unscaledDeltaTime*1000f};
        // Normalize the URP intermediates to the DXGI scanout orientation before NGX sees them.
        cmd.Blit(source.nameID,p.color,FlipY,TopLeft);
        if(q!=Quality.Off) cmd.Blit(source.nameID,p.output,FlipY,TopLeft); // Fallback on an NGX error.
        if(p.vulkan){
            if(q!=Quality.Off)cmd.IssuePluginEventAndData(p.callback,p.first+2,p.PackVulkan(ref p.frame));
            var old=p.hudless;p.hudless=p.alternateHud;p.alternateHud=old;
            var ptr=p.hudPtr;p.hudPtr=p.alternateHudPtr;p.alternateHudPtr=ptr;
        }else cmd.IssuePluginEventAndData(p.callback,p.first+1,Pack(ref p.frame));
        if(!p.vulkan)cmd.CopyTexture(p.color,p.prevColor);
        var scene=q==Quality.Off?p.color:(p.nativeFrame==index?p.detailed:p.output);
        if(!early && p.frame.generated!=0){cmd.Blit(scene,p.hudless);p.hudCaptureFrame=index;}
        if(q!=Quality.Off) {cmd.Blit(scene,p.urpOutput,FlipY,TopLeft); source=p.outputHandle;}
        p.prevPos=tr.position;p.prevForward=tr.forward;p.previousProjection=p.projection;p.previousViewRotation=p.viewRotation;
        p.previousFov=cam.fieldOfView;p.previousJitter=new Vector2(-j.x,j.y);p.previousFrame=index;
        p.previousId=cam.GetInstanceID();p.previousMode=(int)q;p.resolved=index;
    }
    void Issue(int op,IntPtr data) {
        var cmd=CommandBufferPool.Get("Nuclear Option DLSS"); cmd.IssuePluginEventAndData(callback,first+op,data);
        Graphics.ExecuteCommandBuffer(cmd); CommandBufferPool.Release(cmd);
    }
    IEnumerator EndFrames() {
        var end=new WaitForEndOfFrame();
        while(true) {
            yield return end;
            if(state!=1) continue;
            if(Count!=0 && resolved==Time.frameCount && frame.generated!=0 && (vulkan || final)) {
                if(!usingFsr) ScreenCapture.CaptureScreenshotIntoRenderTexture(final);
                frame.hudless=(!vulkan || hudCaptureFrame==Time.frameCount)?hudPtr:IntPtr.Zero; frame.final=finalPtr;
                if(vulkan)Issue(0,PackVulkan(ref frame));else Issue(2,Pack(ref frame));
            } else if(!vulkan) Issue(4,(IntPtr)Time.frameCount);
        }
    }
    void Update() {
        if(Input.GetKeyDown(KeyCode.F8)) SetMenu(!show);
        if(vulkan){VulkanUpdate();return;}
        if(callback==IntPtr.Zero) return;
        int before=state; Status(out state,out max,out dyn,out presented,out evals,statusText,statusText.Capacity);status=statusText.ToString();
        features=(AdapterFeatures)GetAdapterFeatures();
        usingFsr=UsingFsr()!=0;hasReflex=HasReflex()!=0;
        gameForeground=state==1 && max>0 && GameIsForeground()!=0;
        if(state==1 && (capToRefresh.Value || show || (nativeMenu && nativeMenu.isActiveAndEnabled))) UpdateMonitorRefresh();
        if(before!=state || (max>0 && !loggedPrimary)) {Logger.LogInfo(status+" | generated frames supported: "+max);loggedPrimary=max>0;}
        if(state==1) {
            int w=Screen.width,h=Screen.height;
            bool warmFsr=usingFsr && max>0 && fg.Value && w>0 && h>0;
            if(warmFsr && (!fsrWarmRequested || displayW!=w || displayH!=h)) Issue(6,IntPtr.Zero);
            fsrWarmRequested=warmFsr;
            if(w>0 && h>0 && (displayW!=w || displayH!=h)) {
                displayW=w;displayH=h;
                if(HasDlssScaling) Issue(5,(IntPtr)(((long)w<<32)|(uint)h));
            }
            sizedMode=ActiveQuality;
            sizeReady=FsrScaling?FsrSize(sizedMode,w,h,out optimalW,out optimalH):HasDlssScaling && OptimalSize((int)sizedMode,w,h,out optimalW,out optimalH)!=0;
        }
        if(fpsOverlay.Value && state==1) UpdateRates();
        else rateStart=0;
        // Some Unity systems replace the player loop after scene load.
        if(reflexFrame!=Time.frameCount) StartReflexFrame();
    }
    struct ReflexInputStage {}
    void UpdateMonitorRefresh() {
        double now=Time.realtimeSinceStartupAsDouble;
        if(now<nextRefreshCheck) return;
        nextRefreshCheck=now+1;
        var rate=Screen.mainWindowDisplayInfo.refreshRate;
        double hz=rate.denominator>0 ? (double)rate.numerator/rate.denominator : 0;
        if(hz<1) hz=0;
        if(hz==monitorHz && refreshCapLabel!=RefreshCapTitle) return;
        monitorHz=hz;
        displayPeriodUs=hz>0 ? (uint)Math.Ceiling(1000000.0/hz) : 0;
        refreshCapLabel=hz>0 ? RefreshCapTitle+" ("+hz.ToString("0.##")+" Hz)" : RefreshCapTitle+" (unavailable)";
    }
    static bool SetReflexLoop(bool install) {
        var loop=PlayerLoop.GetCurrentPlayerLoop();
        for(int i=0;i<loop.subSystemList.Length;i++) {
            var phase=loop.subSystemList[i];
            if(phase.type!=typeof(UnityEngine.PlayerLoop.EarlyUpdate)) continue;
            var list=new List<PlayerLoopSystem>(phase.subSystemList);
            int removed=list.RemoveAll(x=>x.type==typeof(ReflexInputStage));
            if(!install && removed==0) return false;
            if(install) list.Insert(0,new PlayerLoopSystem{type=typeof(ReflexInputStage),updateDelegate=StartReflexFrame});
            phase.subSystemList=list.ToArray();
            loop.subSystemList[i]=phase;
            PlayerLoop.SetPlayerLoop(loop);
            return true;
        }
        return false;
    }
    static void StartReflexFrame() {
        var p=self;
        if(!p) return;
        if(p.state!=1 || !p.hasReflex || p.Count==0) {
            if(p.reflexPacing) {PauseReflexInput();p.reflexPacing=false;p.appliedReflex=0;}
            return;
        }
        int frame=Time.frameCount;
        if(p.reflexFrame==frame) return;
        p.reflexFrame=frame;
        p.reflexPacing=true;
        uint limit=p.capToRefresh.Value && (p.usingFsr || p.presented>0) ? p.displayPeriodUs*(p.usingFsr?2u:1u) : 0;
        p.appliedReflex=BeginReflexFrame(frame,p.reflexBoost.Value?1:0,limit);
    }
    void RenderStart(ScriptableRenderContext ctx,Camera[] cams) {
        int frame=Time.frameCount;
        if(state!=1 || lastSubmitFrame==frame || Count==0) return;
        lastSubmitFrame=frame;
        Marker(frame,1); Marker(frame,2);
    }
    void SetMenu(bool open) {
        if(show==open) return;
        if(open) {oldCursorLock=Cursor.lockState; oldCursorVisible=Cursor.visible; Cursor.lockState=CursorLockMode.None; Cursor.visible=true;}
        else {Cursor.lockState=oldCursorLock; Cursor.visible=oldCursorVisible;}
        show=open;
    }
    void UpdateRates() {
        double now=Time.realtimeSinceStartupAsDouble;
        int mode=Count;
        bool reset=rateStart==0 || rateMode!=mode || rateQuality!=ActiveQuality;
        double elapsed=now-rateStart;
        if(!reset && elapsed<1) return;
        ulong total=vulkan?vkTotal:TotalPresents();
        if(reset || elapsed>3) fpsText="";
        else {
            double rendered=(Time.frameCount-rateFrame)/elapsed;
            fpsText="Game  "+rendered.ToString("F0")+" FPS";
            if(mode!=0) fpsText+=(vulkan?"    Vulkan  ":usingFsr?"    DXGI  ":"    Streamline  ")+((total-ratePresents)/elapsed).ToString("F0")+" presents/s";
        }
        rateStart=now;rateFrame=Time.frameCount;ratePresents=total;rateMode=mode;rateQuality=ActiveQuality;
    }
    void OnGUI() {
        if(show) menu=GUILayout.Window(0x44534e,menu,Draw,"Nuclear Option Graphics",GUILayout.Width(345));
        else if(fpsOverlay!=null && fpsOverlay.Value && fpsText.Length>0)
            GUI.Box(new Rect(12,12,310,26),fpsText);
    }
    void Draw(int id) {
        GUILayout.Label(status);
        GUI.enabled=state==1 && CanUpscale;
        GUILayout.Label("Super Resolution");
        GUI.enabled=state==1 && HasDlssScaling && HasFsrScaling;
        int scaler=FsrScaling?1:0;
        int selectedScaler=GUILayout.SelectionGrid(scaler,new[]{"DLSS","FSR 3.1"},2);
        if(selectedScaler>=0 && selectedScaler!=scaler)preferFsrScaling.Value=selectedScaler==1;
        GUI.enabled=state==1 && CanUpscale;
        int cur=Array.IndexOf(modes,ActiveQuality);
        int next=GUILayout.SelectionGrid(cur,new[]{"Off",FsrScaling?"Native AA":"DLAA","Quality","Balanced","Performance","Ultra Perf."},3);
        if(next>=0 && next!=cur) SelectQuality(next);
        GUI.enabled=state==1 && ActiveQuality!=Quality.Off;
        if(GUI.enabled && color && output) GUILayout.Label("World render  "+color.width+" x "+color.height+"  ->  "+output.width+" x "+output.height);
        GUI.enabled=GUI.enabled && !FsrScaling;
        bool force=GUILayout.Toggle(forceSecondGen.Value,"Force DLSS 4.5 model for all presets");
        if(force!=forceSecondGen.Value) forceSecondGen.Value=force;
        GUI.enabled=state==1 && !vulkan && ActiveQuality!=Quality.Off;
        bool native=GUILayout.Toggle(nativePanel.Value,"Native-resolution instruments");
        if(nativePanel.Value!=native)nativePanel.Value=native;
        GUI.enabled=state==1 && CanSelectDLSS;
        GUILayout.Label("Frame Generation Provider");
        int index=ProviderIndex(provider.Value);
        int selected=GUILayout.SelectionGrid(index,providers,2);
        if(selected>=0 && selected!=index)provider.Value=providers[selected];
        GUI.enabled=true;
        if(ProviderNote!=null)GUILayout.Label(ProviderNote);
        GUI.enabled=state==1 && max>=1;
        bool enabled=GUILayout.Toggle(fg.Value,mfg.Value && max>1?"Frame Generation":"2x Frame Generation");
        if(enabled!=fg.Value) fg.Value=enabled;
        GUI.enabled=state==1 && max>=1 && fg.Value && hasReflex;
        bool boost=GUILayout.Toggle(reflexBoost.Value,"NVIDIA Reflex On + Boost");
        if(boost!=reflexBoost.Value) reflexBoost.Value=boost;
        bool cap=GUILayout.Toggle(capToRefresh.Value,refreshCapLabel);
        if(cap!=capToRefresh.Value) capToRefresh.Value=cap;
        GUI.enabled=state==1 && fg.Value && max>=2;
        bool multi=GUILayout.Toggle(mfg.Value,"Multi Frame Generation");
        if(multi!=mfg.Value) mfg.Value=multi;
        GUI.enabled=GUI.enabled && mfg.Value;
        if(dyn!=0) {bool v=GUILayout.Toggle(dynamic.Value,"Dynamic multiplier"); if(v!=dynamic.Value) dynamic.Value=v;}
        GUI.enabled=GUI.enabled && !(dyn!=0 && dynamic.Value);
        GUILayout.BeginHorizontal();
        for(int i=3;i<=Math.Min(max+1,6);i++)
            if(GUILayout.Toggle(i==multiplier.Value,i+"x",GUI.skin.button) && i!=multiplier.Value) multiplier.Value=i;
        GUILayout.EndHorizontal(); GUI.enabled=true;
        bool rates=GUILayout.Toggle(fpsOverlay.Value,"Show FPS while flying");
        if(rates!=fpsOverlay.Value) fpsOverlay.Value=rates;
        if(fg.Value && max>0 && !gameForeground) GUILayout.Label("Frame generation paused while the game is in the background");
        else if(fg.Value && max>0 && Time.timeScale<=0) GUILayout.Label("Frame generation paused with the simulation");
        if(vulkan)GUILayout.Label("FSR frames generated  "+vkGenerated);
        else GUILayout.Label((FsrScaling?"FSR evaluations  ":"DLSS evaluations  ")+evals+"     Latest presents  "+presented);
        if(GUILayout.Button("Close (F8)")) SetMenu(false);
        GUI.DragWindow();
    }
    void OnDestroy() {
        SetMenu(false);
        if(earlyReflex) SetReflexLoop(false);

        if(!vulkan && callback!=IntPtr.Zero) ReleaseReflexInput();
        RenderPipelineManager.beginFrameRendering-=RenderStart;
        if(!vulkan && callback!=IntPtr.Zero) Issue(4,IntPtr.Zero);
        if(patches!=null) patches.UnpatchSelf();
        if(menuHooks!=null) menuHooks.UnpatchSelf();
        Release(); self=null;
    }
}

