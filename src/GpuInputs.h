#pragma once
#include "TemporalBytecode.h"
#include "HudBytecode.h"
#include "CockpitBytecode.h"

struct InputKernels {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> temporal,hud,cockpit;
    void initialize(ID3D12Device* dev) {
        D3D12_DESCRIPTOR_RANGE ranges[]={
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,9,0,0,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,9}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable={2,ranges};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants={0,0,26};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        D3D12_ROOT_SIGNATURE_DESC desc{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob,errors;
        check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"Serialize input root signature");
        check(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create input root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();
        pipeline.CS={TemporalBytecode,sizeof(TemporalBytecode)};
        check(dev->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&temporal)),"Create motion/depth pipeline");
        pipeline.CS={HudBytecode,sizeof(HudBytecode)};
        check(dev->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&hud)),"Create HUD pipeline");
        pipeline.CS={CockpitBytecode,sizeof(CockpitBytecode)};
        check(dev->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&cockpit)),"Create cockpit composite pipeline");
    }
    struct Binding {ID3D12Resource* texture; DXGI_FORMAT format;};
    void dispatch(Slot& slot,ID3D12PipelineState* pipeline,std::initializer_list<Binding> inputs,
        std::initializer_list<Binding> outputs, const void* constants,int words,int width,int height) {
        auto& views=pipeline==cockpit.Get()?slot.cockpitViews:slot.views;
        if(!views) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,13,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&views)),"Create input views");
        }
        UINT stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto start=views->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<9;i++) {
            Binding b=i<inputs.size()?inputs.begin()[i]:Binding{nullptr,DXGI_FORMAT_R32_FLOAT};
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=b.format;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(b.texture,&v,{start.ptr+i*stride});
        }
        for(UINT i=0;i<4;i++) {
            Binding b=i<outputs.size()?outputs.begin()[i]:Binding{nullptr,DXGI_FORMAT_R32_FLOAT};
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=b.format;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(b.texture,nullptr,&v,{start.ptr+(i+9)*stride});
        }
        auto* cmd=slot.list.Get();auto* heap=views.Get();cmd->SetDescriptorHeaps(1,&heap);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipeline);
        cmd->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        if(words)cmd->SetComputeRoot32BitConstants(1,words,constants,0);
        cmd->Dispatch((width+7)/8,(height+7)/8,1);
    }
};

