#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <IUnityInterface.h>
#include <IUnityGraphics.h>
#include <IUnityGraphicsD3D12.h>
#include "UAL/UnityLog.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
extern IUnityInterfaces* s_UnityInterfaces;
extern DxcCreateInstanceProc GetShaderCompiler();
extern "C" int UnityRhiInitializeShaderExecutionReordering(IUnknown*);

namespace
{
struct Pipeline
{
    ComPtr<ID3D12StateObject> state;
    ComPtr<ID3D12Resource> table;
    UINT hitCount = 0;
    HRESULT status = S_FALSE;
};
struct Material
{
    std::vector<char> code;
    std::wstring closest, hitGroup;
};
struct Program
{
    std::vector<char> code;
    std::wstring entry;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::vector<char>> libraries;
    std::map<ID3D12RootSignature*, Pipeline> pipelines;
    UINT threadsX, threadsY;
    UINT64 fence = 0, dispatches = 0;
    HRESULT status = S_FALSE;
};
struct Compute
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12ShaderReflection> reflection;
};
struct Recording
{
    ID3D12PipelineState* state = nullptr;
    std::shared_ptr<Program> program;
};
std::mutex gate;
ComPtr<ID3D12Device5> device;
IUnityGraphicsD3D12v7* api = nullptr;
DxcCreateInstanceProc createDxc = nullptr;
int eventId = 0;
uintptr_t nextHandle = 1;
std::map<uintptr_t, std::shared_ptr<Program>> programs;
std::vector<std::shared_ptr<Program>> retired;
std::map<std::wstring, std::weak_ptr<Material>> materialCache;
std::map<ID3D12PipelineState*, Compute> computes;
std::map<ID3D12GraphicsCommandList*, Recording> recordings;
std::vector<void*> hooks;
using CreateStream = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using SetPipeline = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using Dispatch = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
CreateStream originalStream;
std::array<SetPipeline, 8> originalSets{};
std::array<Dispatch, 8> originalDispatches{};

bool Install(void* target, void* replacement, void** original)
{
    for (void* hook : hooks) if (hook == target) return true;
    if (*original) return false;
    if (MH_CreateHook(target, replacement, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); return false; }
    hooks.push_back(target);
    return true;
}

void Capture(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, void* result)
{
    {
        std::lock_guard lock(gate);
        if (!createDxc) createDxc = GetShaderCompiler();
        if (!createDxc) return;
        computes.erase(static_cast<ID3D12PipelineState*>(result));
    }
    ComPtr<IDxcLibrary> library;
    ComPtr<IDxcBlobEncoding> blob;
    ComPtr<IDxcContainerReflection> container;
    ComPtr<ID3D12ShaderReflection> reflection;
    UINT part;
    if (!desc.pRootSignature || !desc.CS.pShaderBytecode || desc.CS.BytecodeLength > UINT_MAX ||
        FAILED(createDxc(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
        FAILED(library->CreateBlobWithEncodingFromPinned(const_cast<void*>(desc.CS.pShaderBytecode), UINT(desc.CS.BytecodeLength), 0, &blob)) ||
        FAILED(createDxc(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container))) ||
        FAILED(container->Load(blob.Get())) || FAILED(container->FindFirstPartKind(0x4c495844, &part)) ||
        FAILED(container->GetPartReflection(part, IID_PPV_ARGS(&reflection)))) return;
    D3D12_SHADER_DESC details{};
    reflection->GetDesc(&details);
    for (UINT i = 0; i < details.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC binding{};
        reflection->GetResourceBindingDesc(i, &binding);
        if ((binding.Space != 1 && binding.Space != 2) || binding.BindPoint != 63 || binding.BindCount != 65536) continue;
        std::lock_guard lock(gate);
        computes[static_cast<ID3D12PipelineState*>(result)] = {desc.pRootSignature, reflection};
        break;
    }
}

HRESULT STDMETHODCALLTYPE CreateFromStream(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** result)
{
    HRESULT hr = originalStream(d, desc, iid, result);
    if (FAILED(hr) || !result || !*result) return hr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    const char* bytes = static_cast<const char*>(desc->pPipelineStateSubobjectStream);
    size_t offset = 0;
    while (offset + sizeof(UINT) <= desc->SizeInBytes)
    {
        auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(bytes + offset);
        size_t size, alignment = 8;
        switch (type)
        {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: size = sizeof(ID3D12RootSignature*); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: size = sizeof(D3D12_SHADER_BYTECODE); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: size = sizeof(D3D12_CACHED_PIPELINE_STATE); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: size = 4; alignment = 4; break;
        default: return hr;
        }
        size_t start = (offset + 4 + alignment - 1) & ~(alignment - 1);
        if (start + size > desc->SizeInBytes) return hr;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) memcpy(&compute.pRootSignature, bytes + start, size);
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) memcpy(&compute.CS, bytes + start, size);
        offset = (start + size + 7) & ~size_t(7);
    }
    Capture(compute, *result);
    return hr;
}

HRESULT ValidateBindings(Program& program, ID3D12ShaderReflection* carrier)
{
    ComPtr<IDxcUtils> utils;
    ComPtr<ID3D12LibraryReflection> reflection;
    HRESULT hr = createDxc(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hr)) return hr;
    std::vector<const std::vector<char>*> libraries{&program.code};
    for (const auto& material : program.materials)
        libraries.push_back(&material->code);
    for (const auto& library : program.libraries)
        libraries.push_back(&library);
    for (auto bytes : libraries)
    {
        DxcBuffer code{bytes->data(), bytes->size(), 0};
        hr = utils->CreateReflection(&code, IID_PPV_ARGS(&reflection));
        if (FAILED(hr)) return hr;
        D3D12_LIBRARY_DESC libraryDesc{};
        if (FAILED(reflection->GetDesc(&libraryDesc))) return E_INVALIDARG;
        for (UINT functionIndex = 0; functionIndex < libraryDesc.FunctionCount; ++functionIndex)
        {
            auto function = reflection->GetFunctionByIndex(functionIndex);
            if (!function) return E_INVALIDARG;
            D3D12_FUNCTION_DESC desc{};
            hr = function->GetDesc(&desc);
            if (FAILED(hr)) return hr;
            D3D12_SHADER_DESC carrierDesc{};
            hr = carrier->GetDesc(&carrierDesc);
            if (FAILED(hr)) return hr;
            for (UINT i = 0; i < desc.BoundResources; ++i)
            {
                D3D12_SHADER_INPUT_BIND_DESC expected{}, actual{};
                hr = function->GetResourceBindingDesc(i, &expected);
                if (FAILED(hr)) return hr;
                bool extension = strcmp(expected.Name, "g_NvidiaExt") == 0;
                bool found = false;
                for (UINT j = 0; j < carrierDesc.BoundResources; ++j)
                {
                    if (FAILED(carrier->GetResourceBindingDesc(j, &actual))) continue;
                    if (expected.BindPoint == actual.BindPoint && expected.Space == actual.Space &&
                        (extension ? D3D_SIT_UAV_RWSTRUCTURED : expected.Type) == actual.Type)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found || expected.BindCount != actual.BindCount || expected.Dimension != actual.Dimension ||
                    expected.ReturnType != actual.ReturnType)
                {
                    UnityLog::LogError("Inline RayGen binding mismatch: %s.\n", expected.Name);
                    return E_INVALIDARG;
                }
                if (expected.Type == D3D_SIT_CBUFFER)
                {
                    D3D12_SHADER_BUFFER_DESC a{}, b{};
                    if (FAILED(function->GetConstantBufferByName(expected.Name)->GetDesc(&a)) ||
                        FAILED(carrier->GetConstantBufferByIndex(actual.uID)->GetDesc(&b)) || a.Size != b.Size)
                    {
                        UnityLog::LogError("Inline RayGen constant buffer size mismatch: %s.\n", expected.Name);
                        return E_INVALIDARG;
                    }
                }
            }
        }
    }
    return S_OK;
}

HRESULT Build(Program& program, const Compute& compute, Pipeline& pipeline)
{
    HRESULT hr = ValidateBindings(program, compute.reflection.Get());
    if (FAILED(hr)) return hr;
    ID3D12RootSignature* root = compute.root.Get();
    std::vector<D3D12_DXIL_LIBRARY_DESC> libraries(1);
    libraries.reserve(1 + program.materials.size() + program.libraries.size());
    libraries[0].DXILLibrary = {program.code.data(), program.code.size()};
    D3D12_EXPORT_DESC raygenExport{program.entry.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE};
    libraries[0].NumExports = 1;
    libraries[0].pExports = &raygenExport;
    std::vector<D3D12_EXPORT_DESC> exports;
    exports.reserve(program.materials.size());
    std::vector<const Material*> included;
    std::vector<D3D12_HIT_GROUP_DESC> groups;
    groups.reserve(program.materials.size());
    for (size_t i = 0; i < program.materials.size(); ++i)
    {
        const auto& material = program.materials[i];
        if (std::find(included.begin(), included.end(), material.get()) != included.end()) continue;
        included.push_back(material.get());
        D3D12_DXIL_LIBRARY_DESC library{};
        library.DXILLibrary = {material->code.data(), material->code.size()};
        exports.push_back({material->closest.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE});
        library.NumExports = 1;
        library.pExports = &exports.back();
        libraries.push_back(library);
        D3D12_HIT_GROUP_DESC hit{};
        hit.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hit.HitGroupExport = material->hitGroup.c_str();
        hit.ClosestHitShaderImport = material->closest.c_str();
        groups.push_back(hit);
    }
    for (const auto& code : program.libraries)
    {
        D3D12_DXIL_LIBRARY_DESC library{};
        library.DXILLibrary = {code.data(), code.size()};
        libraries.push_back(library);
    }
    D3D12_RAYTRACING_SHADER_CONFIG shader{108, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG config{1};
    std::vector<D3D12_STATE_SUBOBJECT> objects;
    for (auto& library : libraries)
        objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library});
    for (auto& group : groups)
        objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group});
    objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root});
    objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader});
    objects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &config});
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(objects.size()), objects.data()};
    ComPtr<ID3D12InfoQueue> diagnostics;
    device.As(&diagnostics);
    UINT64 messageStart = diagnostics ? diagnostics->GetNumStoredMessages() : 0;
    auto started = GetTickCount64();
    hr = device->CreateStateObject(&desc, IID_PPV_ARGS(&pipeline.state));
    if (FAILED(hr))
    {
        if (diagnostics)
            for (UINT64 i = messageStart; i < diagnostics->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                diagnostics->GetMessage(i, nullptr, &size);
                std::vector<char> bytes(size);
                auto message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                if (SUCCEEDED(diagnostics->GetMessage(i, message, &size)))
                    UnityLog::LogError("Inline pipeline: %s\n", message->pDescription);
            }
        return hr;
    }
    ComPtr<ID3D12StateObjectProperties> properties;
    hr = pipeline.state.As(&properties);
    if (FAILED(hr)) return hr;
    const void* identifier = properties->GetShaderIdentifier(program.entry.c_str());
    if (!identifier) return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    pipeline.hitCount = UINT(program.materials.size());
    buffer.Width = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT +
                   ((UINT64(pipeline.hitCount) * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + 63) & ~UINT64(63));
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         nullptr, IID_PPV_ARGS(&pipeline.table));
    if (FAILED(hr)) return hr;
    void* mapped;
    hr = pipeline.table->Map(0, nullptr, &mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    auto records = static_cast<char*>(mapped) + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    for (size_t i = 0; i < pipeline.hitCount; ++i)
    {
        const void* hit = properties->GetShaderIdentifier(program.materials[i]->hitGroup.c_str());
        if (!hit)
        {
            pipeline.table->Unmap(0, nullptr);
            return E_INVALIDARG;
        }
        memcpy(records + i * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, hit, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    pipeline.table->Unmap(0, nullptr);
    UnityLog::Log("Inline pipeline '%ls': %zu material libraries, %u cached SBT records, %llu ms.\n",
                  program.entry.c_str(), included.size(), pipeline.hitCount, GetTickCount64() - started);
    return S_OK;
}

template<size_t Index>
void STDMETHODCALLTYPE Set(ID3D12GraphicsCommandList* list, ID3D12PipelineState* state)
{
    originalSets[Index](list, state);
    std::lock_guard lock(gate);
    recordings[list].state = state;
}

template<size_t Index>
void STDMETHODCALLTYPE DispatchList(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z)
{
    std::unique_lock lock(gate);
    auto& recording = recordings[list];
    auto program = std::move(recording.program);
    if (!program)
    {
        lock.unlock();
        originalDispatches[Index](list, x, y, z);
        return;
    }
    if (FAILED(program->status)) return;
    auto compute = computes.find(recording.state);
    if (compute != computes.end())
    {
        auto& pipeline = program->pipelines[compute->second.root.Get()];
        if (pipeline.status == S_FALSE)
        {
            pipeline.status = Build(*program, compute->second, pipeline);
            if (FAILED(pipeline.status))
                UnityLog::LogError("Inline RayGen '%ls' pipeline failed (0x%08X). Dispatch stopped.\n", program->entry.c_str(), pipeline.status);
        }
        program->status = pipeline.status;
        ComPtr<ID3D12GraphicsCommandList4> rayList;
        if (SUCCEEDED(pipeline.status)) program->status = list->QueryInterface(IID_PPV_ARGS(&rayList));
        if (SUCCEEDED(program->status))
        {
            D3D12_DISPATCH_RAYS_DESC rays{};
            rays.RayGenerationShaderRecord = {pipeline.table->GetGPUVirtualAddress(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
            if (pipeline.hitCount)
                rays.HitGroupTable = {pipeline.table->GetGPUVirtualAddress() + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                    UINT64(pipeline.hitCount) * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
            rays.Width = x * program->threadsX;
            rays.Height = y * program->threadsY;
            rays.Depth = z;
            rayList->SetPipelineState1(pipeline.state.Get());
            rayList->DispatchRays(&rays);
            auto state = recording.state;
            program->fence = api->GetNextFrameFenceValue();
            ++program->dispatches;
            lock.unlock();
            list->SetPipelineState(state);
            return;
        }
    }
    else
    {
        program->status = E_UNEXPECTED;
        UnityLog::LogError("Inline RayGen '%ls' binding carrier pipeline was not captured.\n", program->entry.c_str());
    }
    UnityLog::LogError("Inline RayGen '%ls' dispatch failed (0x%08X).\n", program->entry.c_str(), program->status);
}

template<size_t Index = 0>
bool InstallCommand(void* target, bool dispatch)
{
    for (void* hook : hooks) if (hook == target) return true;
    if constexpr (Index == originalSets.size()) return false;
    else
    {
        void** original = dispatch ? reinterpret_cast<void**>(&originalDispatches[Index]) : reinterpret_cast<void**>(&originalSets[Index]);
        if (*original) return InstallCommand<Index + 1>(target, dispatch);
        void* replacement = dispatch ? reinterpret_cast<void*>(&DispatchList<Index>) : reinterpret_cast<void*>(&Set<Index>);
        return Install(target, replacement, original);
    }
}

void UNITY_INTERFACE_API Begin(int, void* data)
{
    std::lock_guard lock(gate);
    UINT64 completed = api->GetFrameFence()->GetCompletedValue();
    std::erase_if(retired, [completed](const auto& p) { return p->fence <= completed && p.use_count() == 1; });
    UnityGraphicsD3D12RecordingState state{};
    auto program = programs.find(reinterpret_cast<uintptr_t>(data));
    if (program == programs.end()) return;
    if (!api->CommandRecordingState(&state) || !state.commandList)
    {
        program->second->status = E_UNEXPECTED;
        UnityLog::LogError("Inline RayGen could not obtain Unity command recording state.\n");
        return;
    }
    recordings[state.commandList].program = program->second;
    void** methods = *reinterpret_cast<void***>(state.commandList);
    if (!InstallCommand(methods[25], false) ||
        !InstallCommand(methods[14], true))
    {
        program->second->status = E_FAIL;
        UnityLog::LogError("Inline RayGen command list hook installation failed.\n");
        return;
    }
}
}

extern "C" __declspec(dllexport) int UnityRhiInitializeInlineRayTracing()
{
    std::lock_guard lock(gate);
    if (device) return eventId;
    api = s_UnityInterfaces ? s_UnityInterfaces->Get<IUnityGraphicsD3D12v7>() : nullptr;
    if (!api || !api->GetDevice() || FAILED(api->GetDevice()->QueryInterface(IID_PPV_ARGS(&device))))
    {
        UnityLog::LogError("Inline RayGen requires Unity D3D12 and ID3D12Device5.\n");
        return 0;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        UnityLog::LogError("Inline RayGen hook initialization failed: %d.\n", status);
        device.Reset();
        return 0;
    }
    void** methods = *reinterpret_cast<void***>(device.Get());
    if (!Install(methods[47], reinterpret_cast<void*>(&CreateFromStream), reinterpret_cast<void**>(&originalStream)))
    {
        UnityLog::LogError("Inline RayGen pipeline capture hook installation failed.\n");
        device.Reset();
        return 0;
    }
    eventId = s_UnityInterfaces->Get<IUnityGraphics>()->ReserveEventIDRange(1);
    UnityD3D12PluginEventConfig config{};
    config.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
    config.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
    api->ConfigureEvent(eventId, &config);
    return eventId;
}

extern "C" __declspec(dllexport) void* UnityRhiGetInlineRayTracingCallback() { return reinterpret_cast<void*>(&Begin); }

void ResetInlineRayTracing(ID3D12GraphicsCommandList* list, ID3D12PipelineState* state)
{
    std::lock_guard lock(gate);
    recordings[list] = {state, nullptr};
}

void CaptureInlineRayTracing(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, void* result)
{
    if (device) Capture(desc, result);
}

extern "C" __declspec(dllexport) uintptr_t UnityRhiCreateInlineRayTracing(const void* code, int size, const wchar_t* entry, UINT x, UINT y)
{
    std::lock_guard lock(gate);
    if (!device || !code || size <= 0 || !entry || !x || !y) return 0;
    if (!createDxc) createDxc = GetShaderCompiler();
    if (!createDxc)
    {
        UnityLog::LogError("Inline RayGen DXC reflection is unavailable.\n");
        return 0;
    }
    if (!UnityRhiInitializeShaderExecutionReordering(device.Get()))
    {
        UnityLog::LogError("Inline RayGen SER initialization failed.\n");
        return 0;
    }
    auto program = std::make_shared<Program>();
    program->code.assign(static_cast<const char*>(code), static_cast<const char*>(code) + size);
    program->entry = entry;
    program->threadsX = x;
    program->threadsY = y;
    uintptr_t handle = nextHandle++;
    programs.emplace(handle, std::move(program));
    return handle;
}

extern "C" __declspec(dllexport) HRESULT UnityRhiAddInlineRayTracingMaterial(uintptr_t handle,
    const void* code, int size, const wchar_t* closest)
{
    std::lock_guard lock(gate);
    auto found = programs.find(handle);
    if (found == programs.end() || !code || size <= 0 || !closest) return E_INVALIDARG;
    auto& program = *found->second;
    if (!program.pipelines.empty()) return E_UNEXPECTED;
    std::shared_ptr<Material> material = materialCache[closest].lock();
    if (!material || material->code.size() != size || memcmp(material->code.data(), code, size) != 0)
    {
        material = std::make_shared<Material>();
        material->code.assign(static_cast<const char*>(code), static_cast<const char*>(code) + size);
        material->closest = closest;
        material->hitGroup = std::wstring(closest) + L"Group";
        materialCache[closest] = material;
    }
    program.materials.push_back(std::move(material));
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT UnityRhiAddInlineRayTracingLibrary(uintptr_t handle, const void* code, int size)
{
    std::lock_guard lock(gate);
    auto found = programs.find(handle);
    if (found == programs.end() || !code || size <= 0) return E_INVALIDARG;
    auto& program = *found->second;
    if (!program.pipelines.empty()) return E_UNEXPECTED;
    program.libraries.emplace_back(static_cast<const char*>(code), static_cast<const char*>(code) + size);
    return S_OK;
}

extern "C" __declspec(dllexport) void UnityRhiReleaseInlineRayTracing(uintptr_t handle)
{
    std::lock_guard lock(gate);
    auto program = programs.find(handle);
    if (program == programs.end()) return;
    retired.push_back(std::move(program->second));
    programs.erase(program);
}

extern "C" __declspec(dllexport) UINT64 UnityRhiGetInlineRayTracingDispatches(uintptr_t handle, HRESULT* status)
{
    std::lock_guard lock(gate);
    auto program = programs.find(handle);
    if (program == programs.end()) { *status = E_HANDLE; return 0; }
    *status = program->second->status;
    return program->second->dispatches;
}

void ShutdownInlineRayTracing()
{
    std::lock_guard lock(gate);
    for (void* target : hooks) { MH_DisableHook(target); MH_RemoveHook(target); }
    hooks.clear();
    recordings.clear();
    programs.clear();
    materialCache.clear();
    retired.clear();
    computes.clear();
    device.Reset();
    createDxc = nullptr;
    api = nullptr;
    eventId = 0;
    originalStream = nullptr;
    originalSets.fill(nullptr);
    originalDispatches.fill(nullptr);
}
