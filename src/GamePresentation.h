// Used only inside NuclearOption.exe, before Unity creates its D3D12 game swap chain.
#include <MinHook.h>

using CreateLegacyChain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**);
using CreateWindowChain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
CreateLegacyChain originalLegacy = nullptr;
CreateWindowChain originalWindow = nullptr;
thread_local bool handlingGameChain = false;
std::atomic<bool> attached{false};
std::atomic<HWND> gameWindow{nullptr};
bool runtimeInitialized = false;
void initializeRuntime(ID3D12Device* gameDevice);

ComPtr<IDXGIFactory2> gameFactory(IDXGIFactory* factory,IUnknown* creator,HWND hwnd,ComPtr<ID3D12CommandQueue>& amdQueue) {
    if (!hwnd || !creator) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd,&pid);
    wchar_t name[64]{};
    GetClassNameW(hwnd,name,64);
    if (pid != GetCurrentProcessId() || wcscmp(name,L"UnityWndClass") != 0) return {};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> gameDevice;
    if (FAILED(creator->QueryInterface(IID_PPV_ARGS(&queue))) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&gameDevice)))) return {};
    trace("Creating Unity main-window swap chain");
    initializeRuntime(gameDevice.Get());
    gameWindow.store(hwnd,std::memory_order_relaxed);
    const int features=adapterFeatures.load();
    if(preferFsr || !(features&dlssFG)) {
        if(features&(radeon|geforce)) amdQueue=queue;
        return {};
    }
    ComPtr<IDXGIFactory2> base;
    check(factory->QueryInterface(IID_PPV_ARGS(&base)),"Get game factory");
    void* proxy = base.Get();
    check(slUpgradeInterface(&proxy),"Upgrade game factory");
    if (proxy == base.Get()) return {};
    // The Streamline proxy owns the factory reference passed to slUpgradeInterface.
    base.Detach();
    ComPtr<IDXGIFactory2> result;
    result.Attach(static_cast<IDXGIFactory2*>(proxy));
    return result;
}
using GamePresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using GamePresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
GamePresent originalPresent=nullptr;
GamePresent1 originalPresent1=nullptr;
std::atomic<IDXGISwapChain*> gameChain{nullptr};
HRESULT STDMETHODCALLTYPE presentPrimary(IDXGISwapChain* swap,UINT interval,UINT flags) {
    auto* t=(swap==gameChain && !(flags&DXGI_PRESENT_TEST))?presentingToken.exchange(nullptr):nullptr;
    beforeGamePresent(t);
    auto hr=originalPresent(swap,interval,flags);
    afterGamePresent(t,hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE presentPrimary1(IDXGISwapChain1* swap,UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* params) {
    auto* t=(swap==gameChain && !(flags&DXGI_PRESENT_TEST))?presentingToken.exchange(nullptr):nullptr;
    beforeGamePresent(t);
    auto hr=originalPresent1(swap,interval,flags,params);
    afterGamePresent(t,hr);
    return hr;
}
void observePrimary(IDXGISwapChain* swap) {
    if(!swap) return;
    ComPtr<IDXGISwapChain1> chain1;
    if(FAILED(swap->QueryInterface(IID_PPV_ARGS(&chain1)))) return;
    auto entries=*reinterpret_cast<void***>(chain1.Get());
    // Observe only the Streamline proxy returned to Unity, leaving DXGI Present untouched.
    if(!originalPresent) {
        if(MH_CreateHook(entries[8],reinterpret_cast<void*>(&presentPrimary),reinterpret_cast<void**>(&originalPresent))!=MH_OK) return;
        if(MH_EnableHook(entries[8])!=MH_OK) return;
    }
    if(!originalPresent1) {
        if(MH_CreateHook(entries[22],reinterpret_cast<void*>(&presentPrimary1),reinterpret_cast<void**>(&originalPresent1))!=MH_OK) return;
        if(MH_EnableHook(entries[22])!=MH_OK) return;
    }
    trace("Unity received the Streamline swap chain");
    gameChain=swap;
    attached=true;
}
struct GameChainScope {
    GameChainScope(){handlingGameChain=true;}
    ~GameChainScope(){handlingGameChain=false;}
};
HRESULT STDMETHODCALLTYPE createGameWindowChain(IDXGIFactory2* factory,IUnknown* queue,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,IDXGISwapChain1** result) {
    if (!handlingGameChain) {
        try {
            GameChainScope guard;
            ComPtr<ID3D12CommandQueue> amdQueue;
            auto proxy = gameFactory(factory,queue,hwnd,amdQueue);
            if(amdQueue && desc && result && !output) {
                auto settings=*desc;
                auto fs=fullscreen?*fullscreen:DXGI_SWAP_CHAIN_FULLSCREEN_DESC{};
                fsr.create(libraryPath,factory,amdQueue.Get(),hwnd,&settings,fullscreen?&fs:nullptr,nullptr);
                return fsr.get(IID_PPV_ARGS(result));
            }
            if (proxy) {
                HRESULT hr = proxy->CreateSwapChainForHwnd(queue,hwnd,desc,fullscreen,output,result);
                if (SUCCEEDED(hr)) { observePrimary(*result); return hr; }
            }
        } catch(const std::exception& e) { trace(e.what()); report(e.what()); }
    }
    return originalWindow(factory,queue,hwnd,desc,fullscreen,output,result);
}
HRESULT STDMETHODCALLTYPE createGameLegacyChain(IDXGIFactory* factory,IUnknown* queue,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** result) {
    if (!handlingGameChain && desc) {
        try {
            GameChainScope guard;
            ComPtr<ID3D12CommandQueue> amdQueue;
            auto proxy = gameFactory(factory,queue,desc->OutputWindow,amdQueue);
            if(amdQueue && result) {
                auto settings=*desc;
                fsr.create(libraryPath,factory,amdQueue.Get(),settings.OutputWindow,nullptr,nullptr,&settings);
                return fsr.get(IID_PPV_ARGS(result));
            }
            if (proxy) {
                HRESULT hr = proxy->CreateSwapChain(queue,desc,result);
                if (SUCCEEDED(hr)) { observePrimary(*result);return hr; }
            }
        } catch(const std::exception& e) { trace(e.what()); report(e.what()); }
    }
    return originalLegacy(factory,queue,desc,result);
}
HRESULT createSystemFactory(REFIID iid,void** result) {
    // Streamline's import library also exports CreateDXGIFactory1. Adapter
    // discovery must work before loading Streamline and on PCs without it.
    static HMODULE dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto create=dxgi?reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(GetProcAddress(dxgi,"CreateDXGIFactory1")):nullptr;
    return create?create(iid,result):E_NOINTERFACE;
}
int installGamePresentation() {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(createSystemFactory(IID_PPV_ARGS(&factory)))) return 1;
    // The entries are the ABI slots of IDXGIFactory::CreateSwapChain and
    // IDXGIFactory2::CreateSwapChainForHwnd in the Windows SDK.
    auto methods = *reinterpret_cast<void***>(factory.Get());
    const auto initialized=MH_Initialize();
    if (initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return 2;
    if (MH_CreateHook(methods[10],reinterpret_cast<void*>(&createGameLegacyChain),reinterpret_cast<void**>(&originalLegacy))!=MH_OK) return 3;
    if (MH_CreateHook(methods[15],reinterpret_cast<void*>(&createGameWindowChain),reinterpret_cast<void**>(&originalWindow))!=MH_OK) {
        MH_RemoveHook(methods[10]); return 4;
    }
    if (MH_EnableHook(methods[10])==MH_OK && MH_EnableHook(methods[15])==MH_OK) return 0;
    MH_RemoveHook(methods[10]);MH_RemoveHook(methods[15]);return 5;
}

