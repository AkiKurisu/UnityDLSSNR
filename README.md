# UnityRHI for IllusionRP

UnityRHI fork with bindless resource access and Shader Execution Reordering for Unity 6.3 / Direct3D 12 on Windows x64.

## Build and install

Requires Visual Studio 2022 with C++ tools and CMake 3.24 or newer.

```bat
1-Deploy.bat
2-Build.bat Release
3-Pack.bat Release
```

Install `Build/com.kurisu.unityrhi` as a local UPM package. It contains the C# runtime, shaders and native DLLs.

For standalone URP integration, also install [DLSS for URP](Packages/top.kuanmi.dlss.urp/README.md).

`nvngx_dlssnr.dll` must be obtained separately for Neural Rendering and placed in the installed package's `Plugins/x86_64` directory.

Upstream: [Kuan-Mi/UnityDLSSNR](https://github.com/Kuan-Mi/UnityDLSSNR). See [third-party notices](Packages/com.kurisu.unityrhi/ThirdPartyNotices.md).
