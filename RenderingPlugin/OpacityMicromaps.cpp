#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <IUnityInterface.h>
#include <IUnityGraphics.h>
#include <IUnityGraphicsD3D12.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "UAL/UnityLog.h"

using Microsoft::WRL::ComPtr;

extern IUnityInterfaces* s_UnityInterfaces;

namespace
{
struct MicromapArray
{
    ComPtr<ID3D12Resource> data;
    ComPtr<ID3D12Resource> descs;
    ComPtr<ID3D12Resource> result;
    ComPtr<ID3D12Resource> scratch;
    D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY histogram{};
    bool built = false;
    UINT64 buildFence = 0;
    UINT64 retiredFence = 0;
};

struct Linkage
{
    std::shared_ptr<MicromapArray> array;
    ComPtr<ID3D12Resource> indices;
};

struct Operation
{
    enum Kind { Build, Attach, Detach, Release } kind;
    std::shared_ptr<MicromapArray> array;
    UINT64 indexAddress = 0;
    UINT indexCount = 0;
    ComPtr<ID3D12Resource> indices;
};

std::mutex gate;
std::map<uintptr_t, std::shared_ptr<MicromapArray>> arrays;
std::deque<Operation> pending;
std::map<std::pair<UINT64, UINT>, Linkage> attached;
std::vector<std::shared_ptr<MicromapArray>> retired;
IUnityGraphicsD3D12v8* api = nullptr;
ComPtr<ID3D12Device5> device;
int eventId = 0;

bool EnsureDevice()
{
    if (device)
        return true;
    api = s_UnityInterfaces ? s_UnityInterfaces->Get<IUnityGraphicsD3D12v8>() : nullptr;
    return api && api->GetDevice() && SUCCEEDED(api->GetDevice()->QueryInterface(IID_PPV_ARGS(&device)));
}

ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size > 0 ? size : 1;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs(const MicromapArray& array, D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC& desc)
{
    desc = {};
    desc.NumOmmHistogramEntries = 1;
    desc.pOmmHistogram = &array.histogram;
    desc.InputBuffer = array.data ? array.data->GetGPUVirtualAddress() : 0;
    desc.PerOmmDescs = {array.descs ? array.descs->GetGPUVirtualAddress() : 0, sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.pOpacityMicromapArrayDesc = &desc;
    return inputs;
}

void Record(ID3D12GraphicsCommandList4* list, MicromapArray& array)
{
    api->RequestResourceState(array.data.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    api->RequestResourceState(array.descs.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC desc;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = Inputs(array, desc);
    build.DestAccelerationStructureData = array.result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = array.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = array.result.Get();
    list->ResourceBarrier(1, &barrier);
    array.built = true;
    array.buildFence = api->GetNextFrameFenceValue();
}

void UNITY_INTERFACE_API OnEvent(int)
{
    UnityGraphicsD3D12RecordingState state{};
    if (!api || !api->CommandRecordingState(&state))
        return;
    ComPtr<ID3D12GraphicsCommandList4> list;
    if (FAILED(state.commandList->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    std::lock_guard lock(gate);
    const UINT64 completed = api->GetFrameFence()->GetCompletedValue();
    std::erase_if(retired, [&](const std::shared_ptr<MicromapArray>& array) { return array->retiredFence <= completed; });
    for (auto& [handle, array] : arrays)
        if (array->built && array->data && array->buildFence <= completed)
        {
            array->data.Reset();
            array->descs.Reset();
            array->scratch.Reset();
        }
    while (!pending.empty())
    {
        Operation op = std::move(pending.front());
        pending.pop_front();
        switch (op.kind)
        {
        case Operation::Build:
            Record(list.Get(), *op.array);
            break;
        case Operation::Attach:
            if (op.array->built)
                attached[{op.indexAddress, op.indexCount}] = {op.array, op.indices};
            break;
        case Operation::Detach:
            attached.erase({op.indexAddress, op.indexCount});
            break;
        case Operation::Release:
            op.array->retiredFence = api->GetNextFrameFenceValue();
            retired.push_back(op.array);
            break;
        }
    }
}
}

bool FindOpacityMicromap(UINT64 indexAddress, UINT indexCount, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage)
{
    std::lock_guard lock(gate);
    auto it = attached.find({indexAddress, indexCount});
    if (it == attached.end())
        return false;
    linkage = {};
    linkage.OpacityMicromapIndexBuffer = {it->second.indices->GetGPUVirtualAddress(), sizeof(UINT)};
    linkage.OpacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
    linkage.OpacityMicromapBaseLocation = 0;
    linkage.OpacityMicromapArray = it->second.array->result->GetGPUVirtualAddress();
    return true;
}

bool OpacityMicromapsSupported()
{
    if (!EnsureDevice())
        return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) &&
           options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_2;
}

extern "C" __declspec(dllexport) int UnityRhiOpacityMicromapsSupported() { return OpacityMicromapsSupported() ? 1 : 0; }

extern "C" __declspec(dllexport) int UnityRhiGetOpacityMicromapEvent()
{
    std::lock_guard lock(gate);
    if (eventId == 0 && EnsureDevice())
    {
        eventId = s_UnityInterfaces->Get<IUnityGraphics>()->ReserveEventIDRange(1);
        UnityD3D12PluginEventConfig config{};
        config.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
        config.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
        api->ConfigureEvent(eventId, &config);
    }
    return eventId;
}

extern "C" __declspec(dllexport) void* UnityRhiGetOpacityMicromapCallback() { return reinterpret_cast<void*>(&OnEvent); }

// Allocates an array for count micromaps of one subdivision level and format and queues its build. The data and per-micromap
// descriptors are written on the GPU before the plugin event runs.
extern "C" __declspec(dllexport) uintptr_t UnityRhiCreateOpacityMicromapArray(IUnknown* data, IUnknown* descs, UINT count, UINT level, UINT format)
{
    if (!OpacityMicromapsSupported() || !data || !descs || count == 0)
        return 0;
    auto array = std::make_shared<MicromapArray>();
    if (FAILED(data->QueryInterface(IID_PPV_ARGS(&array->data))) || FAILED(descs->QueryInterface(IID_PPV_ARGS(&array->descs))))
        return 0;
    array->histogram.Count = count;
    array->histogram.SubdivisionLevel = level;
    array->histogram.Format = static_cast<D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT>(format);
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC desc;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = Inputs(*array, desc);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0)
        return 0;
    array->result = Buffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    array->scratch = Buffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!array->result || !array->scratch)
        return 0;
    std::lock_guard lock(gate);
    auto handle = reinterpret_cast<uintptr_t>(array.get());
    arrays[handle] = array;
    pending.push_back({Operation::Build, array});
    return handle;
}

extern "C" __declspec(dllexport) void UnityRhiReleaseOpacityMicromapArray(uintptr_t handle)
{
    std::lock_guard lock(gate);
    auto it = arrays.find(handle);
    if (it == arrays.end())
        return;
    pending.push_back({Operation::Release, it->second});
    arrays.erase(it);
}

// Links an array to the bottom-level geometry whose index range starts at indexAddress. Takes effect at the next plugin event.
extern "C" __declspec(dllexport) void UnityRhiAttachOpacityMicromap(UINT64 indexAddress, UINT indexCount, uintptr_t handle, IUnknown* indices)
{
    std::lock_guard lock(gate);
    auto it = arrays.find(handle);
    Operation op{Operation::Attach, it != arrays.end() ? it->second : nullptr, indexAddress, indexCount};
    if (!op.array || !indices || FAILED(indices->QueryInterface(IID_PPV_ARGS(&op.indices))))
        return;
    pending.push_back(std::move(op));
}

extern "C" __declspec(dllexport) void UnityRhiDetachOpacityMicromap(UINT64 indexAddress, UINT indexCount)
{
    std::lock_guard lock(gate);
    pending.push_back({Operation::Detach, nullptr, indexAddress, indexCount});
}
