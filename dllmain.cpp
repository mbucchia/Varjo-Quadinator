// MIT License
//
// Copyright(c) 2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <detours.h>
#include <traceloggingactivity.h>
#include <traceloggingprovider.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <vector>

#include <Varjo.h>
#include <Varjo_layers.h>
#include <Varjo_math.h>

/////////////////////////////////////////////////////////////////////////////
// Install this DLL into the Varjo OpenXR runtime:
//   setdll.exe /d:Quadinator.dll VarjoLib.dll

#define USE_FOVEATED_TANGENTS 1
#define USE_FOVEATED_GAZE 0

#pragma region "Tracelogging"

// {cbf3adcd-42b1-4c38-830b-95980af201f6}
TRACELOGGING_DEFINE_PROVIDER(g_traceProvider,
                             "Quadinator",
                             (0xcbf3adcd, 0x42b1, 0x4e38, 0x93, 0x0b, 0x95, 0x98, 0x0a, 0xf2, 0x01, 0xf6));

#define IsTraceEnabled() TraceLoggingProviderEnabled(g_traceProvider, 0, 0)
#define TraceLocalActivity(activity) TraceLoggingActivity<g_traceProvider> activity;
#define TLArg(var, ...) TraceLoggingValue(var, ##__VA_ARGS__)
#define TLPArg(var, ...) TraceLoggingPointer(var, ##__VA_ARGS__)
#pragma endregion

#pragma region "Detours"
template <typename TMethod>
void DetourDllAttach(HMODULE dll, const char* target, TMethod hooked, TMethod& original) {
    if (original) {
        // Already hooked.
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    original = (TMethod)GetProcAddress(dll, target);
    assert(original);
    DetourAttach((PVOID*)&original, hooked);
    DetourTransactionCommit();
}
#pragma endregion

template <uint32_t alignment>
inline constexpr uint32_t AlignTo(uint32_t n) {
    static_assert((alignment & (alignment - 1)) == 0); // must be power-of-two
    return (n + alignment - 1) & ~(alignment - 1);
}

/////////////////////////////////////////////////////////////////////////////
// Begin, Fun.

namespace {

    // clang-format off
    struct varjo_AlignedView (*original_GetAlignedView)(double* projectionMatrix) = nullptr;
    struct varjo_FovTangents (*original_GetFovTangents)(struct varjo_Session* session,
                                                        int32_t viewIndex) = nullptr;
    struct varjo_FovTangents (*original_GetFoveatedFovTangents)(struct varjo_Session* session,
                                                                int32_t indexView,
                                                                struct varjo_Gaze* gaze,
                                                                struct varjo_FoveatedFovTangents_Hints* hints) = nullptr;
    varjo_Bool (*original_GetRenderingGaze)(struct varjo_Session* session,
                                            struct varjo_Gaze* gaze) = nullptr;
    struct varjo_Matrix (*original_GetProjectionMatrix)(struct varjo_FovTangents* tangents) = nullptr;
    // clang-format on

    varjo_Bool GetRenderingGaze(struct varjo_Session* session, struct varjo_Gaze* gaze) {
#if USE_FOVEATED_GAZE == 1
        return original_GetRenderingGaze(session, gaze);
#else
        *gaze = {};
        gaze->leftEye.forward[2] = gaze->rightEye.forward[2] = gaze->gaze.forward[2] = 1.0;
        // gaze->leftPupilSize = gaze->rightPupilSize = 0.5;
        gaze->leftStatus = gaze->rightStatus = 3;
        gaze->stability = 1.0;
        gaze->status = 2;
        return true;
#endif
    }

    struct varjo_FovTangents GetFovTangents(struct varjo_Session* session, int32_t viewIndex) {
        varjo_Gaze gaze{};
        if (USE_FOVEATED_TANGENTS && GetRenderingGaze(session, &gaze)) {
            varjo_FoveatedFovTangents_Hints hints{};
            return original_GetFoveatedFovTangents(session, viewIndex, &gaze, &hints);
        } else {
            return original_GetFovTangents(session, viewIndex);
        }
    }

    void (*original_GetTextureSize)(struct varjo_Session* session,
                                    varjo_TextureSize_Type type,
                                    int32_t viewIndex,
                                    int32_t* width,
                                    int32_t* height) = nullptr;
    void hooked_GetTextureSize(struct varjo_Session* session,
                               varjo_TextureSize_Type type,
                               int32_t viewIndex,
                               int32_t* width,
                               int32_t* height) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local,
                               "varjo_GetTextureSize",
                               TLPArg(session, "Session"),
                               TLArg(type, "TextureSize_Type"),
                               TLArg(viewIndex, "ViewIndex"));

        if (type == varjo_TextureSize_Type_Stereo) {
            // Query the focus view resolution.
            original_GetTextureSize(session,
                                    USE_FOVEATED_TANGENTS ? varjo_TextureSize_Type_DynamicFoveation
                                                          : varjo_TextureSize_Type_Quad,
                                    2 + viewIndex,
                                    width,
                                    height);

            const auto fullFovTangents = GetFovTangents(session, viewIndex);
            const auto focusFovTangents = GetFovTangents(session, 2 + viewIndex);
            TraceLoggingWriteTagged(local,
                                    "varjo_GetTextureSize_FullFov",
                                    TLArg(viewIndex, "ViewIndex"),
                                    TLArg(atan(fullFovTangents.bottom), "Bottom"),
                                    TLArg(atan(fullFovTangents.top), "Top"),
                                    TLArg(atan(fullFovTangents.left), "Left"),
                                    TLArg(atan(fullFovTangents.right), "Right"));
            TraceLoggingWriteTagged(local,
                                    "varjo_GetTextureSize_FocusFov",
                                    TLArg(viewIndex, "ViewIndex"),
                                    TLArg(atan(focusFovTangents.bottom), "Bottom"),
                                    TLArg(atan(focusFovTangents.top), "Top"),
                                    TLArg(atan(focusFovTangents.left), "Left"),
                                    TLArg(atan(focusFovTangents.right), "Right"));

            // Transpose the resolution to the full FOV while keeping a uniform PPD.
            const double horizontalMultiplier = std::abs(fullFovTangents.right - fullFovTangents.left) /
                                                std::abs(focusFovTangents.right - focusFovTangents.left);
            const double verticalMultiplier = std::abs(fullFovTangents.top - fullFovTangents.bottom) /
                                              std::abs(focusFovTangents.top - focusFovTangents.bottom);
            TraceLoggingWriteTagged(local,
                                    "varjo_GetTextureSize_Multipliers",
                                    TLArg(viewIndex, "ViewIndex"),
                                    TLArg(horizontalMultiplier, "HorizontalMultiplier"),
                                    TLArg(verticalMultiplier, "VerticalMultiplier"));
            *width = AlignTo<2>(static_cast<int32_t>(*width * horizontalMultiplier));
            *height = AlignTo<2>(static_cast<int32_t>(*height * verticalMultiplier));
        } else {
            original_GetTextureSize(session, type, viewIndex, width, height);
        }

        TraceLoggingWriteStop(local, "varjo_GetTextureSize", TLArg(*width, "Width"), TLArg(*height, "Height"));
    }

    struct varjo_ViewDescription (*original_GetViewDescription)(struct varjo_Session* session,
                                                                int32_t viewIndex) = nullptr;
    struct varjo_ViewDescription hooked_GetViewDescription(struct varjo_Session* session, int32_t viewIndex) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(
            local, "varjo_GetViewDescription", TLPArg(session, "Session"), TLArg(viewIndex, "ViewIndex"));

        struct varjo_ViewDescription result = original_GetViewDescription(session, viewIndex);
        if (viewIndex == 0 || viewIndex == 1) {
            hooked_GetTextureSize(session, varjo_TextureSize_Type_Stereo, viewIndex, &result.width, &result.height);
        }

        TraceLoggingWriteStop(
            local, "varjo_GetViewDescription", TLArg(result.width, "Width"), TLArg(result.width, "Height"));

        return result;
    }

    void (*original_EndFrameWithLayers)(struct varjo_Session* session,
                                        struct varjo_SubmitInfoLayers* submitInfo) = nullptr;
    void hooked_EndFrameWithLayers(struct varjo_Session* session, struct varjo_SubmitInfoLayers* submitInfo) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local,
                               "varjo_EndFrameWithLayers",
                               TLPArg(session, "Session"),
                               TLArg(submitInfo->frameNumber, "FrameNumber"),
                               TLArg(submitInfo->layerCount, "LayerCount"));

        struct varjo_SubmitInfoLayers newSubmitInfo = *submitInfo;
        std::vector<varjo_LayerHeader*> newLayersPtr;

        std::vector<varjo_LayerMultiProj> projAllocator;
        projAllocator.reserve(submitInfo->layerCount);
        std::vector<std::array<varjo_LayerMultiProjView, 4>> viewsAllocator;
        viewsAllocator.reserve(submitInfo->layerCount);

        for (int32_t i = 0; i < submitInfo->layerCount; i++) {
            TraceLoggingWriteTagged(
                local, "varjo_EndFrameWithLayers_Layer", TLArg(submitInfo->layers[i]->type, "Type"));
            if (submitInfo->layers[i]->type == varjo_LayerMultiProjType) {
                const varjo_LayerMultiProj* proj = reinterpret_cast<const varjo_LayerMultiProj*>(submitInfo->layers[i]);
                TraceLoggingWriteTagged(local,
                                        "varjo_EndFrameWithLayers_MultiProj",
                                        TLArg(proj->header.flags, "Flags"),
                                        TLArg(proj->space, "Space"),
                                        TLArg(proj->viewCount, "ViewCount"));

                for (int32_t j = 0; j < proj->viewCount; j++) {
                    TraceLoggingWriteTagged(local,
                                            "varjo_EndFrameWithLayers_MultiProj",
                                            TLArg(j, "ViewIndex"),
                                            TLPArg(proj->views[j].viewport.swapChain, "SwapChain"),
                                            TLArg(proj->views[j].viewport.arrayIndex, "ArrayIndex"),
                                            TLArg(proj->views[j].viewport.x, "X"),
                                            TLArg(proj->views[j].viewport.y, "Y"),
                                            TLArg(proj->views[j].viewport.width, "Width"),
                                            TLArg(proj->views[j].viewport.height, "Height"));

                    const auto tangents = original_GetAlignedView(proj->views[j].projection.value);
                    TraceLoggingWriteTagged(local,
                                            "varjo_EndFrameWithLayers_MultiProj",
                                            TLArg(j, "ViewIndex"),
                                            TLArg(-atan(tangents.projectionBottom), "Bottom"),
                                            TLArg(atan(tangents.projectionTop), "Top"),
                                            TLArg(-atan(tangents.projectionLeft), "Left"),
                                            TLArg(atan(tangents.projectionRight), "Right"));
                }

                // Deep copy the projection and view.
                projAllocator.push_back(*proj);
                viewsAllocator.push_back({
                    proj->views[0],
                    proj->views[1],
                    proj->views[2 % proj->viewCount],
                    proj->views[3 % proj->viewCount],
                });
                projAllocator.back().views = viewsAllocator.back().data();
                newLayersPtr.push_back(reinterpret_cast<varjo_LayerHeader*>(&projAllocator.back()));

                // Patch the focus views.
                auto& views = viewsAllocator.back();
                for (int32_t k = 2; k < proj->viewCount; k++) {
                    auto& focusView = views[k];
                    const auto& referenceView = views[k % 2];

                    // This seems to be how Varjo SDK accepts stereo input.
                    if (focusView.viewport.width == 1 && focusView.viewport.height == 1) {
                        const auto fullFovTangents =
                            original_GetAlignedView(const_cast<double*>(referenceView.projection.value));
                        const auto focusFovTangents = GetFovTangents(session, k);

                        // Patch viewport to carve the focus view out of the full view.
                        focusView.viewport.swapChain = referenceView.viewport.swapChain;
                        focusView.viewport.arrayIndex = referenceView.viewport.arrayIndex;
                        const double horizontalFov = fullFovTangents.projectionRight + fullFovTangents.projectionLeft;
                        const double leftOffset =
                            (focusFovTangents.left + fullFovTangents.projectionLeft) / horizontalFov;
                        focusView.viewport.x += static_cast<int32_t>(leftOffset * referenceView.viewport.width);
                        focusView.viewport.width = AlignTo<2>(static_cast<int32_t>(
                            (std::abs(focusFovTangents.right - focusFovTangents.left) / horizontalFov) *
                            referenceView.viewport.width));
                        const double verticalFov = fullFovTangents.projectionTop + fullFovTangents.projectionBottom;
                        const double topOffset = (fullFovTangents.projectionTop - focusFovTangents.top) / verticalFov;
                        focusView.viewport.y += static_cast<int32_t>(topOffset * referenceView.viewport.height);
                        focusView.viewport.height = AlignTo<2>(static_cast<int32_t>(
                            (std::abs(focusFovTangents.top - focusFovTangents.bottom) / verticalFov) *
                            referenceView.viewport.height));

                        TraceLoggingWriteTagged(local,
                                                "varjo_EndFrameWithLayers_MultiProj_Patched",
                                                TLArg(k, "ViewIndex"),
                                                TLPArg(focusView.viewport.swapChain, "SwapChain"),
                                                TLArg(focusView.viewport.arrayIndex, "ArrayIndex"),
                                                TLArg(focusView.viewport.x, "X"),
                                                TLArg(focusView.viewport.y, "Y"),
                                                TLArg(focusView.viewport.width, "Width"),
                                                TLArg(focusView.viewport.height, "Height"));

                        // Patch to pass the focus FOV.
                        focusView.projection =
                            varjo_GetProjectionMatrix(const_cast<varjo_FovTangents*>(&focusFovTangents));

                        TraceLoggingWriteTagged(local,
                                                "varjo_EndFrameWithLayers_MultiProj_Patched",
                                                TLArg(k, "ViewIndex"),
                                                TLArg(atan(focusFovTangents.bottom), "Bottom"),
                                                TLArg(atan(focusFovTangents.top), "Top"),
                                                TLArg(atan(focusFovTangents.left), "Left"),
                                                TLArg(atan(focusFovTangents.right), "Right"));

#if USE_FOVEATED_TANGENTS
                        projAllocator.back().header.flags |= varjo_LayerFlag_Foveated;
#endif

                        // TODO: Strip extensions? Do the same process with depth extension?
                    }
                }
            }
        }
        newSubmitInfo.layers = newLayersPtr.data();

        original_EndFrameWithLayers(session, &newSubmitInfo);

        TraceLoggingWriteStop(local, "varjo_EndFrameWithLayers");
    }

    void InstallHooks() {
        std::filesystem::path dllRoot;
        HMODULE module;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&InstallHooks,
                               &module)) {
            wchar_t path[_MAX_PATH];
            GetModuleFileNameW(module, path, sizeof(path));
            dllRoot = std::filesystem::path(path).parent_path();
        }

        std::filesystem::path varjoHome;
        varjoHome = std::filesystem::path(getenv("ProgramFiles")) / "Varjo";

        bool isVrServer = false;
        {
            char path[_MAX_PATH];
            GetModuleFileNameA(nullptr, path, sizeof(path));
            std::string_view fullPath(path);
            isVrServer = fullPath.rfind("\\vrserver.exe") != std::string::npos;
        }
        TraceLoggingWrite(g_traceProvider, "InstallHooks", TLArg(isVrServer, "IsVrServer"));

        HMODULE varjoLib;
        bool isVarjoRuntime = false;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, (dllRoot / "VarjoLib.dll").c_str(), &varjoLib);
        TraceLoggingWrite(g_traceProvider,
                          "InstallHooks_Try",
                          TLArg((dllRoot / "VarjoLib.dll").c_str(), "Path"),
                          TLPArg(varjoLib, "Lib"));
        if (!varjoLib && isVrServer) {
            isVarjoRuntime = true;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, (dllRoot / "VarjoRuntime.dll").c_str(), &varjoLib);
            TraceLoggingWrite(g_traceProvider,
                              "InstallHooks_Try",
                              TLArg((dllRoot / "VarjoRuntime.dll").c_str(), "Path"),
                              TLPArg(varjoLib, "Lib"));
        }

#ifdef _DEBUG
        // For convenience, search the Varjo folder in order to allow running Quadinator in-place from VS.
        if (!varjoLib) {
            isVarjoRuntime = false;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_PIN, (varjoHome / "varjo-openxr" / "VarjoLib.dll").c_str(), &varjoLib);
            TraceLoggingWrite(g_traceProvider,
                              "InstallHooks_Try",
                              TLArg((varjoHome / "varjo-openxr" / "VarjoLib.dll").c_str(), "Path"),
                              TLPArg(varjoLib, "Lib"));
        }
        if (!varjoLib && isVrServer) {
            isVarjoRuntime = true;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (varjoHome / "varjo-compositor" / "VarjoRuntime.dll").c_str(),
                               &varjoLib);
            TraceLoggingWrite(g_traceProvider,
                              "InstallHooks_Try",
                              TLArg((varjoHome / "varjo-compositor" / "VarjoRuntime.dll").c_str(), "Path"),
                              TLPArg(varjoLib, "Lib"));
        }
#endif

        if (varjoLib) {
            TraceLoggingWrite(
                g_traceProvider, "InstallHooks", TLPArg(varjoLib, "Lib"), TLArg(isVarjoRuntime, "IsVarjoRuntime"));
            // clang-format off
            original_GetAlignedView = reinterpret_cast<decltype(original_GetAlignedView)>(
                GetProcAddress(varjoLib,
                               !isVarjoRuntime ? "varjo_GetAlignedView"
                                               : "struct_varjo_AlignedViewvarjo_GetAlignedViewdoubleP"));
            original_GetFovTangents = reinterpret_cast<decltype(original_GetFovTangents)>(
                GetProcAddress(varjoLib,
                               !isVarjoRuntime ? "varjo_GetFovTangents"
                                               : "varjo_FovTangentsvarjo_GetFovTangentsstruct_varjo_SessionPint32_t"));
            original_GetFoveatedFovTangents = reinterpret_cast<decltype(original_GetFoveatedFovTangents)>(
                GetProcAddress(varjoLib,
                               !isVarjoRuntime ? "varjo_GetFoveatedFovTangents"
                                               : "varjo_FovTangentsvarjo_GetFoveatedFovTangentsstruct_varjo_SessionPint32_tstruct_varjo_GazePstruct_varjo_FoveatedFovTangents_HintsP"));
            original_GetRenderingGaze = reinterpret_cast<decltype(original_GetRenderingGaze)>(
                GetProcAddress(varjoLib,
                               !isVarjoRuntime ? "varjo_GetRenderingGaze"
                                               : "varjo_Boolvarjo_GetRenderingGazestruct_varjo_SessionPstruct_varjo_GazeP"));
            DetourDllAttach(varjoLib,
                            !isVarjoRuntime ? "varjo_GetTextureSize"
                                            : "voidvarjo_GetTextureSizestruct_varjo_SessionPvarjo_TextureSize_Typeint32_tint32_tPint32_tP",
                            hooked_GetTextureSize,
                            original_GetTextureSize);
            DetourDllAttach(varjoLib,
                            !isVarjoRuntime ? "varjo_GetViewDescription"
                                            : "struct_varjo_ViewDescriptionvarjo_GetViewDescriptionstruct_varjo_SessionPint32_t",
                            hooked_GetViewDescription,
                            original_GetViewDescription);
            DetourDllAttach(varjoLib,
                            !isVarjoRuntime ? "varjo_EndFrameWithLayers"
                                            : "voidvarjo_EndFrameWithLayersstruct_varjo_SessionPstruct_varjo_SubmitInfoLayersP",
                            hooked_EndFrameWithLayers,
                            original_EndFrameWithLayers);
            // clang-format on
        }
    }

} // namespace

// Detours require at least one exported symbol.
void __declspec(dllexport) dummy() {
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DetourRestoreAfterWith();
        TraceLoggingRegister(g_traceProvider);
        TraceLoggingWrite(g_traceProvider, "Hello");
        InstallHooks();
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
