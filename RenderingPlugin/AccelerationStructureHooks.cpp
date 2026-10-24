#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "UAL/UnityLog.h"

using Microsoft::WRL::ComPtr;

extern "C" const int IllusionRhiPrebuildInfoSlot;
extern "C" const int IllusionRhiBuildAccelerationStructureSlot;
extern "C" const int IllusionRhiCopyAccelerationStructureSlot;
bool FindOpacityMicromap(UINT64 indexAddress, UINT indexCount, D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC& linkage);

namespace
{
using PrebuildInfo = void(STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*);
using BuildAccelerationStructure = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);

using CopyAccelerationStructure = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE);

PrebuildInfo originalPrebuild = nullptr;
BuildAccelerationStructure originalBuild = nullptr;
CopyAccelerationStructure originalCopy = nullptr;
std::mutex captureGate;
bool capturing = false;
std::string capture;

const D3D12_RAYTRACING_GEOMETRY_DESC& Geometry(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, UINT i)
{
    return inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? inputs.pGeometryDescs[i] : *inputs.ppGeometryDescs[i];
}

void Record(const char* kind, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, D3D12_GPU_VIRTUAL_ADDRESS dest,
    UINT64 size)
{
    if (inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
        return;
    std::lock_guard lock(captureGate);
    if (!capturing)
        return;
    char line[256];
    snprintf(line, sizeof(line), "%s dest=0x%llx size=%llu flags=0x%x geometries=%u\n", kind, dest, size, inputs.Flags, inputs.NumDescs);
    capture += line;
    for (UINT i = 0; i < inputs.NumDescs; i++)
    {
        const D3D12_RAYTRACING_GEOMETRY_DESC& g = Geometry(inputs, i);
        const bool omm = g.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
        if (g.Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES && !omm)
        {
            snprintf(line, sizeof(line), "  type=%d flags=0x%x\n", g.Type, g.Flags);
            capture += line;
            continue;
        }
        const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& t = omm ? *g.OmmTriangles.pTriangles : g.Triangles;
        snprintf(line, sizeof(line), "  %sflags=0x%x index=0x%llx count=%u format=%d vertex=0x%llx count=%u stride=%llu format=%d\n",
            omm ? "omm " : "", g.Flags, t.IndexBuffer, t.IndexCount, t.IndexFormat, t.VertexBuffer.StartAddress, t.VertexCount,
            t.VertexBuffer.StrideInBytes, t.VertexFormat);
        capture += line;
    }
}

// Unity's full bottom-level builds of non-opaque geometry pick up the opacity micromaps attached to their index ranges.
struct Micromapped
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    std::vector<D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC> triangles;
    std::vector<D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC> linkages;

    bool Attach(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& source)
    {
        if (source.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL ||
            (source.Flags & (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE)) != 0)
            return false;
        geometries.resize(source.NumDescs);
        triangles.resize(source.NumDescs);
        linkages.resize(source.NumDescs);
        bool any = false;
        for (UINT i = 0; i < source.NumDescs; i++)
        {
            geometries[i] = Geometry(source, i);
            const D3D12_RAYTRACING_GEOMETRY_DESC& g = geometries[i];
            if (g.Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES || (g.Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) != 0 ||
                !FindOpacityMicromap(g.Triangles.IndexBuffer, g.Triangles.IndexCount, linkages[i]))
                continue;
            triangles[i] = g.Triangles;
            geometries[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
            geometries[i].OmmTriangles = {&triangles[i], &linkages[i]};
            any = true;
        }
        inputs = source;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.pGeometryDescs = geometries.data();
        return any;
    }
};

void STDMETHODCALLTYPE Prebuild(ID3D12Device5* self, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inputs,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info)
{
    Micromapped micromapped;
    if (inputs && micromapped.Attach(*inputs))
        inputs = &micromapped.inputs;
    originalPrebuild(self, inputs, info);
    if (inputs && info)
        Record("prebuild", *inputs, 0, info->ResultDataMaxSizeInBytes);
}

void STDMETHODCALLTYPE Build(ID3D12GraphicsCommandList4* self, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc, UINT count,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* postbuild)
{
    Micromapped micromapped;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC rewritten;
    if (desc && micromapped.Attach(desc->Inputs))
    {
        rewritten = *desc;
        rewritten.Inputs = micromapped.inputs;
        desc = &rewritten;
    }
    if (desc)
        Record(desc->SourceAccelerationStructureData ? "update" : "build", desc->Inputs, desc->DestAccelerationStructureData, 0);
    originalBuild(self, desc, count, postbuild);
}

void STDMETHODCALLTYPE Copy(ID3D12GraphicsCommandList4* self, D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS source,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode)
{
    {
        std::lock_guard lock(captureGate);
        if (capturing)
        {
            char line[128];
            snprintf(line, sizeof(line), "copy dest=0x%llx source=0x%llx mode=%d\n", dest, source, mode);
            capture += line;
        }
    }
    originalCopy(self, dest, source, mode);
}

bool Hook(void* target, void* detour, void** original)
{
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        return false;
    status = MH_EnableHook(target);
    return status == MH_OK || status == MH_ERROR_ENABLED;
}
}

bool InstallAccelerationStructureHooks(ID3D12Device5* device)
{
    if (originalBuild)
        return true;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(list.As(&list4)))
        return false;
    list->Close();
    void** deviceMethods = *reinterpret_cast<void***>(device);
    void** listMethods = *reinterpret_cast<void***>(list4.Get());
    if (!Hook(deviceMethods[IllusionRhiPrebuildInfoSlot], reinterpret_cast<void*>(&Prebuild), reinterpret_cast<void**>(&originalPrebuild)) ||
        !Hook(listMethods[IllusionRhiBuildAccelerationStructureSlot], reinterpret_cast<void*>(&Build), reinterpret_cast<void**>(&originalBuild)) ||
        !Hook(listMethods[IllusionRhiCopyAccelerationStructureSlot], reinterpret_cast<void*>(&Copy), reinterpret_cast<void**>(&originalCopy)))
    {
        UnityLog::LogError("[IllusionRHI] Acceleration structure build hooks could not be installed.\n");
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) void UnityRhiCaptureAccelerationStructureBuilds(int enabled)
{
    std::lock_guard lock(captureGate);
    capturing = enabled != 0;
    capture.clear();
}

// Returns the captured text length; copies at most capacity - 1 characters and stops capturing.
extern "C" __declspec(dllexport) int UnityRhiReadAccelerationStructureBuilds(char* buffer, int capacity)
{
    std::lock_guard lock(captureGate);
    capturing = false;
    if (buffer && capacity > 0)
    {
        const size_t copied = capture.size() < static_cast<size_t>(capacity - 1) ? capture.size() : static_cast<size_t>(capacity - 1);
        memcpy(buffer, capture.data(), copied);
        buffer[copied] = '\0';
    }
    return static_cast<int>(capture.size());
}

extern "C" __declspec(dllexport) UINT64 UnityRhiGetGpuVirtualAddress(IUnknown* resource)
{
    ComPtr<ID3D12Resource> d3d12;
    return resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&d3d12))) ? d3d12->GetGPUVirtualAddress() : 0;
}
