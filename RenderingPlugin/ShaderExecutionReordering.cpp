#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <mutex>
#include <cstring>
#if UNITYRHI_WITH_NVAPI
#include <nvapi.h>
#include <nvShaderExtnEnums.h>
#endif

using Microsoft::WRL::ComPtr;
namespace
{
using CreateStateObject = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID, void**);
CreateStateObject originalCreate = nullptr;
void* hookTarget = nullptr;
DxcCreateInstanceProc createDxc = nullptr;
HMODULE dxcModule = nullptr;
std::mutex installMutex;

#if UNITYRHI_WITH_NVAPI
bool FindExtension(const D3D12_SHADER_BYTECODE& code, UINT& slot, UINT& space)
{
    ComPtr<IDxcLibrary> library;
    ComPtr<IDxcBlobEncoding> blob;
    ComPtr<IDxcContainerReflection> container;
    ComPtr<ID3D12LibraryReflection> reflection;
    UINT part = 0;
    if (code.BytecodeLength > UINT_MAX ||
        FAILED(createDxc(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
        FAILED(library->CreateBlobWithEncodingFromPinned(const_cast<void*>(code.pShaderBytecode), UINT32(code.BytecodeLength), 0, &blob)) ||
        FAILED(createDxc(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container))) ||
        FAILED(container->Load(blob.Get())) || FAILED(container->FindFirstPartKind(0x4c495844, &part)) ||
        FAILED(container->GetPartReflection(part, IID_PPV_ARGS(&reflection))))
        return false;

    D3D12_LIBRARY_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc))) return false;
    for (UINT i = 0; i < desc.FunctionCount; ++i)
    {
        auto* function = reflection->GetFunctionByIndex(i);
        D3D12_FUNCTION_DESC functionDesc{};
        if (FAILED(function->GetDesc(&functionDesc))) continue;
        for (UINT j = 0; j < functionDesc.BoundResources; ++j)
        {
            D3D12_SHADER_INPUT_BIND_DESC binding{};
            if (SUCCEEDED(function->GetResourceBindingDesc(j, &binding)) && binding.Name &&
                std::strcmp(binding.Name, "g_NvidiaExt") == 0)
            {
                slot = binding.BindPoint;
                space = binding.Space;
                return true;
            }
        }
    }
    return false;
}

HRESULT STDMETHODCALLTYPE CreateWithExtensions(ID3D12Device5* device, const D3D12_STATE_OBJECT_DESC* desc, REFIID iid, void** result)
{
    UINT slot = 0, space = 0;
    bool usesExtensions = false;
    if (desc && desc->pSubobjects)
        for (UINT i = 0; i < desc->NumSubobjects; ++i)
        {
            const auto& subobject = desc->pSubobjects[i];
            if (subobject.Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !subobject.pDesc) continue;
            UINT librarySlot = 0, librarySpace = 0;
            if (!FindExtension(static_cast<const D3D12_DXIL_LIBRARY_DESC*>(subobject.pDesc)->DXILLibrary, librarySlot, librarySpace)) continue;
            if (usesExtensions && (slot != librarySlot || space != librarySpace)) return E_INVALIDARG;
            slot = librarySlot;
            space = librarySpace;
            usesExtensions = true;
        }
    if (!usesExtensions) return originalCreate(device, desc, iid, result);
    if (NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(device, slot, space) != NVAPI_OK) return E_FAIL;
    HRESULT status = originalCreate(device, desc, iid, result);
    NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(device, ~0u, 0);
    return status;
}

bool LoadCompiler()
{
    if (createDxc) return true;
    HMODULE self = GetModuleHandleW(L"UnityRHI.dll");
    wchar_t path[MAX_PATH];
    if (!self) return false;
    DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    wchar_t* filename = wcsrchr(path, L'\\');
    if (!filename || wcscpy_s(filename + 1, MAX_PATH - (filename + 1 - path), L"dxcompiler.dll") != 0) return false;
    dxcModule = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    createDxc = dxcModule ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcModule, "DxcCreateInstance")) : nullptr;
    return createDxc != nullptr;
}
#endif
}

extern "C" __declspec(dllexport) int UnityRhiInitializeShaderExecutionReordering(IUnknown* nativeDevice)
{
#if UNITYRHI_WITH_NVAPI
    std::lock_guard<std::mutex> lock(installMutex);
    ComPtr<ID3D12Device5> device;
    if (!nativeDevice || FAILED(nativeDevice->QueryInterface(IID_PPV_ARGS(&device))) || NvAPI_Initialize() != NVAPI_OK) return 0;
    NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS caps{};
    bool supported = false;
    if (NvAPI_D3D12_GetRaytracingCaps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING, &caps, sizeof(caps)) != NVAPI_OK ||
        !(caps & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD) ||
        NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(device.Get(), NV_EXTN_OP_HIT_OBJECT_REORDER_THREAD, &supported) != NVAPI_OK || !supported)
        return 0;
    void* target = (*reinterpret_cast<void***>(device.Get()))[62];
    if (hookTarget) return hookTarget == target;
    if (!LoadCompiler()) return 0;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return 0;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&CreateWithExtensions), reinterpret_cast<void**>(&originalCreate)) != MH_OK) return 0;
    if (MH_EnableHook(target) != MH_OK)
    {
        MH_RemoveHook(target);
        return 0;
    }
    hookTarget = target;
    return 1;
#else
    return 0;
#endif
}

DxcCreateInstanceProc GetShaderCompiler()
{
#if UNITYRHI_WITH_NVAPI
    std::lock_guard<std::mutex> lock(installMutex);
    return LoadCompiler() ? createDxc : nullptr;
#else
    return nullptr;
#endif
}

void ShutdownShaderExecutionReordering()
{
    std::lock_guard<std::mutex> lock(installMutex);
    if (hookTarget)
    {
        MH_DisableHook(hookTarget);
        MH_RemoveHook(hookTarget);
        hookTarget = nullptr;
    }
    if (dxcModule) FreeLibrary(dxcModule);
    dxcModule = nullptr;
    createDxc = nullptr;
}
