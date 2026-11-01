// My Summer Frames core: the AMD FSR API (FSR SDK 2.4, github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, MIT).
// Copyright (C) 2026 ANICKON
//
// The SDK's signed DLLs ship in the mod's assets folder (amd_fidelityfx_loader_dx12.dll and the effect DLLs next to
// it); the loader is loaded at run time and finds the effect DLLs in its own folder.
#pragma once
#include "common.h"

#pragma warning(push)
#pragma warning(disable : 4201 4996) // nameless unions; the SDK's deprecated structs
#include "api/include/ffx_api.h"
#include "api/include/dx12/ffx_api_dx12.h"
#include "upscalers/include/ffx_upscale.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#pragma warning(pop)

namespace msf
{
    struct FfxFns
    {
        PfnFfxCreateContext CreateContext = nullptr;
        PfnFfxDestroyContext DestroyContext = nullptr;
        PfnFfxConfigure Configure = nullptr;
        PfnFfxQuery Query = nullptr;
        PfnFfxDispatch Dispatch = nullptr;
    };

    // The version name of a context ("3.1.6"), or "" when the query fails.
    std::string FfxProviderVersion(ffxContext* context);

    // Messages of the FSR runtime into the log.
    void FfxMessage(uint32_t type, const wchar_t* message);
}
