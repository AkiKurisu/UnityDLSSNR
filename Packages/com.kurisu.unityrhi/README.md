# UnityRHI

Direct3D 12 runtime for Unity 6.3, including managed APIs, shader tools and native plugins.

Install the complete package produced by `3-Pack.bat`. Native plugins are located in `Plugins/x86_64`. The package ships the D3D12 Agility SDK runtime in `Plugins/x86_64/D3D12`; the preloaded `GfxPluginIllusionRHI` selects it before Unity creates the device.

DLSS Neural Rendering additionally requires `nvngx_dlssnr.dll`, obtained separately. See [third-party notices](ThirdPartyNotices.md).
