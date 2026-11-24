#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>
#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"
#include "api/include/ffx_api_loader.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/vk/ffx_api_framegeneration_vk.h"

#include "FsrUpscaler.h"
namespace {
IUnityGraphicsVulkan* unity;
int eventBase;
PFN_vkGetInstanceProcAddr instanceProc;
PFN_vkGetDeviceProcAddr deviceProc;
VkInstance instance;
VkDevice device;
VkPhysicalDevice physical;
ffxFunctions api{};
ffxContext presentation{}, effect{};
VkSwapchainKHR proxy{};
VkExtent2D extent{};
VkFormat format{};
ffxQueryDescSwapchainReplacementFunctionsVK swapFns{};
bool active, prepared, skipOldDestroy;
std::atomic<bool> supported{}, failed{};
std::atomic<uint64_t> presents{}, dispatches{};
std::mutex renderLock, dispatchLock;
std::atomic<const char*> message{"Vulkan FSR startup is pending"};
struct QueueSlot { uint32_t family,index; };
std::vector<QueueSlot> reserves;
uint32_t graphicsFamily;
uint64_t lastCount{};
FsrUpscaler scaler;
VkDeviceContext scaleDevice{};
std::atomic<bool> scaleAvailable{};
struct Inputs {
    void *depth,*motion,*hudless;
    uint32_t width,height;
    int reset;
    float nearZ,farZ,fov,deltaMs,position[3],up[3],right[3],forward[3];
    void *color,*output; uint32_t outWidth,outHeight; float jitterX,jitterY;
};
uint64_t frameId;

void report(const char* text) {
    message.store(text);
    wchar_t game[MAX_PATH];
    if (!GetModuleFileNameW(nullptr,game,MAX_PATH)) return;
    auto path=std::filesystem::path(game).parent_path()/L"BepInEx"/L"NuclearVulkan.log";
    FILE* file=nullptr;
    if (!_wfopen_s(&file,path.c_str(),L"a")) { fprintf(file,"%s\n",text); fclose(file); }
}
template<class T> T inst(const char* name){return reinterpret_cast<T>(instanceProc(instance,name));}
template<class T> T dev(VkDevice d,const char* name){return reinterpret_cast<T>(deviceProc(d,name));}

VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* original,const VkAllocationCallbacks* alloc,VkInstance* out) {
    auto create=reinterpret_cast<PFN_vkCreateInstance>(instanceProc(nullptr,"vkCreateInstance"));
    auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(instanceProc(nullptr,"vkEnumerateInstanceVersion"));
    uint32_t version=0;
    if (original->pApplicationInfo && enumerate && enumerate(&version)==VK_SUCCESS && version>=VK_API_VERSION_1_2) {
        auto app=*original->pApplicationInfo;
        app.apiVersion=std::max(app.apiVersion,VK_API_VERSION_1_2);
        auto info=*original; info.pApplicationInfo=&app;
        if (create(&info,alloc,out)==VK_SUCCESS) return VK_SUCCESS;
    }
    return create(original,alloc,out);
}

// The proxy owns present timing. Never advertise optional WSI entry points
// that take a driver swapchain handle and have no equivalent in the AMD proxy.
bool proxyIncompatible(const char* name) {
    for(auto ext:{"VK_KHR_present_id","VK_KHR_present_wait","VK_EXT_swapchain_maintenance1",
        "VK_KHR_swapchain_maintenance1","VK_KHR_present_id2","VK_KHR_present_wait2",
        "VK_GOOGLE_display_timing","VK_EXT_full_screen_exclusive","VK_KHR_shared_presentable_image",
        "VK_NV_low_latency2","VK_EXT_display_control","VK_EXT_present_timing"})
        if(!strcmp(name,ext)) return true;
    return false;
}
VkResult VKAPI_CALL enumerateExtensions(VkPhysicalDevice gpu,const char* layer,uint32_t* count,VkExtensionProperties* out) {
    auto fn=inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    if(layer) return fn(gpu,layer,count,out);
    uint32_t n=0; auto result=fn(gpu,nullptr,&n,nullptr);
    if(result!=VK_SUCCESS) return result;
    std::vector<VkExtensionProperties> all(n);
    result=fn(gpu,nullptr,&n,all.data());
    if(result!=VK_SUCCESS) return result;
    all.resize(n);
    all.erase(std::remove_if(all.begin(),all.end(),[](const auto& e){return proxyIncompatible(e.extensionName);}),all.end());
    if(!out){*count=uint32_t(all.size());return VK_SUCCESS;}
    uint32_t copied=std::min(*count,uint32_t(all.size()));
    std::copy_n(all.data(),copied,out); *count=copied;
    return copied==all.size()?VK_SUCCESS:VK_INCOMPLETE;
}
VkResult VKAPI_CALL createDevice(VkPhysicalDevice gpu,const VkDeviceCreateInfo* original,const VkAllocationCallbacks* alloc,VkDevice* out) {
    auto create=inst<PFN_vkCreateDevice>("vkCreateDevice");
    auto fallback=[&](){return create(gpu,original,alloc,out);};
    if(device) return fallback();
    VkPhysicalDeviceProperties props{};
    inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(gpu,&props);
    if(props.apiVersion<VK_API_VERSION_1_2){report("FSR needs Vulkan 1.2 timeline semaphores");return fallback();}
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&timeline};
    inst<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(gpu,&features);
    if(!timeline.timelineSemaphore){report("Timeline semaphores unavailable");return fallback();}
    auto families=inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t n=0;families(gpu,&n,nullptr);
    std::vector<VkQueueFamilyProperties> properties(n);families(gpu,&n,properties.data());
    std::vector<VkDeviceQueueCreateInfo> queues(original->pQueueCreateInfos,original->pQueueCreateInfos+original->queueCreateInfoCount);
    std::vector<std::vector<float>> priorities(n);
    std::vector<QueueSlot> extra;
    bool foundGraphics=false;
    for(uint32_t family=0;family<n;++family) {
        auto it=std::find_if(queues.begin(),queues.end(),[&](const auto& q){return q.queueFamilyIndex==family;});
        uint32_t used=it==queues.end()?0:it->queueCount;
        auto flags=properties[family].queueFlags;
        if(used && !foundGraphics && (flags&VK_QUEUE_GRAPHICS_BIT)){graphicsFamily=family;foundGraphics=true;}
        if(!(flags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT|VK_QUEUE_TRANSFER_BIT)) || (used && it->flags)) continue;
        uint32_t wanted=std::min(used+2,properties[family].queueCount);
        if(wanted<=used) continue;
        priorities[family].assign(wanted,1.f);
        if(used) std::copy_n(it->pQueuePriorities,used,priorities[family].begin());
        else {queues.push_back({VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO});it=queues.end()-1;it->queueFamilyIndex=family;}
        it->queueCount=wanted;it->pQueuePriorities=priorities[family].data();
        for(uint32_t i=used;i<wanted;i++) extra.push_back({family,i});
    }
    if(!foundGraphics || extra.size()<2){report("No spare presentation and acquisition queues");return fallback();}
    VkDeviceCreateInfo info=*original;
    bool supplied=false;
    for(auto node=(const VkBaseInStructure*)info.pNext;node;node=node->pNext) {
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES){
            if(!reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)->timelineSemaphore) return fallback();
            supplied=true;
        }
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES){
            if(!reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->timelineSemaphore) return fallback();
            supplied=true;
        }
    }
    if(!supplied){timeline.pNext=const_cast<void*>(info.pNext);info.pNext=&timeline;}
    info.queueCreateInfoCount=uint32_t(queues.size());info.pQueueCreateInfos=queues.data();
    if(create(gpu,&info,alloc,out)!=VK_SUCCESS){report("FSR device request declined");return fallback();}
    device=*out;physical=gpu;reserves=std::move(extra);
    report("Vulkan device and frame-generation queues ready");
    return VK_SUCCESS;
}
bool loadRuntime() {
    if(api.CreateContext) return api.Query && api.Configure && api.Dispatch && api.DestroyContext;
    wchar_t exe[MAX_PATH]{};
    if(!GetModuleFileNameW(nullptr,exe,MAX_PATH)) return false;
    auto folder=std::filesystem::path(exe).parent_path()/L"BepInEx/plugins/NuclearOptionDLSS/Runtime";
    HMODULE module=LoadLibraryExW((folder/L"amd_fidelityfx_loader_vk.dll").c_str(),nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module) return false;
    ffxLoadFunctions(&api,module);
    return api.CreateContext && api.Query && api.Configure && api.Dispatch && api.DestroyContext;
}
uint64_t findProvider(uint64_t type,const char* name){
    VkDeviceContext gpu{device,physical,deviceProc};
    uint64_t count=0;
    ffxQueryDescGetVersions query{{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS},type,&gpu,&count};
    if(api.Query(nullptr,&query.header)!=FFX_API_RETURN_OK || !count)return 0;
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    query.versionIds=ids.data();query.versionNames=names.data();
    if(api.Query(nullptr,&query.header)!=FFX_API_RETURN_OK)return 0;
    for(size_t i=0;i<count;i++)if(names[i] && strcmp(names[i],name)==0)return ids[i];
    return 0;
}
void disable() {
    if(active && effect) {
        ffxConfigureDescFrameGeneration off{{FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION}};
        off.swapChain=proxy;api.Configure(&effect,&off.header);
    }
    active=false;prepared=false;
}
void releaseEffect() {
    disable();
    if(presentation){
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{{FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK}};
        api.Dispatch(&presentation,&wait.header);
    }
    if(effect){dev<PFN_vkDeviceWaitIdle>(device,"vkDeviceWaitIdle")(device);api.DestroyContext(&effect,nullptr);effect=nullptr;}
}
VkResult VKAPI_CALL createSwap(VkDevice d,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks* alloc,VkSwapchainKHR* out) {
    auto fallback=[&](){return dev<PFN_vkCreateSwapchainKHR>(d,"vkCreateSwapchainKHR")(d,info,alloc,out);};
    if(d!=device || !loadRuntime()) return fallback();
    std::lock_guard<std::mutex> guard(renderLock);
    if(proxy){
        if(info->oldSwapchain!=proxy) return fallback();
        releaseEffect();
        auto result=swapFns.pOutCreateSwapchainFFXAPI(d,info,alloc,out,proxy);
        if(result==VK_SUCCESS){extent=info->imageExtent;format=info->imageFormat;skipOldDestroy=true;lastCount=swapFns.pOutGetLastPresentCountFFXAPI(proxy);}
        return result;
    }
    if(info->oldSwapchain || ffxApiGetSurfaceFormatVK(info->imageFormat)==FFX_API_SURFACE_FORMAT_UNKNOWN) return fallback();
    auto getQueue=dev<PFN_vkGetDeviceQueue>(d,"vkGetDeviceQueue");
    auto presentSupport=inst<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR");
    auto versionId=findProvider(FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK,"3.1.7");
    if(!versionId){report("AMD Vulkan frame-generation swapchain unavailable");return fallback();}
    ffxOverrideVersion select{{FFX_API_DESC_TYPE_OVERRIDE_VERSION},versionId};
    ffxCreateContextDescFrameGenerationSwapChainVersionVK version{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK,&select.header},FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION};
    ffxCreateContextDescFrameGenerationSwapChainVK sc{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK,&version.header}};
    sc.device=d;sc.physicalDevice=physical;sc.swapchain=&proxy;sc.createInfo=*info;sc.allocator=const_cast<VkAllocationCallbacks*>(alloc);
    sc.gameQueue.familyIndex=graphicsFamily;getQueue(d,graphicsFamily,0,&sc.gameQueue.queue);
    for(auto slot:reserves){
        VkBool32 canPresent{};
        if(presentSupport(physical,slot.family,info->surface,&canPresent)!=VK_SUCCESS || !canPresent)continue;
        sc.presentQueue.familyIndex=slot.family;getQueue(d,slot.family,slot.index,&sc.presentQueue.queue);break;
    }
    for(auto slot:reserves){
        VkQueue q{};getQueue(d,slot.family,slot.index,&q);
        if(q==sc.presentQueue.queue)continue;
        sc.imageAcquireQueue={q,slot.family,nullptr};break;
    }
    if(!sc.presentQueue.queue || !sc.imageAcquireQueue.queue){report("No independent queue can present to this surface");return fallback();}
    if(api.CreateContext(&presentation,&sc.header,nullptr)!=FFX_API_RETURN_OK){report("AMD Vulkan swapchain creation failed");return fallback();}
    swapFns={{FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK}};
    if(api.Query(&presentation,&swapFns.header)!=FFX_API_RETURN_OK){report("AMD Vulkan swapchain functions unavailable");return VK_ERROR_INITIALIZATION_FAILED;}
    extent=info->imageExtent;format=info->imageFormat;lastCount=0;failed=false;
    *out=proxy;supported=true;report("Vulkan FSR 2x ready");return VK_SUCCESS;
}
void VKAPI_CALL destroySwap(VkDevice d,VkSwapchainKHR chain,const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::mutex> guard(renderLock);
    if(chain && chain==proxy){
        if(skipOldDestroy){skipOldDestroy=false;return;}
        supported=false;releaseEffect();api.DestroyContext(&presentation,nullptr);presentation=nullptr;
        swapFns.pOutDestroySwapchainFFXAPI(d,chain,alloc,chain);proxy=nullptr;return;
    }
    dev<PFN_vkDestroySwapchainKHR>(d,"vkDestroySwapchainKHR")(d,chain,alloc);
}
VkResult VKAPI_CALL getImages(VkDevice d,VkSwapchainKHR chain,uint32_t* n,VkImage* out) {
    if(chain && chain==proxy) return swapFns.pOutGetSwapchainImagesKHR(d,chain,n,out);
    return dev<PFN_vkGetSwapchainImagesKHR>(d,"vkGetSwapchainImagesKHR")(d,chain,n,out);
}
VkResult VKAPI_CALL acquire(VkDevice d,VkSwapchainKHR chain,uint64_t time,VkSemaphore sem,VkFence fence,uint32_t* index){
    if(chain && chain==proxy) return swapFns.pOutAcquireNextImageKHR(d,chain,time,sem,fence,index);
    return dev<PFN_vkAcquireNextImageKHR>(d,"vkAcquireNextImageKHR")(d,chain,time,sem,fence,index);
}
VkResult VKAPI_CALL acquire2(VkDevice d,const VkAcquireNextImageInfoKHR* info,uint32_t* index){
    if(info->swapchain==proxy && proxy) return acquire(d,proxy,info->timeout,info->semaphore,info->fence,index);
    return dev<PFN_vkAcquireNextImage2KHR>(d,"vkAcquireNextImage2KHR")(d,info,index);
}
PFN_vkQueuePresentKHR realPresent;
VkResult VKAPI_CALL present(VkQueue queue,const VkPresentInfoKHR* info){
    std::lock_guard<std::mutex> guard(renderLock);
    if(!proxy || std::find(info->pSwapchains,info->pSwapchains+info->swapchainCount,proxy)==info->pSwapchains+info->swapchainCount)
        return realPresent(queue,info);
    if(info->swapchainCount!=1)return VK_ERROR_OUT_OF_DATE_KHR;
    if(!prepared) disable();
    prepared=false;
    auto result=swapFns.pOutQueuePresentKHR(queue,info);
    uint64_t count=swapFns.pOutGetLastPresentCountFFXAPI(proxy);
    if(count>=lastCount)presents+=count-lastCount;
    lastCount=count;
    if(info->pResults)info->pResults[0]=result;
    return result;
}
void VKAPI_CALL hdr(VkDevice d,uint32_t n,const VkSwapchainKHR* chains,const VkHdrMetadataEXT* metadata){
    auto real=dev<PFN_vkSetHdrMetadataEXT>(d,"vkSetHdrMetadataEXT");
    for(uint32_t i=0;i<n;i++)
        if(chains[i] && chains[i]==proxy)swapFns.pOutSetHdrMetadataEXT(d,1,chains+i,metadata+i);
        else real(d,1,chains+i,metadata+i);
}
void VKAPI_CALL destroyDevice(VkDevice d,const VkAllocationCallbacks* alloc) {
    if(d==scaleDevice.vkDevice){
        scaleAvailable=false;
        dev<PFN_vkDeviceWaitIdle>(d,"vkDeviceWaitIdle")(d);scaler.release(api);scaleDevice={};
    }
    if(d==device){device=nullptr;physical=nullptr;reserves.clear();}
    dev<PFN_vkDestroyDevice>(d,"vkDestroyDevice")(d,alloc);
}
PFN_vkVoidFunction deviceEntry(const char* name,PFN_vkVoidFunction original){
    if(!original)return nullptr;
#define ENTRY(n,f) if(!strcmp(name,#n))return reinterpret_cast<PFN_vkVoidFunction>(&f)
    ENTRY(vkCreateSwapchainKHR,createSwap);ENTRY(vkDestroySwapchainKHR,destroySwap);ENTRY(vkGetSwapchainImagesKHR,getImages);
    ENTRY(vkAcquireNextImageKHR,acquire);ENTRY(vkAcquireNextImage2KHR,acquire2);ENTRY(vkSetHdrMetadataEXT,hdr);ENTRY(vkDestroyDevice,destroyDevice);
    if(!strcmp(name,"vkQueuePresentKHR")){realPresent=reinterpret_cast<PFN_vkQueuePresentKHR>(original);return reinterpret_cast<PFN_vkVoidFunction>(&present);}
    return original;
}
PFN_vkVoidFunction VKAPI_CALL getDeviceProc(VkDevice d,const char* name){return deviceEntry(name,deviceProc(d,name));}
PFN_vkVoidFunction VKAPI_CALL getInstanceProc(VkInstance i,const char* name){
    auto original=instanceProc(i,name);
    if(!original)return nullptr;
    if(i){instance=i;deviceProc=reinterpret_cast<PFN_vkGetDeviceProcAddr>(instanceProc(i,"vkGetDeviceProcAddr"));}
    ENTRY(vkGetInstanceProcAddr,getInstanceProc);ENTRY(vkGetDeviceProcAddr,getDeviceProc);
    ENTRY(vkCreateInstance,createInstance);ENTRY(vkCreateDevice,createDevice);
    ENTRY(vkEnumerateDeviceExtensionProperties,enumerateExtensions);
    return deviceEntry(name,original);
}
#undef ENTRY
PFN_vkGetInstanceProcAddr UNITY_INTERFACE_API initialize(PFN_vkGetInstanceProcAddr next,void*){instanceProc=next;return getInstanceProc;}
ffxReturnCode_t interpolate(ffxDispatchDescFrameGeneration* desc,void*){
    std::lock_guard<std::mutex> guard(dispatchLock);
    auto result=api.Dispatch(&effect,&desc->header);
    if(result==FFX_API_RETURN_OK)++dispatches;
    else failed=true;
    return result;
}
bool ensureEffect(){
    if(effect)return true;
    ffxCreateBackendVKDesc backend{{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},{device,physical,deviceProc}};
    auto id=findProvider(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,"3.1.6");
    if(!id){failed=true;report("AMD Vulkan FSR 3.1 interpolation unavailable");return false;}
    ffxOverrideVersion select{{FFX_API_DESC_TYPE_OVERRIDE_VERSION,&backend.header},id};
    ffxCreateContextDescFrameGenerationHudless hud{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS,&select.header},FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM};
    ffxCreateContextDescFrameGenerationVersion abi{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION,&hud.header},FFX_FRAMEGENERATION_VERSION};
    FfxApiDimensions2D size{extent.width,extent.height};
    ffxCreateContextDescFrameGeneration create{{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&abi.header},
        FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED,size,size,ffxApiGetSurfaceFormatVK(format)};
    if(api.CreateContext(&effect,&create.header,nullptr)==FFX_API_RETURN_OK){report("Vulkan FSR interpolation context ready");return true;}
    failed=true;report("AMD Vulkan interpolation context unavailable");return false;
}
bool accessImage(void* texture,FfxApiResource& resource,bool write=false){
    UnityVulkanImage image{};
    if(!texture || !unity->AccessTexture(texture,nullptr,write?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,write?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT,kUnityVulkanResourceAccess_PipelineBarrier,&image))return false;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=image.type;info.format=image.format;info.extent=image.extent;info.mipLevels=image.mipCount;info.arrayLayers=image.layers;info.usage=image.usage;
    resource=ffxApiGetResourceVK(image.image,ffxApiGetImageResourceDescriptionVK(image.image,info,0),write?FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:FFX_API_RESOURCE_STATE_COMPUTE_READ);
    return true;
}
void UNITY_INTERFACE_API render(int id,void* data){
    std::unique_ptr<Inputs> input(static_cast<Inputs*>(data));
    std::lock_guard<std::mutex> guard(renderLock);
    if(!unity)return;
    if(id==eventBase+2){
        if(!scaleAvailable || !input)return;
        auto& f=*input;
        ffxDispatchDescUpscale frame{{FFX_API_DISPATCH_DESC_TYPE_UPSCALE}};
        if(!accessImage(f.color,frame.color) || !accessImage(f.depth,frame.depth) ||
           !accessImage(f.motion,frame.motionVectors) || !accessImage(f.output,frame.output,true))return;
        UnityVulkanRecordingState recording{};
        if(!unity->CommandRecordingState(&recording,kUnityVulkanGraphicsQueueAccess_DontCare) || !recording.commandBuffer)return;
        ffxCreateBackendVKDesc backend{{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},scaleDevice};
        frame.commandList=recording.commandBuffer;frame.renderSize={f.width,f.height};frame.upscaleSize={f.outWidth,f.outHeight};
        frame.jitterOffset={f.jitterX,f.jitterY};frame.motionVectorScale={-float(f.width),float(f.height)};
        frame.frameTimeDelta=f.deltaMs;frame.cameraNear=f.nearZ;frame.cameraFar=f.farZ;frame.cameraFovAngleVertical=f.fov;frame.reset=f.reset;
        auto wait=[](){auto fn=(PFN_vkDeviceWaitIdle)scaleDevice.vkDeviceProcAddr(scaleDevice.vkDevice,"vkDeviceWaitIdle");fn(scaleDevice.vkDevice);};
        if(!scaler.dispatch(api,backend.header,frame,wait)){scaleAvailable=false;report("FSR upscaling unavailable; restoring game resolution");}
        return;
    }
    if(!proxy || failed)return;
    if(id==eventBase+1){ensureEffect();return;}
    if(!input || !ensureEffect())return;
    auto& f=*input;
    if(f.width>extent.width || f.height>extent.height || !f.width || !f.height)return;
    ffxDispatchDescFrameGenerationPrepareV2 prep{{FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2}};
    ffxConfigureDescFrameGeneration config{{FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION}};
    if(!accessImage(f.depth,prep.depth) || !accessImage(f.motion,prep.motionVectors) || !accessImage(f.hudless,config.HUDLessColor))return;
    UnityVulkanRecordingState recording{};
    if(!unity->CommandRecordingState(&recording,kUnityVulkanGraphicsQueueAccess_DontCare) || !recording.commandBuffer)return;
    prep.commandList=recording.commandBuffer;prep.frameID=++frameId;prep.renderSize={f.width,f.height};
    prep.motionVectorScale={-float(f.width),float(f.height)};
    prep.jitterOffset={f.jitterX,f.jitterY};prep.reset=!active || f.reset; prep.frameTimeDelta=f.deltaMs;prep.cameraNear=f.nearZ;prep.cameraFar=f.farZ;
    prep.cameraFovAngleVertical=f.fov;prep.viewSpaceToMetersFactor=1;
    memcpy(prep.cameraPosition,f.position,12);memcpy(prep.cameraUp,f.up,12);memcpy(prep.cameraRight,f.right,12);memcpy(prep.cameraForward,f.forward,12);
    config.HUDLessColor.description.format=FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    config.swapChain=proxy;config.frameID=prep.frameID;config.frameGenerationEnabled=true;
    config.generationRect={0,0,int(extent.width),int(extent.height)};
    config.frameGenerationCallback=interpolate;
    if(api.Configure(&effect,&config.header)!=FFX_API_RETURN_OK){failed=true;report("Vulkan FSR configure failed");return;}
    active=true;
    std::lock_guard<std::mutex> dispatch(dispatchLock);
    if(api.Dispatch(&effect,&prep.header)!=FFX_API_RETURN_OK){failed=true;report("Vulkan FSR prepare failed");return;}
    prepared=true;
}
void attach(IUnityInterfaces* api){
    unity=api->Get<IUnityGraphicsVulkan>();
    if(!eventBase && unity)if(auto graphics=api->Get<IUnityGraphics>())eventBase=graphics->ReserveEventIDRange(3);
}
}
#define EXPORT extern "C" __declspec(dllexport)
EXPORT void UNITY_INTERFACE_API XRSDKPreInit(IUnityInterfaces* api){
    attach(api);
    if(unity)unity->InterceptInitialization(initialize,nullptr);
}
EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* api){attach(api);}
EXPORT void* VulkanSetup(int* first){
    if(!unity)return nullptr;
    auto state=unity->Instance();
    if(!state.device || !state.getInstanceProcAddr)return nullptr;
    scaleDevice={state.device,state.physicalDevice,reinterpret_cast<PFN_vkGetDeviceProcAddr>(state.getInstanceProcAddr(state.instance,"vkGetDeviceProcAddr"))};
    if(loadRuntime())scaleAvailable=scaler.available(api,&scaleDevice);
    UnityVulkanPluginEventConfig cfg{kUnityVulkanRenderPass_EnsureOutside,kUnityVulkanGraphicsQueueAccess_DontCare,kUnityVulkanEventConfigFlag_ModifiesCommandBuffersState|kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission};
    unity->ConfigureEvent(eventBase,&cfg);
    unity->ConfigureEvent(eventBase+1,&cfg);
    unity->ConfigureEvent(eventBase+2,&cfg);
    *first=eventBase;return reinterpret_cast<void*>(&render);
}
EXPORT void* VulkanPack(const Inputs* input){return new(std::nothrow) Inputs(*input);}
EXPORT int VulkanUpscaleAvailable(){return scaleAvailable?1:0;}
EXPORT int VulkanStatus(uint64_t* total,uint64_t* generated,char* text,int length){
    *total=presents.load();*generated=dispatches.load();strncpy_s(text,length,message.load(),_TRUNCATE);
    return supported && !failed;
}
