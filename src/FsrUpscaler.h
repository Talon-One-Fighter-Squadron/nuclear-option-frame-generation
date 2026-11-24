// Included by each graphics bridge. The Vulkan and Direct3D loaders expose the
// same FidelityFX API, but each context belongs to its own device and backend.
#include "upscalers/include/ffx_upscale.h"

class FsrUpscaler {
    ffxContext context{};
    uint64_t provider{};
    uint32_t width{},height{};
public:
    bool available(ffxFunctions& api,void* device) {
        provider=0;
        uint64_t count=0;
        ffxQueryDescGetVersions query{{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS},FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,device,&count};
        if(!api.Query || api.Query(nullptr,&query.header)!=FFX_API_RETURN_OK || !count)return false;
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        query.versionIds=ids.data();query.versionNames=names.data();
        if(api.Query(nullptr,&query.header)!=FFX_API_RETURN_OK)return false;
        for(size_t i=0;i<count;i++)if(names[i] && strncmp(names[i],"3.1.",4)==0){provider=ids[i];return true;}
        return false;
    }
    void release(ffxFunctions& api) {
        if(context)api.DestroyContext(&context,nullptr);
        context=nullptr;width=height=0;
    }
    template<class Wait>
    bool dispatch(ffxFunctions& api,ffxCreateContextDescHeader& backend,ffxDispatchDescUpscale& frame,Wait wait) {
        if(!provider)return false;
        if(width!=frame.upscaleSize.width || height!=frame.upscaleSize.height || !context) {
            if(context){wait();release(api);}
            ffxOverrideVersion selection{{FFX_API_DESC_TYPE_OVERRIDE_VERSION,&backend},provider};
            ffxCreateContextDescUpscaleVersion version{{FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION,&selection.header},FFX_UPSCALER_VERSION};
            ffxCreateContextDescUpscale create{{FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,&version.header},
                FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE|FFX_UPSCALE_ENABLE_AUTO_EXPOSURE|FFX_UPSCALE_ENABLE_DEPTH_INVERTED,
                frame.upscaleSize,frame.upscaleSize};
            if(api.CreateContext(&context,&create.header,nullptr)!=FFX_API_RETURN_OK)return false;
            width=frame.upscaleSize.width;height=frame.upscaleSize.height;frame.reset=true;
        }
        frame.preExposure=1;frame.viewSpaceToMetersFactor=1;
        return api.Dispatch(&context,&frame.header)==FFX_API_RETURN_OK;
    }
};
