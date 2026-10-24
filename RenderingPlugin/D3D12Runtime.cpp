#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include "UAL/UnityLog.h"

using Microsoft::WRL::ComPtr;

namespace
{
// Must match the D3D12Core.dll shipped beside this plugin.
constexpr UINT kShippedSdkVersion = 619;

std::wstring ModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module))
        return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return {};
    std::wstring directory(path, length);
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : directory.substr(0, slash + 1);
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

bool Apply(decltype(&D3D12GetInterface) getInterface, const std::wstring& directory)
{
    if (GetFileAttributesW((directory + L"D3D12Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    ComPtr<ID3D12SDKConfiguration1> configuration;
    HRESULT hr = getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration));
    if (FAILED(hr))
    {
        UnityLog::LogWarning("[IllusionRHI] D3D12 runtime selection is not supported by this system (hr=0x%08lX).\n", static_cast<unsigned long>(hr));
        return false;
    }
    const std::string path = Narrow(directory);
    ComPtr<ID3D12DeviceFactory> factory;
    hr = configuration->CreateDeviceFactory(kShippedSdkVersion, path.c_str(), IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->ApplyToGlobalState();
    if (FAILED(hr))
    {
        configuration->FreeUnusedSDKs();
        UnityLog::LogWarning("[IllusionRHI] Could not select D3D12 runtime %u from '%s' (hr=0x%08lX).\n",
            kShippedSdkVersion, path.c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    UnityLog::Log("[IllusionRHI] Selected D3D12 runtime %u from '%s'.\n", kShippedSdkVersion, path.c_str());
    return true;
}
}

// Runs from the plugin's load, which Unity performs before it creates the graphics device.
void SelectShippedD3D12Runtime()
{
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;

    if (GetModuleHandleW(L"D3D12Core.dll") != nullptr)
    {
        UnityLog::LogWarning("[IllusionRHI] A D3D12 runtime was loaded before this plugin; the shipped runtime %u is not used. Restart the Editor.\n",
            kShippedSdkVersion);
        return;
    }
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto getInterface = d3d12 ? reinterpret_cast<decltype(&D3D12GetInterface)>(GetProcAddress(d3d12, "D3D12GetInterface")) : nullptr;
    if (!getInterface)
    {
        UnityLog::LogWarning("[IllusionRHI] This system's D3D12 cannot select a runtime; the shipped runtime %u is not used.\n", kShippedSdkVersion);
        return;
    }
    // Players flatten native plugins, so the runtime may sit beside this plugin instead of in its D3D12 folder.
    const std::wstring directory = ModuleDirectory();
    if (directory.empty() || Apply(getInterface, directory + L"D3D12\\") || Apply(getInterface, directory))
        return;
    UnityLog::LogWarning("[IllusionRHI] The shipped D3D12 runtime %u was not found beside '%s'.\n", kShippedSdkVersion, Narrow(directory).c_str());
}
