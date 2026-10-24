// C vtable layouts give the method slots without hard-coded indices.
#include <stddef.h>
#include <d3d12.h>

const int IllusionRhiPrebuildInfoSlot = offsetof(ID3D12Device5Vtbl, GetRaytracingAccelerationStructurePrebuildInfo) / sizeof(void*);
const int IllusionRhiBuildAccelerationStructureSlot = offsetof(ID3D12GraphicsCommandList4Vtbl, BuildRaytracingAccelerationStructure) / sizeof(void*);
const int IllusionRhiCopyAccelerationStructureSlot = offsetof(ID3D12GraphicsCommandList4Vtbl, CopyRaytracingAccelerationStructure) / sizeof(void*);
