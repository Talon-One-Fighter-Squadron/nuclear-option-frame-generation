#pragma once
#include "api/include/ffx_api_loader.h"
#include "framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "framegeneration/include/ffx_framegeneration.h"

// AMD's 3.x algorithm and its DXGI swap chain are separate versioned providers.
// Select both explicitly so a driver override cannot pair the wrong implementations.
class FsrGeneration {
    ffxFunctions api{};
    ffxContext presentation{}, effect{};
    struct Creation { ffxContext context{}; ffxReturnCode_t result{}; };
    std::future<Creation> pending;
    ComPtr<IDXGISwapChain4> chain;
    ComPtr<ID3D12CommandQueue> queue;
    uint64_t effectVersion{};
    UINT width{},height{},previousPresents{};
    DXGI_FORMAT format{};
    bool enabled{},sampled{};
    std::mutex dispatchLock;
    std::atomic<unsigned> dispatchError{0};
    std::atomic<bool> loggedDispatch{false};
    static void checked(ffxReturnCode_t code,const char* operation) {
        if(code!=FFX_API_RETURN_OK) throw std::runtime_error(std::string(operation)+" (FidelityFX "+std::to_string(code)+")");
    }
    uint64_t version(ID3D12Device* dev,uint64_t type,const char* name) {
        uint64_t count=0;
        ffxQueryDescGetVersions query{{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS},type,dev,&count};
        checked(api.Query(nullptr,&query.header),"Query FSR providers");
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        if(count) {
            query.versionIds=ids.data();query.versionNames=names.data();
            checked(api.Query(nullptr,&query.header),"Enumerate FSR providers");
            for(size_t i=0;i<count;i++) if(names[i] && strcmp(names[i],name)==0) return ids[i];
        }
        throw std::runtime_error(std::string("FSR provider unavailable: ")+name);
    }
    static ffxReturnCode_t interpolate(ffxDispatchDescFrameGeneration* frame,void* context) {
        auto& self=*static_cast<FsrGeneration*>(context);
        std::lock_guard<std::mutex> lock(self.dispatchLock);
        auto result=self.api.Dispatch(&self.effect,&frame->header);
        if(result!=FFX_API_RETURN_OK) self.dispatchError=result;
        else if(!self.loggedDispatch.exchange(true)) trace("FSR 3.1.6 interpolation dispatched on primary swap chain");
        return result;
    }
    void drain() {
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{{FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12}};
        checked(api.Dispatch(&presentation,&wait.header),"Wait for FSR presents");
        // Prepare work may still be queued after presentation was disabled.
        ComPtr<ID3D12Device> dev;
        ComPtr<ID3D12Fence> fence;
        check(queue->GetDevice(IID_PPV_ARGS(&dev)),"FSR device");
        check(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"FSR drain fence");
        check(queue->Signal(fence.Get(),1),"Signal FSR drain");
        check(fence->SetEventOnCompletion(1,nullptr),"Wait for FSR queue");
    }
    void releaseEffect() {
        if(!effect) return;
        disable();
        drain();
        checked(api.DestroyContext(&effect,nullptr),"Destroy FSR generation context");
        effect=nullptr;
    }
public:
    bool owns(IDXGISwapChain* current) const {return chain && current==static_cast<IDXGISwapChain*>(chain.Get());}
    // Create the replacement directly. Creating then releasing an extra real
    // flip-model swap chain for the same HWND can fail on Windows.
    void create(const std::wstring& folder,IDXGIFactory* factory,ID3D12CommandQueue* gameQueue,
                HWND hwnd,DXGI_SWAP_CHAIN_DESC1* desc,DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                DXGI_SWAP_CHAIN_DESC* legacy) {
        if(presentation) shutdown();
        if(!api.CreateContext) {
            const auto file=std::filesystem::path(folder)/L"amd_fidelityfx_loader_dx12.dll";
            HMODULE loader=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!loader) throw std::runtime_error("AMD FidelityFX loader unavailable");
            ffxLoadFunctions(&api,loader);
            if(!api.CreateContext || !api.DestroyContext || !api.Dispatch || !api.Configure || !api.Query)
                throw std::runtime_error("Incomplete AMD FidelityFX loader");
        }
        ComPtr<ID3D12Device> dev;
        check(gameQueue->GetDevice(IID_PPV_ARGS(&dev)),"FSR adapter");
        effectVersion=version(dev.Get(),FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,"3.1.6");
        const auto type=legacy?FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12:
                               FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        ffxOverrideVersion select{{FFX_API_DESC_TYPE_OVERRIDE_VERSION},version(dev.Get(),type,"3.1.7")};
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 abi{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12,&select.header},FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION};
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 window{{type,&abi.header},chain.GetAddressOf(),hwnd,desc,fullscreen,factory,gameQueue};
        ffxCreateContextDescFrameGenerationSwapChainNewDX12 old{{type,&abi.header},chain.GetAddressOf(),legacy,factory,gameQueue};
        checked(api.CreateContext(&presentation,legacy?&old.header:&window.header,nullptr),"Create AMD primary swap chain");
        queue=gameQueue;
        trace("Created FSR 3.1.7 primary swap chain");
    }
    HRESULT get(REFIID iid,void** result) {return chain.CopyTo(iid,result);}
    void disable() {
        if(enabled) {
            ffxConfigureDescFrameGeneration off{{FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION}};
            off.swapChain=chain.Get();
            checked(api.Configure(&effect,&off.header),"Disable FSR interpolation");
            enabled=false;
        }
        sampled=false;
    }
    void shutdown() {
        releaseEffect();
        if(pending.valid()) {
            auto unused=pending.get();
            if(unused.context) checked(api.DestroyContext(&unused.context,nullptr),"Discard pending FSR context");
        }
        if(presentation) {
            checked(api.DestroyContext(&presentation,nullptr),"Destroy AMD swap chain context");
            presentation=nullptr;
        }
        chain.Reset();queue.Reset();sampled=false;
    }
    bool ensureEffect(ID3D12Device* dev,UINT w,UINT h,DXGI_FORMAT backBufferFormat) {
        const bool matches=width==w && height==h && format==backBufferFormat;
        if(effect && matches) return true;
        if(pending.valid()) {
            if(pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return false;
            auto completed=pending.get();
            if(matches && completed.result==FFX_API_RETURN_OK) {
                effect=completed.context;
                trace("FSR generation context ready");
                return true;
            }
            if(completed.context) checked(api.DestroyContext(&completed.context,nullptr),"Discard unused FSR context");
            if(matches) checked(completed.result,"Create FSR generation context");
        }
        releaseEffect();
        width=w;height=h;format=backBufferFormat;
        // Only the worker owns this unpublished context. All use of the active
        // context and primary chain stays on their existing presentation paths.
        pending=std::async(std::launch::async,[deviceRef=ComPtr<ID3D12Device>(dev),versionId=effectVersion,
                                               createContext=api.CreateContext,w,h,backBufferFormat]() {
            ffxCreateBackendDX12Desc backend{{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12},deviceRef.Get()};
            ffxOverrideVersion select{{FFX_API_DESC_TYPE_OVERRIDE_VERSION,&backend.header},versionId};
            ffxCreateContextDescFrameGenerationHudless hud{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS,&select.header},FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM};
            ffxCreateContextDescFrameGenerationVersion abi{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION,&hud.header},FFX_FRAMEGENERATION_VERSION};
            FfxApiDimensions2D size{w,h};
            ffxCreateContextDescFrameGeneration create{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&abi.header},
                FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED,size,size,ffxApiGetSurfaceFormatDX12(backBufferFormat)};
            Creation built{};
            built.result=createContext(&built.context,&create.header,nullptr);
            return built;
        });
        return false;
    }
    void prepare(ID3D12GraphicsCommandList* list,const Frame& f) {
        checked(dispatchError.exchange(0),"FSR interpolation callback");
        ffxConfigureDescFrameGeneration config{{FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION}};
        config.swapChain=chain.Get();config.frameGenerationEnabled=true;
        config.frameGenerationCallback=interpolate;config.frameGenerationCallbackUserContext=this;
        config.frameID=UINT(f.index);config.generationRect={0,0,f.outWidth,f.outHeight};
        config.HUDLessColor=ffxApiGetResourceDX12(f.hudless);
        // Unity creates a typeless resource with sRGB views. AMD needs the encoded SDR bytes.
        config.HUDLessColor.description.format=FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        checked(api.Configure(&effect,&config.header),"Configure FSR interpolation");
        ffxDispatchDescFrameGenerationPrepareV2 prep{{FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2}};
        prep.reset=!enabled || f.reset;enabled=true;
        prep.frameID=config.frameID;prep.commandList=list;prep.renderSize={UINT(f.width),UINT(f.height)};
        prep.jitterOffset={f.jitterX,f.jitterY};prep.motionVectorScale={float(f.width),float(f.height)};
        prep.frameTimeDelta=f.frameTimeMs;prep.cameraNear=f.nearZ;prep.cameraFar=f.farZ;
        prep.cameraFovAngleVertical=f.fov;prep.viewSpaceToMetersFactor=1;
        prep.depth=ffxApiGetResourceDX12(f.mergedDepth);prep.depth.description.format=FFX_API_SURFACE_FORMAT_R32_FLOAT;
        prep.motionVectors=ffxApiGetResourceDX12(f.backwardMotion);prep.motionVectors.description.format=FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        memcpy(prep.cameraPosition,f.vectors[0],12);memcpy(prep.cameraUp,f.vectors[1],12);
        memcpy(prep.cameraRight,f.vectors[2],12);memcpy(prep.cameraForward,f.vectors[3],12);
        std::lock_guard<std::mutex> lock(dispatchLock);
        checked(api.Dispatch(&effect,&prep.header),"Prepare FSR interpolation");
    }
    UINT samplePresents() {
        UINT value=0;
        if(FAILED(chain->GetLastPresentCount(&value))) {sampled=false;return 0;}
        const auto delta=sampled?value-previousPresents:0;
        previousPresents=value;sampled=true;return delta;
    }
};
