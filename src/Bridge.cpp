#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <future>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <vector>
#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"
#include "sl.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "sl_pcl.h"
#include "sl_security.h"

using Microsoft::WRL::ComPtr;
#define API extern "C" __declspec(dllexport)
namespace {
constexpr auto readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto writeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto copyState = D3D12_RESOURCE_STATE_COPY_SOURCE;
const sl::ViewportHandle view{0};
IUnityGraphicsD3D12v7* unity;
IUnityGraphics* graphics;
ID3D12Device* device;
int firstEvent;
std::wstring libraryPath, logPath;
bool started;
std::atomic<int> ready{0}, maxGenerated{0}, dynamicMfg{0}, presented{0}, evaluations{0};
std::atomic<uint64_t> runtimePresents{0};
std::mutex messageLock;
std::string message="Waiting for Direct3D 12";
void report(std::string s) { std::lock_guard<std::mutex> lock(messageLock); message=std::move(s); }
void trace(const char* step) {
    if(!logPath.empty()) std::ofstream(std::filesystem::path(logPath)/"DLSS-Presentation.log",std::ios::app)<<GetTickCount64()<<" "<<GetCurrentThreadId()<<" "<<step<<std::endl;
}
void check(HRESULT h, const char* op) {
    if (FAILED(h)) throw std::runtime_error(std::string(op)+" HRESULT "+std::to_string(h));
}
void check(sl::Result r,const char* op) {
    if (r!=sl::Result::eOk) throw std::runtime_error(std::string(op)+" Streamline "+std::to_string(int(r)));
}
struct Frame {
    int index, mode, generated, reset;
    int width, height, outWidth, outHeight;
    float jitterX, jitterY, nearZ, farZ, fov, aspect;
    float matrices[4][16];
    float vectors[4][3];
    ID3D12Resource *color,*depth,*motion,*output,*hudless,*finalColor;
    ID3D12Resource *previousColor,*beforeCockpit,*afterCockpit,*cockpitDepth,*cockpitMotion;
    ID3D12Resource *mergedDepth,*backwardMotion,*ui;
    float cockpitNear,cockpitFar,deltaJitterX,deltaJitterY;
    int hasCockpit,flags;
    ID3D12Resource *nativeCockpit,*detailed,*coverage,*worldInput,*worldMotion;
    float frameTimeMs;
};
static_assert(sizeof(Frame)==544);
struct Packet {
    Frame f;
    std::vector<ComPtr<ID3D12Resource>> holds;
    explicit Packet(const Frame& in):f(in) {
        holds.reserve(19);
        for(auto* r:{f.color,f.depth,f.motion,f.output,f.hudless,f.finalColor,f.previousColor,
            f.beforeCockpit,f.afterCockpit,f.cockpitDepth,f.cockpitMotion,f.mergedDepth,f.backwardMotion,f.ui,f.nativeCockpit,f.detailed,f.coverage,f.worldInput,f.worldMotion})
            if(r) holds.emplace_back(r);
    }
};
struct RenderSizes {
    int width{},height{};
    sl::DLSSOptimalSettings modes[7]{};
};
std::mutex sizeLock;
RenderSizes optimalSizes;
void querySizes(int width,int height) {
    RenderSizes next{};
    next.width=width;next.height=height;
    for(int mode:{1,2,3,4,6}) {
        sl::DLSSOptions opt{};
        opt.mode=sl::DLSSMode(mode);opt.outputWidth=width;opt.outputHeight=height;
        check(slDLSSGetOptimalSettings(opt,next.modes[mode]),"Query DLSS render dimensions");
    }
    std::lock_guard<std::mutex> lock(sizeLock);optimalSizes=next;
}
struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12DescriptorHeap> views,cockpitViews;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fence=0;
    std::vector<ComPtr<ID3D12Resource>> holds;
};
#include "GpuInputs.h"
InputKernels kernels;
std::vector<Slot> slots;
using CommonPresent=HRESULT(*)(IDXGISwapChain*,UINT,UINT,bool&);
using AfterPresent=HRESULT(*)(UINT);
CommonPresent commonPresent;
AfterPresent afterPresent;
bool manualFrame=false;
Slot& begin() {
    // A Unity fence may cover several submissions. Never wait on the frame
    // currently being built from its own render-thread callback.
    auto complete=unity->GetFrameFence()->GetCompletedValue();
    for(auto& slot:slots) {
        if(slot.fence>complete) continue;
        slot.holds.clear();
        check(slot.allocator->Reset(),"Reset allocator");
        check(slot.list->Reset(slot.allocator.Get(),nullptr),"Reset list");
        return slot;
    }
    if(slots.size()>=64) throw std::runtime_error("Unity GPU fence backlog");
    Slot s;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)),"Create allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list)),"Create list");
    slots.emplace_back(std::move(s));
    return slots.back();
}
void submit(Slot& s,std::vector<UnityGraphicsD3D12ResourceState> states,Packet& p) {
    check(s.list->Close(),"Close list");
    s.holds=std::move(p.holds);
    s.fence=unity->ExecuteCommandList(s.list.Get(),int(states.size()),states.data());
}
void transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
    l->ResourceBarrier(1,&b);
}
std::atomic<sl::FrameToken*> presentingToken{nullptr};
void beforeGamePresent(sl::FrameToken* t);
void afterGamePresent(sl::FrameToken* t,HRESULT hr);
#include "FsrGeneration.h"
#include "FsrUpscaler.h"
FsrGeneration fsr;
FsrUpscaler fsrScaler;
ffxFunctions scaleApi{};
bool preferFsr=false;
std::atomic<bool> fsrPrimary{false};
enum AdapterFeatures { detected=1, dlssSR=2, dlssFG=4, radeon=8, geforce=16, fsrSR=32 };
std::atomic<int> adapterFeatures{0};
#include "GamePresentation.h"
constexpr int noMode = -2;
int configured=noMode, taggedFrame=-1;
sl::FrameToken* sceneToken=nullptr;
int lastEvaluation=-1;
int srMode=0, srWidth=0, srHeight=0, srPreset=0;
bool primary=false;
void disableSR() {
    if(srMode==0) return;
    sl::DLSSOptions off{};
    check(slDLSSSetOptions(view,off),"Disable Super Resolution");
    lastEvaluation=-1;
    srMode=0;
}
std::atomic<bool> reflexReady{false};
// The Unity input thread owns the hook and consumes each ping on its next simulation frame.
thread_local HHOOK pingHook=nullptr;
thread_local UINT pingMessage=0;
thread_local bool acceptingPings=false,pendingPing=false;
thread_local int requestedReflexMode=0, appliedReflexMode=0;
thread_local uint32_t appliedFrameLimitUs=0;
thread_local ULONGLONG nextReflexRetry=0;
LRESULT CALLBACK observePing(int code,WPARAM operation,LPARAM data) {
    if(acceptingPings && code==HC_ACTION && operation==PM_REMOVE && reinterpret_cast<const MSG*>(data)->message==pingMessage)
        pendingPing=true;
    return CallNextHookEx(nullptr,code,operation,data);
}
void suspendOutput() {
    if(fsrPrimary) fsr.disable();
    if(configured!=noMode) {
        sl::DLSSGOptions off{};
        off.flags=sl::DLSSGFlags::eRetainResourcesWhenOff;
        check(slDLSSGSetOptions(view,off),"Suspend frame generation");
    }
    configured=noMode; taggedFrame=-1;presentingToken=nullptr;presented=0;
}
bool pluginsInitialized=false;
void loadFeatures() {
    if(pluginsInitialized) return;
    trace("Loading NVIDIA features");
    const sl::Feature features[]={sl::kFeatureDLSS,sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
    const wchar_t* folder=libraryPath.c_str();
    sl::Preferences prefs{};
    prefs.pathsToPlugins=&folder; prefs.numPathsToPlugins=1;
    prefs.pathToLogsAndData=logPath.c_str();
    prefs.featuresToLoad=features; prefs.numFeaturesToLoad=4;
    prefs.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eUseDXGIFactoryProxy|sl::PreferenceFlags::eUseFrameBasedResourceTagging|sl::PreferenceFlags::eDisableCLStateTracking;
    prefs.engine=sl::EngineType::eUnity; prefs.engineVersion="2022.3.62f2";
    prefs.projectId="14405d55-adfc-48dc-a17c-14c2b755eb91";
    check(slInit(prefs),"Initialize");
    pluginsInitialized=true;
}
std::mutex registration;
void initializeRuntime(ID3D12Device* gameDevice) {
    std::lock_guard<std::mutex> lock(registration);
    if(runtimeInitialized) return;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> gpu;
    DXGI_ADAPTER_DESC1 desc{};
    auto luid=gameDevice->GetAdapterLuid();
    check(createSystemFactory(IID_PPV_ARGS(&factory)),"Find DX12 factory");
    check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&gpu)),"Find active DX12 adapter");
    check(gpu->GetDesc1(&desc),"Read active DX12 adapter");
    int features=detected;
    if(desc.VendorId==0x1002) features|=radeon;
    if(desc.VendorId==0x10de) {
        features|=geforce;
        try {
            auto file=(std::filesystem::path(libraryPath)/L"sl.interposer.dll").wstring();
            if(!sl::security::verifyEmbeddedSignature(file.c_str()) ||
               !LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
                throw std::runtime_error("Signed NVIDIA runtime unavailable");
            loadFeatures();
            check(slSetD3DDevice(gameDevice),"Register NVIDIA device");
            sl::AdapterInfo adapter{};
            adapter.deviceLUID=reinterpret_cast<uint8_t*>(&luid);adapter.deviceLUIDSizeInBytes=sizeof(luid);
            if(slIsFeatureSupported(sl::kFeatureDLSS,adapter)==sl::Result::eOk) features|=dlssSR;
            if((features&dlssSR) && slIsFeatureSupported(sl::kFeatureDLSS_G,adapter)==sl::Result::eOk &&
               slIsFeatureSupported(sl::kFeatureReflex,adapter)==sl::Result::eOk) features|=dlssFG;
        } catch(const std::exception& e) {trace(e.what());}
    }
    adapterFeatures=features;
    runtimeInitialized=true;
}
void init() {
    if(!unity || !graphics || graphics->GetRenderer()!=kUnityGfxRendererD3D12)
        throw std::runtime_error("Select DirectX 12 in Steam and restart");
    trace("Unity rendering plugin initialized");
    device=unity->GetDevice();
    initializeRuntime(device);
    started=true;

    kernels.initialize(device);
    const auto loaderPath=std::filesystem::path(libraryPath)/L"amd_fidelityfx_loader_dx12.dll";
    if(auto module=LoadLibraryExW(loaderPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32)){
        ffxLoadFunctions(&scaleApi,module);
        if(fsrScaler.available(scaleApi,device))adapterFeatures.fetch_or(fsrSR);
    }
    if(adapterFeatures.load()&dlssSR) {
        void *before=nullptr,*after=nullptr;
        check(slGetFeatureFunction(sl::kFeatureCommon,"slHookPresent",before),"Common present hook");
        check(slGetFeatureFunction(sl::kFeatureCommon,"slHookAfterPresent",after),"Common post-present hook");
        commonPresent=reinterpret_cast<CommonPresent>(before);afterPresent=reinterpret_cast<AfterPresent>(after);
    }
    ready=1; report("DX12 ready | waiting for primary game swap chain");
}
void checkGameChain() {
    if(fsr.owns(unity->GetSwapChain())) {
        if(!fsrPrimary.exchange(true)) {
            maxGenerated=1;dynamicMfg=0;
            auto luid=device->GetAdapterLuid();
            sl::AdapterInfo adapter{};adapter.deviceLUID=reinterpret_cast<uint8_t*>(&luid);adapter.deviceLUIDSizeInBytes=sizeof(luid);
            reflexReady=(adapterFeatures.load()&dlssSR) && slIsFeatureSupported(sl::kFeatureReflex,adapter)==sl::Result::eOk;
            report("DX12 ready | AMD FSR 3.1.6 (2x)");
        }
        return;
    }
    if(fsrPrimary.exchange(false)) {maxGenerated=0;reflexReady=false;}
    if(!attached || !unity->GetSwapChain()) return;
    if(primary && unity->GetSwapChain()==gameChain.load()) return;
    ComPtr<IDXGISwapChain> native;
    primary=slGetNativeInterface(unity->GetSwapChain(),reinterpret_cast<void**>(native.GetAddressOf()))==sl::Result::eOk;
    if(!primary) {maxGenerated=0;return;}
    trace("Validated Unity primary swap chain");
    auto luid=device->GetAdapterLuid(); sl::AdapterInfo a{};
    a.deviceLUID=reinterpret_cast<uint8_t*>(&luid); a.deviceLUIDSizeInBytes=sizeof(luid);
    if(slIsFeatureSupported(sl::kFeatureDLSS_G,a)==sl::Result::eOk &&
       slIsFeatureSupported(sl::kFeatureReflex,a)==sl::Result::eOk) {
        sl::ReflexOptions ro{}; ro.mode=sl::ReflexMode::eLowLatency;
        sl::DLSSGState state{};
        if(slReflexSetOptions(ro)==sl::Result::eOk && slDLSSGGetState(view,state,nullptr)==sl::Result::eOk) {
            maxGenerated=state.numFramesToGenerateMax;
            dynamicMfg=state.bIsDynamicMFGSupported==sl::Boolean::eTrue;
            reflexReady=true;
        }
    }
    report("DX12 ready | primary game swap chain");
}
sl::FrameToken* token(int index) {
    sl::FrameToken* t=nullptr; auto n=static_cast<uint32_t>(index);
    check(slGetNewFrameToken(t,&n),"Frame token"); return t;
}
sl::FrameToken* configureDLSS(const Frame& f) {
    auto* t=token(f.index);
    sl::Constants c{};
    memcpy(&c.cameraViewToClip,f.matrices[0],64); memcpy(&c.clipToCameraView,f.matrices[1],64);
    memcpy(&c.clipToPrevClip,f.matrices[2],64); memcpy(&c.prevClipToClip,f.matrices[3],64);
    memcpy(&c.cameraPos,f.vectors[0],12); memcpy(&c.cameraUp,f.vectors[1],12);
    memcpy(&c.cameraRight,f.vectors[2],12); memcpy(&c.cameraFwd,f.vectors[3],12);
    c.cameraNear=f.nearZ; c.cameraFar=f.farZ; c.cameraFOV=f.fov; c.cameraAspectRatio=f.aspect;
    c.jitterOffset={f.jitterX,f.jitterY}; c.mvecScale={1,1}; c.cameraPinholeOffset={0,0};
    c.depthInverted=sl::Boolean::eTrue; c.cameraMotionIncluded=sl::Boolean::eTrue;
    c.motionVectors3D=sl::Boolean::eFalse; c.reset=f.reset?sl::Boolean::eTrue:sl::Boolean::eFalse;
    check(slSetConstants(c,*t,view),"Camera constants");
    sl::DLSSOptions options{};
    options.mode=sl::DLSSMode(f.mode); options.outputWidth=f.outWidth; options.outputHeight=f.outHeight;
    // Unity supplies linear FP16, including when its display backbuffer uses SDR.
    options.colorBuffersHDR=sl::Boolean::eTrue;
    options.useAutoExposure=sl::Boolean::eTrue;
    if(f.flags&1) {
        options.dlaaPreset=options.qualityPreset=options.balancedPreset=options.performancePreset=sl::DLSSPreset::ePresetM;
        options.ultraPerformancePreset=sl::DLSSPreset::ePresetL;
    }
    const int preset=f.flags&1;
    if(!f.mode) disableSR();
    else if(srMode!=f.mode || srWidth!=f.outWidth || srHeight!=f.outHeight || srPreset!=preset) {
        check(slDLSSSetOptions(view,options),"DLSS options");
        srMode=f.mode; srWidth=f.outWidth; srHeight=f.outHeight; srPreset=preset;
    }
    return t;
}
void scene(Packet& packet) {
    if(manualFrame){afterPresent(0); manualFrame=false;}
        bool fsrScaling=(packet.f.flags&16)!=0;
    if(!(adapterFeatures.load()&(fsrScaling?fsrSR:dlssSR)))packet.f.mode=0;
    const auto& f=packet.f;
    if(!f.generated) suspendOutput();
        Frame nvidia=f;
    if(fsrScaling)nvidia.mode=0;
    auto* t=(adapterFeatures.load()&dlssSR)?configureDLSS(nvidia):nullptr;
    auto& s=begin();
    constexpr auto rgba=DXGI_FORMAT_R16G16B16A16_FLOAT, rg=DXGI_FORMAT_R16G16_FLOAT, r=DXGI_FORMAT_R32_FLOAT;
    struct Parameters {float matrix[16]; int width,height,reset,cockpit; float planes[4],jitter[2];} params{};
    memcpy(params.matrix,f.matrices[2],64); params.width=f.width;params.height=f.height;
    bool split=f.hasCockpit && f.nativeCockpit && f.mode;
    params.reset=f.reset;params.cockpit=split?((f.flags&2)?3:2):f.hasCockpit;
    params.planes[0]=f.nearZ;params.planes[1]=f.farZ;params.planes[2]=f.cockpitNear;params.planes[3]=f.cockpitFar;
    params.jitter[0]=f.deltaJitterX;params.jitter[1]=f.deltaJitterY;
    kernels.dispatch(s,kernels.temporal.Get(),{{f.color,rgba},{f.depth,r},{f.motion,rg},{f.previousColor,rgba},
        {f.beforeCockpit,rgba},{f.afterCockpit,rgba},{f.cockpitDepth,r},{f.cockpitMotion,rg},{f.coverage,DXGI_FORMAT_R8G8B8A8_UNORM}},
        {{f.mergedDepth,r},{f.backwardMotion,rg},{f.worldInput,rgba},{f.worldMotion,rg}},&params,sizeof(params)/4,f.width,f.height);
    for(auto* res:{f.mergedDepth,f.backwardMotion,f.worldInput,f.worldMotion}) if(res) transition(s.list.Get(),res,writeState,readState);
    sl::Resource color(sl::ResourceType::eTex2d,split?f.worldInput:f.color,readState),depth(sl::ResourceType::eTex2d,split?f.depth:f.mergedDepth,readState),
        motion(sl::ResourceType::eTex2d,split?f.worldMotion:f.backwardMotion,readState),out(sl::ResourceType::eTex2d,f.output,writeState);
    sl::ResourceTag tags[]={{&depth,sl::kBufferTypeDepth,sl::eValidUntilPresent},
        {&motion,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent},
        {&color,sl::kBufferTypeScalingInputColor,sl::eValidUntilEvaluate},
        {&out,sl::kBufferTypeScalingOutputColor,sl::eValidUntilEvaluate}};
    if(t) check(slSetTagForFrame(*t,view,tags,nvidia.mode?4:2,s.list.Get()),"Scene textures");
    if(f.mode) {
        if(fsrScaling){
            ffxCreateBackendDX12Desc backend{{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12},device};
            ffxDispatchDescUpscale frame{{FFX_API_DISPATCH_DESC_TYPE_UPSCALE}};
            frame.commandList=s.list.Get();frame.renderSize={UINT(f.width),UINT(f.height)};
            frame.upscaleSize={UINT(f.outWidth),UINT(f.outHeight)};
            frame.color=ffxApiGetResourceDX12(split?f.worldInput:f.color);
            frame.depth=ffxApiGetResourceDX12(split?f.depth:f.mergedDepth);
            frame.motionVectors=ffxApiGetResourceDX12(split?f.worldMotion:f.backwardMotion);
            frame.output=ffxApiGetResourceDX12(f.output,FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            frame.color.description.format=frame.output.description.format=FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            frame.depth.description.format=FFX_API_SURFACE_FORMAT_R32_FLOAT;
            frame.motionVectors.description.format=FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
            frame.jitterOffset={f.jitterX,f.jitterY};frame.motionVectorScale={float(f.width),float(f.height)};
            frame.frameTimeDelta=f.frameTimeMs;frame.reset=f.reset;frame.cameraNear=f.nearZ;frame.cameraFar=f.farZ;frame.cameraFovAngleVertical=f.fov;
            auto wait=[](){
                ComPtr<ID3D12Fence> fence;
                check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"FSR resize fence");
                check(unity->GetCommandQueue()->Signal(fence.Get(),1),"Signal FSR resize");
                check(fence->SetEventOnCompletion(1,nullptr),"Wait for FSR resize");
            };
            if(!fsrScaler.dispatch(scaleApi,backend.header,frame,wait)){
                adapterFeatures.fetch_and(~fsrSR);report("FSR upscaling unavailable; restoring game resolution");
            }
        }else{
            const sl::BaseStructure* inputs[]={&view};
            check(slEvaluateFeature(sl::kFeatureDLSS,*t,inputs,1,s.list.Get()),"DLSS evaluation");
        }
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource=f.output;
        s.list->ResourceBarrier(1,&b); ++evaluations;lastEvaluation=f.index;
    }
    std::vector<UnityGraphicsD3D12ResourceState> states;
    for(auto* res:{f.color,f.depth,f.motion,f.previousColor,f.beforeCockpit,f.afterCockpit,f.cockpitDepth,f.cockpitMotion,f.coverage})
        if(res) states.push_back({res,readState,readState});
    for(auto* res:{f.mergedDepth,f.backwardMotion,f.worldInput,f.worldMotion}) if(res) states.push_back({res,writeState,readState});
    if(f.nativeCockpit && f.detailed && f.hasCockpit && f.mode) {
        transition(s.list.Get(),f.output,writeState,readState);
        kernels.dispatch(s,kernels.cockpit.Get(),{{f.output,rgba},{f.nativeCockpit,rgba},{f.afterCockpit,rgba},{f.color,rgba},{f.coverage,DXGI_FORMAT_R8G8B8A8_UNORM}},
            {{f.detailed,rgba}},&f.flags,1,f.outWidth,f.outHeight);
        transition(s.list.Get(),f.detailed,writeState,readState);
        states.push_back({f.nativeCockpit,readState,readState});states.push_back({f.detailed,writeState,readState});
        states.push_back({f.output,writeState,readState});
    } else states.push_back({f.output,writeState,writeState});
    submit(s,std::move(states),packet);
    sceneToken=t; taggedFrame=f.index;
}
void copyFinal(Packet& packet) {
    const auto& f=packet.f;
    if((!primary && !fsrPrimary) || !f.generated || !maxGenerated || taggedFrame!=f.index) {suspendOutput();return;}
    // Ask the primary proxy which buffer this real frame will present. Drop
    // stale tags across a resize and release the buffer before returning to Unity.
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12Resource> backBuffer;
    auto* current=unity->GetSwapChain();
    if(!current || FAILED(current->QueryInterface(IID_PPV_ARGS(&chain))) ||
       FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&backBuffer)))) {
        suspendOutput(); return;
    }
    const auto bufferDesc=backBuffer->GetDesc();
    if(bufferDesc.Width!=static_cast<UINT64>(f.outWidth) || bufferDesc.Height!=static_cast<UINT>(f.outHeight)) {
        suspendOutput(); return;
    }
    if(fsrPrimary) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        check(chain->GetDesc1(&desc),"Query FSR presentation format");
        if(fsr.ensureEffect(device,f.outWidth,f.outHeight,desc.Format)) {
            auto& s=begin();
            try {fsr.prepare(s.list.Get(),f);}
            catch(...) {s.list->Close();throw;}
            submit(s,{{f.hudless,readState,readState},{f.mergedDepth,readState,readState},{f.backwardMotion,readState,readState}},packet);
            presented=fsr.samplePresents();
            runtimePresents.fetch_add(presented.load(),std::memory_order_relaxed);
        } else presented=0;
        // Streamline still needs its normal application-frame boundary for DLSS SR.
        if(commonPresent) {bool skip=false;commonPresent(current,0,0,skip);manualFrame=true;}
        return;
    }
    if(configured!=f.generated) {
        sl::DLSSGOptions opt{};
        opt.mode=f.generated<0 && dynamicMfg?sl::DLSSGMode::eDynamic:sl::DLSSGMode::eOn;
        opt.numFramesToGenerate=std::clamp(f.generated,1,maxGenerated.load());
        opt.flags=sl::DLSSGFlags::eEnableFullscreenMenuDetection;
        opt.enableUserInterfaceRecomposition=sl::Boolean::eTrue;
        check(slDLSSGSetOptions(view,opt),"Frame generation options"); configured=f.generated;
    }
    auto& s=begin();
    constexpr auto display=DXGI_FORMAT_R8G8B8A8_UNORM;
    kernels.dispatch(s,kernels.hud.Get(),{{f.finalColor,display},{f.hudless,display}},{{f.ui,display}},nullptr,0,f.outWidth,f.outHeight);
    transition(s.list.Get(),f.ui,writeState,readState);
    sl::Resource hud(sl::ResourceType::eTex2d,f.hudless,readState);
    sl::Resource opacity(sl::ResourceType::eTex2d,f.ui,readState);
    sl::Extent area{0,0,static_cast<uint32_t>(bufferDesc.Width),bufferDesc.Height};
    sl::Resource depth(sl::ResourceType::eTex2d,f.mergedDepth,readState), motion(sl::ResourceType::eTex2d,f.backwardMotion,readState);
    sl::Extent renderArea{0,0,(uint32_t)f.width,(uint32_t)f.height};
    sl::ResourceTag tags[]={{&hud,sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent,&area},
        {&opacity,sl::kBufferTypeUIColorAndAlpha,sl::eValidUntilPresent,&area},
        {&depth,sl::kBufferTypeDepth,sl::eValidUntilPresent,&renderArea},
        {&motion,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent,&renderArea},
        {nullptr,sl::kBufferTypeBackbuffer,sl::eValidUntilPresent,&area}};
    check(slSetTagForFrame(*sceneToken,view,tags,5,s.list.Get()),"HUD recomposition inputs");
    submit(s,{{f.finalColor,readState,readState},{f.hudless,readState,readState},{f.ui,writeState,readState},
        {f.mergedDepth,readState,readState},{f.backwardMotion,readState,readState}},packet);
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd,*sceneToken);
    presentingToken.store(sceneToken);
}
void beforeGamePresent(sl::FrameToken* t) {
    if(t) slPCLSetMarker(sl::PCLMarker::ePresentStart,*t);
}
void afterGamePresent(sl::FrameToken* t,HRESULT result) {
    if(!t) return;
    slPCLSetMarker(sl::PCLMarker::ePresentEnd,*t);
    if(result!=S_OK) {presented=0;return;}
    sl::DLSSGState state{};
    if(slDLSSGGetState(view,state,nullptr)==sl::Result::eOk) {
        presented=state.numFramesActuallyPresented;
        runtimePresents.fetch_add(state.numFramesActuallyPresented,std::memory_order_relaxed);
        if(state.status!=sl::DLSSGStatus::eOk) report("FG status "+std::to_string(unsigned(state.status)));
    }
}
void UNITY_INTERFACE_API event(int id,void* data) {
    std::unique_ptr<Packet> packet((id==firstEvent+1 || id==firstEvent+2)?static_cast<Packet*>(data):nullptr);
    try {
        if(id==firstEvent && !started) init();
        else if(id==firstEvent+5 && ready==1 && (adapterFeatures.load()&dlssSR)) {
            auto size=reinterpret_cast<uintptr_t>(data);
            querySizes(int(size>>32),int(uint32_t(size)));
        }
        else if(id==firstEvent+6 && ready==1 && fsrPrimary && maxGenerated) {
            auto* current=unity->GetSwapChain();
            DXGI_SWAP_CHAIN_DESC desc{};
            if(fsr.owns(current) && SUCCEEDED(current->GetDesc(&desc)) && desc.BufferDesc.Width && desc.BufferDesc.Height)
                fsr.ensureEffect(device,desc.BufferDesc.Width,desc.BufferDesc.Height,desc.BufferDesc.Format);
        }
        else if(id==firstEvent+4 && ready==1) {
            checkGameChain();
            if(lastEvaluation!=int(reinterpret_cast<intptr_t>(data))) disableSR();
            suspendOutput();
            if(primary || !commonPresent) return;
            bool skip=false;
            if(manualFrame) afterPresent(0);
            commonPresent(unity->GetSwapChain(),0,0,skip);
            manualFrame=true;
        }
        else if(ready==1) {
            if(id==firstEvent+1 && packet) scene(*packet);
            if(id==firstEvent+2 && packet) copyFinal(*packet);

        }
    } catch(const std::exception& e) {
        if(id==firstEvent+2 || id==firstEvent+3 || id==firstEvent+6) { maxGenerated=0; try{suspendOutput();}catch(...){} }
        else { ready=-1; }
        trace(e.what());report(e.what());
    }
}
void UNITY_INTERFACE_API deviceEvent(UnityGfxDeviceEventType e) {
    if(e==kUnityGfxDeviceEventShutdown && started) {
        ready=-1; reflexReady=false; srMode=0; lastEvaluation=-1;
        try{suspendOutput();fsr.shutdown();}catch(const std::exception& ex){trace(ex.what());}
        fsrScaler.release(scaleApi);fsrPrimary=false;slots.clear();
        if(pluginsInitialized) {slShutdown();pluginsInitialized=false;}
        commonPresent=nullptr;afterPresent=nullptr;manualFrame=false;primary=false;
        adapterFeatures=0;maxGenerated=0;dynamicMfg=0;started=runtimeInitialized=false;
    }
}
}
API void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* api) {
    graphics=api->Get<IUnityGraphics>(); unity=api->Get<IUnityGraphicsD3D12v7>();
    if(!graphics) return;
    firstEvent=graphics->ReserveEventIDRange(7);
    graphics->RegisterDeviceEventCallback(deviceEvent);
    if(unity && graphics->GetRenderer()==kUnityGfxRendererD3D12) {
        UnityD3D12PluginEventConfig cfg{kUnityD3D12GraphicsQueueAccess_Allow,
            kUnityD3D12EventConfigFlag_FlushCommandBuffers|kUnityD3D12EventConfigFlag_SyncWorkerThreads,false};
        for(int i=0;i<7;i++) unity->ConfigureEvent(firstEvent+i,&cfg);
    }
}
API void UNITY_INTERFACE_API UnityPluginUnload() {
    if(graphics) graphics->UnregisterDeviceEventCallback(deviceEvent);
}
API int VerifyInterposer(const wchar_t* path) {return sl::security::verifyEmbeddedSignature(path)?1:0;}
API int OptimalSize(int mode,int ow,int oh,int* w,int* h) {
    std::lock_guard<std::mutex> lock(sizeLock);
    if(mode<=0 || mode>=7 || ow!=optimalSizes.width || oh!=optimalSizes.height) return 0;
    *w=optimalSizes.modes[mode].optimalRenderWidth;*h=optimalSizes.modes[mode].optimalRenderHeight;
    return *w>0 && *h>0;
}
API void* Setup(const wchar_t* lib,const wchar_t* logs,int* base) {
    if(libraryPath.empty()) {libraryPath=lib;logPath=logs;}
    *base=firstEvent;
    return unity && graphics?reinterpret_cast<void*>(&event):nullptr;
}
API void* Pack(const Frame* f) { try{return new Packet(*f);} catch(...){return nullptr;} }
API void Status(int* state,int* max,int* dyn,int* frames,int* sr,char* info,int length) {
    *state=ready; *max=maxGenerated; *dyn=dynamicMfg; *frames=presented; *sr=evaluations;
    std::lock_guard<std::mutex> lock(messageLock); strncpy_s(info,length,message.c_str(),_TRUNCATE);
}
API uint64_t TotalPresents() {return runtimePresents.load(std::memory_order_relaxed);}
API int UsingFsr() {return fsrPrimary.load()?1:0;}
API int GetAdapterFeatures() {return adapterFeatures.load();}
API int HasReflex() {return reflexReady.load()?1:0;}
API int PrepareGamePresentationV2(const wchar_t* runtime,const wchar_t* logs,int useFsr) {
    preferFsr=useFsr!=0;
    libraryPath=runtime;logPath=logs;
    try {return installGamePresentation();} catch(const std::exception& e) {trace(e.what());return 8;}
}
API int PrepareGamePresentation(const wchar_t* runtime,const wchar_t* logs) {
    return PrepareGamePresentationV2(runtime,logs,0);
}
API int GameIsForeground() {
    HWND game=gameWindow.load(std::memory_order_relaxed);
    return game && GetAncestor(GetForegroundWindow(),GA_ROOT)==game && !IsIconic(game);
}
API void Marker(int frame,int phase) {
    if(!reflexReady || ready!=1) return;
    try {
        auto* t=token(frame);
        slPCLSetMarker(static_cast<sl::PCLMarker>(phase),*t);
    } catch(...) {}
}
API void PauseReflexInput() {
    acceptingPings=false; pendingPing=false;
    if((appliedFrameLimitUs || appliedReflexMode==int(sl::ReflexMode::eLowLatencyWithBoost)) && reflexReady && ready==1) {
        sl::ReflexOptions options{};
        options.mode=sl::ReflexMode::eLowLatency;
        options.idThread=GetCurrentThreadId();
        if(slReflexSetOptions(options)==sl::Result::eOk) {
            appliedReflexMode=int(options.mode);
            appliedFrameLimitUs=0;
        }
    }
    requestedReflexMode=0;
    nextReflexRetry=0;
}
API void ReleaseReflexInput() {
    PauseReflexInput();
    if(pingHook) UnhookWindowsHookEx(pingHook);
    pingHook=nullptr; pingMessage=0; pendingPing=false;
    requestedReflexMode=appliedReflexMode=0;
    appliedFrameLimitUs=0;
}
API int BeginReflexFrame(int frame,int boost,uint32_t frameLimitUs) {
    if(!reflexReady || ready!=1) return 0;
    const auto mode=boost?sl::ReflexMode::eLowLatencyWithBoost:sl::ReflexMode::eLowLatency;
    try {
        if((requestedReflexMode!=int(mode) || appliedFrameLimitUs!=frameLimitUs) &&
            (!nextReflexRetry || GetTickCount64()>=nextReflexRetry)) {
            sl::ReflexOptions options{};
            options.mode=mode;
            options.frameLimitUs=frameLimitUs;
            options.idThread=GetCurrentThreadId();
            const auto result=slReflexSetOptions(options);
            if(result==sl::Result::eOk) {
                requestedReflexMode=appliedReflexMode=int(mode);
                appliedFrameLimitUs=frameLimitUs;
                nextReflexRetry=0;
                trace(((boost?std::string("Reflex On + Boost"):std::string("Reflex On"))+
                    " applied, frame limit us="+std::to_string(frameLimitUs)).c_str());
            } else {
                nextReflexRetry=GetTickCount64()+1000;
                report("Reflex options failed: "+std::to_string(int(result)));
            }
            if(!pingHook) {
                sl::PCLState state{};
                if(slPCLGetState(state)==sl::Result::eOk && state.statsWindowMessage) {
                    pingMessage=state.statsWindowMessage;
                    pingHook=SetWindowsHookExW(WH_GETMESSAGE,observePing,nullptr,options.idThread);
                }
            }
        }
        auto* t=token(frame);
        acceptingPings=true;
        slReflexSleep(*t);
        slPCLSetMarker(sl::PCLMarker::eSimulationStart,*t);
        if(pendingPing) {
            pendingPing=false;
            slPCLSetMarker(sl::PCLMarker::ePCLatencyPing,*t);
        }
        return appliedReflexMode;
    } catch(...) {return 0;}
}

