// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri
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

#pragma once

#include "pch.h"

#include "layer.h"
#include "log.h"
#include "util.h"
#include "dx11mirror.h"

#include <directxmath.h> // Matrix math functions and objects
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <winrt/base.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>

#include <cmath>
#include <deque>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
#define CHECK_DX(expression)                                                                                           \
    do {                                                                                                               \
        HRESULT res = (expression);                                                                                    \
        if (FAILED(res)) {                                                                                             \
            Log("DX Call failed with: 0x%08x\n", res);                                                                 \
            Log("CHECK_DX failed on: " #expression " DirectX error - see log for details\n");                         \
        }                                                                                                              \
    } while (0);

    using namespace layer_OBSMirror;
    using namespace layer_OBSMirror::log;
    using namespace DirectX; // Matrix math
    using namespace Mirror;

    // How long the game-side copy may wait for the mirror device to release
    // the shared texture before skipping this frame's mirror update.
    constexpr DWORD kAcquireTimeoutMs = 8;

    // Owning wrapper for Win32 handles so container/struct moves stay correct.
    class UniqueHandle {
      public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) : _handle(handle) {}
        UniqueHandle(UniqueHandle&& other) noexcept : _handle(other._handle) {
            other._handle = nullptr;
        }
        UniqueHandle& operator=(UniqueHandle&& other) noexcept {
            if (this != &other) {
                reset(other._handle);
                other._handle = nullptr;
            }
            return *this;
        }
        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;
        ~UniqueHandle() {
            reset();
        }

        void reset(HANDLE handle = nullptr) {
            if (_handle)
                CloseHandle(_handle);
            _handle = handle;
        }

        HANDLE get() const {
            return _handle;
        }

        explicit operator bool() const {
            return _handle != nullptr;
        }

      private:
        HANDLE _handle = nullptr;
    };

    // Returns false when the fence did not reach completionValue in time; the
    // caller must then skip work that reuses the associated resources.
    bool WaitForFence(ID3D12Fence* fence, UINT64 completionValue, HANDLE waitEvent) {
        if (fence->GetCompletedValue() >= completionValue)
            return true;
        if (FAILED(fence->SetEventOnCompletion(completionValue, waitEvent)))
            return false;
        return WaitForSingleObject(waitEvent, 1000) == WAIT_OBJECT_0;
    }

    using namespace xr::math;

    // Recording overscan is latched from the registry at instance creation and
    // must not change for the lifetime of the session (swapchain sizes and the
    // game's FOV are derived from it).
    constexpr wchar_t kConfigRegistryKey[] = L"Software\\OpenXR-OBSMirror";

    DWORD readConfigDword(const wchar_t* name, DWORD defaultValue) {
        DWORD value = defaultValue;
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kConfigRegistryKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
            ERROR_SUCCESS) {
            return defaultValue;
        }
        return value;
    }

    bool fovNearEqual(const XrFovf& a, const XrFovf& b) {
        return XMScalarNearEqual(a.angleLeft, b.angleLeft, 0.001f) &&
               XMScalarNearEqual(a.angleRight, b.angleRight, 0.001f) &&
               XMScalarNearEqual(a.angleUp, b.angleUp, 0.001f) &&
               XMScalarNearEqual(a.angleDown, b.angleDown, 0.001f);
    }

    class OpenXrLayer : public layer_OBSMirror::OpenXrApi {
      public:
        OpenXrLayer() {
            _overscanRequested = readConfigDword(L"RecordingOverscan", 0) != 0;
            if (_overscanRequested) {
                const DWORD hPercent =
                    std::clamp<DWORD>(readConfigDword(L"OverscanHorizontalPercent", 115), 100, 150);
                const DWORD vPercent = std::clamp<DWORD>(readConfigDword(L"OverscanVerticalPercent", 108), 100, 150);
                _overscanDesiredH = static_cast<float>(hPercent) / 100.0f;
                _overscanDesiredV = static_cast<float>(vPercent) / 100.0f;
                Log("Recording overscan requested: %.2fx horizontal, %.2fx vertical\n",
                    _overscanDesiredH,
                    _overscanDesiredV);
            }
        }

        ~OpenXrLayer() override = default;

        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledApiLayerNames[i], "ApiLayerName"));
            }
            for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledExtensionNames[i], "ExtensionName"));
            }

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // Dump the application name and OpenXR runtime information to help debugging customer issues.
            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(runtimeName.c_str(), "RuntimeName"));
            Log("Application: %s\n", GetApplicationName().c_str());
            Log("Using OpenXR runtime: %s\n", runtimeName.c_str());

            return XR_SUCCESS;
        }

        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            Log("xrCreateSession\n");
            if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSession",
                              TLXArg(instance, "Instance"),
                              TLArg(createInfo->systemId, "SystemId"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            // Walk the next chain looking for a graphics binding we support.
            // Unrelated chained structures (overlay extensions, vendor structs)
            // are ignored rather than clearing an already-found binding.
            XrStructureType boundApi = XR_TYPE_UNKNOWN;
            const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
            while (entry) {
                Log("Session create chain entry: %d\n", entry->type);
                if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    boundApi = entry->type;
                    const XrGraphicsBindingD3D11KHR* d3d11Bindings =
                        reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
                    _d3d11Device = d3d11Bindings->device;
                    _d3d11Device->GetImmediateContext(_d3d11Context.ReleaseAndGetAddressOf());
                } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                    boundApi = entry->type;
                    const XrGraphicsBindingD3D12KHR* d3d12Bindings =
                        reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
                    _d3d12Device = d3d12Bindings->device;
                    _d3d12CommandQueue = d3d12Bindings->queue;
                }

                entry = entry->next;
            }

            if (boundApi != XR_TYPE_UNKNOWN) {
                _xrGraphicsAPI = boundApi;
                Log("Graphics binding: %s\n", boundApi == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR ? "D3D11" : "D3D12");
            } else {
                Log("No supported graphics binding found (D3D11 or D3D12 required); mirroring disabled for this "
                    "session\n");
            }

            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (XR_SUCCEEDED(result)) {
                Session newSession;
                newSession._xrSession = *session;
                _sessions.insert_or_assign(*session, newSession);

                if (boundApi != XR_TYPE_UNKNOWN) {
                    ensureMirror();
                }

                // List off the views and store them locally for easy access
                XrSystemId xr_system;
                XrSystemGetInfo systemInfo{};
                systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
                systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                CHECK_XRCMD(xrGetSystem(instance, &systemInfo, &xr_system));

                uint32_t viewCount = 0;
                CHECK_XRCMD(xrEnumerateViewConfigurationViews(
                    instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));

                _xrViewsList = std::vector<XrViewConfigurationView>(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});

                CHECK_XRCMD(xrEnumerateViewConfigurationViews(instance,
                                                              xr_system,
                                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                              viewCount,
                                                              &viewCount,
                                                              _xrViewsList.data()));

                assert(viewCount == _xrViewsList.size());

                TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLXArg(*session, "Session"));
            }

            return result;
        }

        XrResult xrEnumerateViewConfigurationViews(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrViewConfigurationType viewConfigurationType,
                                                   uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput,
                                                   XrViewConfigurationView* views) override {
            const XrResult result = OpenXrApi::xrEnumerateViewConfigurationViews(
                instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);

            if (XR_SUCCEEDED(result) && _overscanRequested &&
                viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && views && viewCountOutput &&
                *viewCountOutput > 0 && viewCapacityInput >= *viewCountOutput) {
                computeOverscanScales(views, *viewCountOutput);

                if (overscanActive()) {
                    // Grow the recommended render target so pixels-per-degree
                    // stays constant across the widened FOV.
                    for (uint32_t i = 0; i < *viewCountOutput; ++i) {
                        const uint32_t scaledWidth = static_cast<uint32_t>(
                            std::lroundf(views[i].recommendedImageRectWidth * _overscanHScale));
                        const uint32_t scaledHeight = static_cast<uint32_t>(
                            std::lroundf(views[i].recommendedImageRectHeight * _overscanVScale));
                        views[i].recommendedImageRectWidth =
                            std::min(scaledWidth, views[i].maxImageRectWidth);
                        views[i].recommendedImageRectHeight =
                            std::min(scaledHeight, views[i].maxImageRectHeight);
                    }
                }
            }

            return result;
        }

        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLXArg(session, "Session"));
            Log("xrDestroySession\n");

            const XrResult result = OpenXrApi::xrDestroySession(session);
            if (XR_SUCCEEDED(result) && isSessionHandled(session)) {
                // Destroying a session destroys all of its child handles, so
                // drop the swapchains and spaces that belonged to it.
                for (auto it = _swapchains.begin(); it != _swapchains.end();) {
                    if (it->second._xrSession == session) {
                        if (_mirror) {
                            _mirror->removeSwapchain(it->first);
                        }
                        it = _swapchains.erase(it);
                    } else {
                        ++it;
                    }
                }
                _sessions.erase(session);

                if (_sessions.empty()) {
                    if (_mirror) {
                        _mirror->clearSpaces();
                    }
                    _projectionViews.clear();
                    _originalViewFovs.clear();
                    // Release the game's graphics objects so the layer does not
                    // keep the game's device alive after the session ends.
                    _d3d11Context.Reset();
                    _d3d11Device = nullptr;
                    _d3d12Device = nullptr;
                    _d3d12CommandQueue = nullptr;
                    _xrGraphicsAPI = XR_TYPE_UNKNOWN;
                }
            }

            return result;
        }

        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            Log("xrCreateSwapchain\n");
            if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSwapchain",
                              TLXArg(session, "Session"),
                              TLArg(createInfo->arraySize, "ArraySize"),
                              TLArg(createInfo->width, "Width"),
                              TLArg(createInfo->height, "Height"),
                              TLArg(createInfo->createFlags, "CreateFlags"),
                              TLArg(createInfo->format, "Format"),
                              TLArg(createInfo->faceCount, "FaceCount"),
                              TLArg(createInfo->mipCount, "MipCount"),
                              TLArg(createInfo->sampleCount, "SampleCount"),
                              TLArg(createInfo->usageFlags, "UsageFlags"));

            XrSwapchainCreateInfo chainCreateInfo = *createInfo;
            const bool handled = isSessionHandled(session);

            if (handled) {
                Log("Creating swapchain with dimensions=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, format=%d, "
                    "usage=0x%x\n",
                    createInfo->width,
                    createInfo->height,
                    createInfo->arraySize,
                    createInfo->mipCount,
                    createInfo->sampleCount,
                    createInfo->format,
                    createInfo->usageFlags);
            }

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, &chainCreateInfo, swapchain);
            if (handled && XR_SUCCEEDED(result)) {
                // On success, record the state.
                Swapchain newSwapchain;
                newSwapchain._xrSwapchain = *swapchain;
                newSwapchain._xrSession = session;
                newSwapchain._createInfo = chainCreateInfo;
                _swapchains.insert_or_assign(*swapchain, std::move(newSwapchain));
                Log("Tracking swapchain %p\n", *swapchain);

                TraceLoggingWrite(g_traceProvider, "xrCreateSwapchain", TLXArg(*swapchain, "Swapchain"));
            }

            return result;
        }

        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLXArg(swapchain, "Swapchain"));

            Log("xrDestroySwapchain %p\n", swapchain);
            const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);
            if (XR_SUCCEEDED(result) && isSwapchainHandled(swapchain)) {
                if (_mirror) {
                    _mirror->removeSwapchain(swapchain);
                }
                _swapchains.erase(swapchain);
            }

            return result;
        }

        XrResult xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                            uint32_t imageCapacityInput,
                                            uint32_t* imageCountOutput,
                                            XrSwapchainImageBaseHeader* images) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateSwapchainImages",
                              TLXArg(swapchain, "Swapchain"),
                              TLArg(imageCapacityInput, "ImageCapacityInput"));
            Log("xrEnumerateSwapchainImages swapChain %p imageCapacityInput %d\n", swapchain, imageCapacityInput);
            if (!isSwapchainHandled(swapchain) || imageCapacityInput == 0) {
                const XrResult result =
                    OpenXrApi::xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
                TraceLoggingWrite(
                    g_traceProvider, "xrEnumerateSwapchainImages", TLArg(*imageCountOutput, "ImageCountOutput"));
                Log("Result %d\n", result);
                return result;
            }

            // Enumerate the actual D3D swapchain images.
            auto& swapchainState = _swapchains[swapchain];
            const XrResult result =
                OpenXrApi::xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
            if (XR_SUCCEEDED(result) && _mirror && _mirror->initialized()) {
                Mirror::DxgiFormatInfo formatInfo{};
                const bool knownFormat =
                    Mirror::GetFormatInfo((DXGI_FORMAT)swapchainState._createInfo.format, formatInfo);
                const bool colorAttachment =
                    (swapchainState._createInfo.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
                const bool mirrorable = knownFormat && formatInfo.bpc <= 10 && colorAttachment;

                // Whether a swapchain is mirrored decides if OBS ever receives
                // pixels from it, so the decision and its reason must reach the
                // log in release builds too - once per swapchain.
                if (!swapchainState._mirrorDecisionLogged) {
                    swapchainState._mirrorDecisionLogged = true;
                    if (mirrorable) {
                        Log("Mirroring swapchain %p: %ux%u format %d usage 0x%x samples %u array %u\n",
                            swapchain,
                            swapchainState._createInfo.width,
                            swapchainState._createInfo.height,
                            swapchainState._createInfo.format,
                            swapchainState._createInfo.usageFlags,
                            swapchainState._createInfo.sampleCount,
                            swapchainState._createInfo.arraySize);
                    } else if (!knownFormat) {
                        Log("NOT mirroring swapchain %p: unsupported DXGI format %d. If the OBS capture stays "
                            "blank, the game renders in a format the mirror does not support.\n",
                            swapchain,
                            swapchainState._createInfo.format);
                    } else if (!colorAttachment) {
                        Log("NOT mirroring swapchain %p: no color-attachment usage (flags 0x%x) - typically a "
                            "depth or utility swapchain\n",
                            swapchain,
                            swapchainState._createInfo.usageFlags);
                    } else {
                        Log("NOT mirroring swapchain %p: %d bits per channel exceeds the supported 10 "
                            "(HDR-format swapchain, format %d). If the OBS capture stays blank, this is why.\n",
                            swapchain,
                            formatInfo.bpc,
                            swapchainState._createInfo.format);
                    }
                }

                if (mirrorable) {
                    if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                        Log("XR_TYPE_GRAPHICS_BINDING_D3D11_KHR\n");
                        swapchainState._dx11SurfaceImages.resize(*imageCountOutput);
                        for (uint32_t i = 0; i < *imageCountOutput; ++i) {
                            swapchainState._dx11SurfaceImages[i] =
                                reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[i];
                        }
                        if (swapchainState._dx11LastTexture) {
                            D3D11_TEXTURE2D_DESC srcDesc;
                            swapchainState._dx11LastTexture->GetDesc(&srcDesc);
                            if (srcDesc.Width != swapchainState._createInfo.width ||
                                srcDesc.Height != swapchainState._createInfo.height ||
                                srcDesc.ArraySize != swapchainState._createInfo.arraySize ||
                                srcDesc.Format != (DXGI_FORMAT)swapchainState._createInfo.format) {
                                swapchainState._dx11LastTexture = nullptr;
                                swapchainState._dx11KeyedMutex = nullptr;
                            }
                        }
                        if (swapchainState._dx11LastTexture == nullptr) {
                            D3D11_TEXTURE2D_DESC desc;
                            ZeroMemory(&desc, sizeof(desc));
                            desc.Width = swapchainState._createInfo.width;
                            desc.Height = swapchainState._createInfo.height;
                            desc.MipLevels = 1;
                            desc.ArraySize = swapchainState._createInfo.arraySize;
                            desc.Format = (DXGI_FORMAT)swapchainState._createInfo.format;
                            desc.SampleDesc.Count = 1;
                            desc.SampleDesc.Quality = 0;
                            desc.Usage = D3D11_USAGE_DEFAULT;
                            desc.CPUAccessFlags = 0;
                            // The keyed mutex orders the game-side copy against
                            // the mirror device's reads.
                            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                            CHECK_DX(_d3d11Device->CreateTexture2D(
                                &desc, NULL, swapchainState._dx11LastTexture.ReleaseAndGetAddressOf()));

                            swapchainState._dx11KeyedMutex.Reset();
                            if (swapchainState._dx11LastTexture) {
                                swapchainState._dx11LastTexture.As(&swapchainState._dx11KeyedMutex);
                                _mirror->createSharedMirrorTexture(swapchain, swapchainState._dx11LastTexture);
                            }
                        }
                    } else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                        Log("XR_TYPE_GRAPHICS_BINDING_D3D12_KHR\n");
                        swapchainState._frameFenceEvents.clear();
                        swapchainState._frameFences.clear();
                        swapchainState._fenceValues.clear();
                        swapchainState._dx12SurfaceImages.resize(*imageCountOutput);
                        swapchainState._commandAllocators.resize(*imageCountOutput);
                        swapchainState._commandLists.resize(*imageCountOutput);
                        swapchainState._frameFenceEvents.resize(*imageCountOutput);
                        swapchainState._frameFences.resize(*imageCountOutput);
                        swapchainState._fenceValues.resize(*imageCountOutput);

                        for (uint32_t i = 0; i < *imageCountOutput; ++i) {
                            swapchainState._dx12SurfaceImages[i] =
                                reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[i];

                            swapchainState._frameFenceEvents[i].reset(CreateEvent(nullptr, FALSE, FALSE, nullptr));
                            swapchainState._fenceValues[i] = 0;
                            CHECK_DX(_d3d12Device->CreateFence(
                                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&swapchainState._frameFences[i])));

                            CHECK_DX(_d3d12Device->CreateCommandAllocator(
                                D3D12_COMMAND_LIST_TYPE_DIRECT,
                                IID_PPV_ARGS(&swapchainState._commandAllocators[i])));
                            CHECK_DX(_d3d12Device->CreateCommandList(0,
                                                                     D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                     swapchainState._commandAllocators[i].Get(),
                                                                     nullptr,
                                                                     IID_PPV_ARGS(&swapchainState._commandLists[i])));
                            if (swapchainState._commandLists[i]) {
                                swapchainState._commandLists[i]->Close();
                            }
                        }
                        if (swapchainState._dx12LastTexture) {
                            D3D12_RESOURCE_DESC srcDesc = swapchainState._dx12LastTexture->GetDesc();
                            if (srcDesc.Width != swapchainState._createInfo.width ||
                                srcDesc.Height != swapchainState._createInfo.height ||
                                srcDesc.DepthOrArraySize != swapchainState._createInfo.arraySize ||
                                srcDesc.Format != (DXGI_FORMAT)swapchainState._createInfo.format) {
                                swapchainState._dx12LastTexture = nullptr;
                            }
                        }
                        if (swapchainState._dx12LastTexture == nullptr) {
                            D3D12_RESOURCE_DESC d3d12TextureDesc{};
                            d3d12TextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                            d3d12TextureDesc.Alignment = 0;
                            d3d12TextureDesc.Width = swapchainState._createInfo.width;
                            d3d12TextureDesc.Height = swapchainState._createInfo.height;
                            d3d12TextureDesc.DepthOrArraySize = swapchainState._createInfo.arraySize;
                            d3d12TextureDesc.MipLevels = 1;
                            d3d12TextureDesc.Format = (DXGI_FORMAT)swapchainState._createInfo.format;
                            d3d12TextureDesc.SampleDesc.Count = 1;
                            d3d12TextureDesc.SampleDesc.Quality = 0;
                            d3d12TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                            d3d12TextureDesc.Flags =
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

                            D3D12_HEAP_PROPERTIES heapProperties;
                            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
                            heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                            heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                            heapProperties.CreationNodeMask = 0;
                            heapProperties.VisibleNodeMask = 0;

                            D3D12_CLEAR_VALUE clearValue{};
                            clearValue.Format = d3d12TextureDesc.Format;

                            CHECK_DX(
                                _d3d12Device->CreateCommittedResource(&heapProperties,
                                                                      D3D12_HEAP_FLAG_SHARED,
                                                                      &d3d12TextureDesc,
                                                                      D3D12_RESOURCE_STATE_COMMON,
                                                                      &clearValue,
                                                                      IID_PPV_ARGS(&swapchainState._dx12LastTexture)));
                            if (!swapchainState._dx12LastTexture) {
                                return result;
                            }

                            swapchainState._sharedHandle.reset();
                            HANDLE sharedHandle = nullptr;
                            CHECK_DX(_d3d12Device->CreateSharedHandle(swapchainState._dx12LastTexture.Get(),
                                                                      nullptr,
                                                                      GENERIC_ALL,
                                                                      nullptr,
                                                                      &sharedHandle));
                            if (!sharedHandle) {
                                swapchainState._dx12LastTexture = nullptr;
                                return result;
                            }
                            swapchainState._sharedHandle.reset(sharedHandle);

                            // A shared fence lets the mirror device wait for the
                            // game queue's copy before sampling the texture.
                            swapchainState._copyFence.Reset();
                            swapchainState._copyFenceHandle.reset();
                            swapchainState._copyFenceValue = 0;
                            if (SUCCEEDED(_d3d12Device->CreateFence(
                                    0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&swapchainState._copyFence)))) {
                                HANDLE fenceHandle = nullptr;
                                if (SUCCEEDED(_d3d12Device->CreateSharedHandle(
                                        swapchainState._copyFence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
                                    swapchainState._copyFenceHandle.reset(fenceHandle);
                                } else {
                                    swapchainState._copyFence.Reset();
                                }
                            }

                            _mirror->createSharedMirrorTexture(
                                swapchain, swapchainState._sharedHandle.get(), swapchainState._copyFenceHandle.get());
                        }
                    }
                }
            } else {
                if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
                    swapchainState._dx11SurfaceImages.clear();
                else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)
                    swapchainState._dx12SurfaceImages.clear();
            }

            return result;
        }

        XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageAcquireInfo* acquireInfo,
                                         uint32_t* index) override {
            if (acquireInfo && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            const XrResult result = OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);

            if (XR_SUCCEEDED(result) && isSwapchainHandled(swapchain)) {
                auto& swapchainState = _swapchains[swapchain];
                // Apps may acquire several images before releasing any; track
                // the queue so each release copies the image actually released.
                swapchainState._acquiredIndices.push_back(*index);
                swapchainState._lastAcquiredIndex = *index;
            }

            return result;
        }

        XrResult updateSwapChainImages(XrSwapchain swapchain,
                                       const XrSwapchainImageReleaseInfo* releaseInfo,
                                       bool doXRcall) {
            if (_mirror && _mirror->enabled() && isSwapchainHandled(swapchain)) {
                Swapchain& swapchainState = _swapchains[swapchain];

                // The runtime releases the oldest acquired image; a refresh
                // outside of release (quad layers) copies the newest one.
                uint32_t idx = UINT32_MAX;
                if (doXRcall) {
                    if (!swapchainState._acquiredIndices.empty())
                        idx = swapchainState._acquiredIndices.front();
                } else {
                    idx = swapchainState._lastAcquiredIndex;
                }

                if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR &&
                    idx < swapchainState._dx11SurfaceImages.size()) {
                    auto* textPtr = swapchainState._dx11SurfaceImages[idx].texture;
                    if (swapchainState._dx11LastTexture) {
                        bool acquired = true;
                        if (swapchainState._dx11KeyedMutex)
                            acquired = swapchainState._dx11KeyedMutex->AcquireSync(0, kAcquireTimeoutMs) == S_OK;
                        if (acquired) {
                            _d3d11Context->CopyResource(swapchainState._dx11LastTexture.Get(), textPtr);
                            if (swapchainState._dx11KeyedMutex)
                                swapchainState._dx11KeyedMutex->ReleaseSync(0);
                            swapchainState._lastCopiedIndex = idx;
                        }
                        noteMirrorCopyResult(swapchainState, acquired, "keyed mutex timeout");
                    }
                } else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR &&
                           idx < swapchainState._dx12SurfaceImages.size()) {
                    auto* textPtr = swapchainState._dx12SurfaceImages[idx].texture;
                    if (swapchainState._dx12LastTexture && swapchainState._commandLists[idx] &&
                        swapchainState._commandAllocators[idx] && swapchainState._frameFences[idx]) {
                        if (WaitForFence(swapchainState._frameFences[idx].Get(),
                                         swapchainState._fenceValues[idx],
                                         swapchainState._frameFenceEvents[idx].get())) {
                            swapchainState._commandAllocators[idx]->Reset();
                            swapchainState._commandLists[idx]->Reset(swapchainState._commandAllocators[idx].Get(),
                                                                     nullptr);
                            swapchainState._commandLists[idx]->CopyResource(swapchainState._dx12LastTexture.Get(),
                                                                            textPtr);
                            swapchainState._commandLists[idx]->Close();
                            ID3D12CommandList* set[] = {swapchainState._commandLists[idx].Get()};
                            _d3d12CommandQueue->ExecuteCommandLists(1, set);

                            // Tell the mirror device when this copy will be done.
                            if (swapchainState._copyFence) {
                                const UINT64 copyValue = ++swapchainState._copyFenceValue;
                                _d3d12CommandQueue->Signal(swapchainState._copyFence.Get(), copyValue);
                                _mirror->notifyFenceValue(swapchain, copyValue);
                            }
                            swapchainState._lastCopiedIndex = idx;
                            noteMirrorCopyResult(swapchainState, true, nullptr);
                        } else {
                            noteMirrorCopyResult(swapchainState, false, "fence wait timed out");
                        }
                    }
                }
            }

            XrResult result{XR_SUCCESS};
            if (doXRcall) {
                result = OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
                if (XR_SUCCEEDED(result) && isSwapchainHandled(swapchain)) {
                    auto& acquiredIndices = _swapchains[swapchain]._acquiredIndices;
                    if (!acquiredIndices.empty())
                        acquiredIndices.pop_front();
                }
            }

            if (_mirror && _mirror->enabled() && isSwapchainHandled(swapchain) &&
                _xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                auto& swapchainState = _swapchains[swapchain];
                const uint32_t idx = swapchainState._lastCopiedIndex;
                if (idx < swapchainState._dx12SurfaceImages.size() && swapchainState._frameFences[idx]) {
                    // Guards command allocator reuse for this image slot.
                    const auto fenceValue = _currentFenceValue;
                    _d3d12CommandQueue->Signal(swapchainState._frameFences[idx].Get(), fenceValue);
                    swapchainState._fenceValues[idx] = fenceValue;
                    ++_currentFenceValue;
                }
            }
            return result;
        }

        XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageReleaseInfo* releaseInfo) override {
            return updateSwapChainImages(swapchain, releaseInfo, true);
        }

        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            XrResult res =
                OpenXrApi::xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

            // Hand the game a widened FOV so it renders extra perimeter for the
            // recording. The submission is cropped back in xrEndFrame, so the
            // headset never sees the wide image.
            if (XR_SUCCEEDED(res) && overscanActive() && views && viewCountOutput &&
                viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
                viewCapacityInput >= *viewCountOutput) {
                _originalViewFovs.resize(*viewCountOutput);
                for (uint32_t nView = 0; nView < *viewCountOutput; nView++) {
                    _originalViewFovs[nView] = views[nView].fov;
                    views[nView].fov = widenFov(views[nView].fov);
                }
            }

            if (_mirror && _mirror->enabled() && XR_SUCCEEDED(res)) {
                auto siPtr = _mirror->getSpaceInfo(viewLocateInfo->space);
                if (views && !siPtr && !_untrackedViewSpaceLogged) {
                    // Without a tracked reference space the projection views
                    // are never recorded and the mirror stays blank forever.
                    _untrackedViewSpaceLogged = true;
                    Log("xrLocateViews uses space %p which is not a tracked reference space; views located against "
                        "it cannot be mirrored\n",
                        viewLocateInfo->space);
                }
                if (views && siPtr) {
                    if (_projectionViews.size() != *viewCountOutput) {
                        Log("Reference Space Type: %d\n", siPtr->referenceSpaceType);
                        _projectionViews.resize(*viewCountOutput, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                    }
                    for (uint32_t nView = 0; nView < *viewCountOutput; nView++) {
                        _projectionViews[nView].fov = views[nView].fov;

                        XrPosef pose = views[nView].pose;

                        // Make sure we at least have halfway-sane values if the runtime isn't providing them. In
                        // particular if the runtime gives us an invalid orientation, that'd otherwise cause
                        // XR_ERROR_POSE_INVALID errors later.
                        if ((viewState->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
                            pose.orientation = XrQuaternionf{0, 0, 0, 1};
                        }
                        if ((viewState->viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
                            pose.position = XrVector3f{0, 1.5, 0};
                        }

                        _projectionViews[nView].pose = pose;
                    }
                }
            }
            return res;
        }

        XrResult xrGetVisibilityMaskKHR(XrSession session,
                                        XrViewConfigurationType viewConfigurationType,
                                        uint32_t viewIndex,
                                        XrVisibilityMaskTypeKHR visibilityMaskType,
                                        XrVisibilityMaskKHR* visibilityMask) override {
            // The runtime's mask describes the displayed (original) FOV; with
            // overscan the game would stencil away perimeter pixels that the
            // recording needs. Report an empty mask so everything is rendered;
            // the runtime still applies its own mask to the displayed crop.
            if (overscanActive() && viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                if (visibilityMask) {
                    visibilityMask->vertexCountOutput = 0;
                    visibilityMask->indexCountOutput = 0;
                }
                return XR_SUCCESS;
            }

            // Resolve directly: the pointer may legitimately be absent when the
            // app did not enable XR_KHR_visibility_mask.
            PFN_xrGetVisibilityMaskKHR pfnGetVisibilityMask = nullptr;
            m_xrGetInstanceProcAddr(GetXrInstance(),
                                    "xrGetVisibilityMaskKHR",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVisibilityMask));
            if (!pfnGetVisibilityMask) {
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            }
            return pfnGetVisibilityMask(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
        }

        XrResult xrCreateReferenceSpace(XrSession session,
                                        const XrReferenceSpaceCreateInfo* createInfo,
                                        XrSpace* space) override {
            XrResult res = OpenXrApi::xrCreateReferenceSpace(session, createInfo, space);
            if (_mirror && XR_SUCCEEDED(res)) {
                _mirror->addSpace(*space, createInfo);
            }
            return res;
        }

        XrResult xrDestroySpace(XrSpace space) override {
            XrResult res = OpenXrApi::xrDestroySpace(space);
            if (_mirror && XR_SUCCEEDED(res)) {
                _mirror->removeSpace(space);
            }
            return res;
        }

        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            if (_mirror)
                _mirror->flush();
            return OpenXrApi::xrBeginFrame(session, frameBeginInfo);
        }

        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            if (_mirror) {
                _mirror->checkOBSRunning();

                // Classify how far this frame makes it through the mirror
                // pipeline; noteMirrorOutcome() logs the transitions.
                MirrorOutcome outcome = MirrorOutcome::Unset;
                if (isSessionHandled(session)) {
                    if (!_mirror->initialized())
                        outcome = MirrorOutcome::MirrorUnavailable;
                    else if (!_mirror->enabled())
                        outcome = MirrorOutcome::WaitingForObs;
                    else if (_projectionViews.empty() || _xrViewsList.empty())
                        outcome = MirrorOutcome::NoViewData;
                    else
                        outcome = MirrorOutcome::NoProjectionLayer;
                }

                if (outcome >= MirrorOutcome::NoProjectionLayer) {
                    const XrCompositionLayerProjectionView* projView = &_projectionViews[0];
                    const XrCompositionLayerProjection* projLayer = nullptr;

                    _projectionViews[0].subImage.imageRect.offset.x = 0;
                    _projectionViews[0].subImage.imageRect.offset.y = 0;
                    _projectionViews[0].subImage.imageRect.extent.width = _xrViewsList[0].recommendedImageRectWidth;
                    _projectionViews[0].subImage.imageRect.extent.height = _xrViewsList[0].recommendedImageRectHeight;

                    const bool includeQuadLayers = mirrorQuadLayers();
                    uint32_t count = frameEndInfo->layerCount;
                    for (uint32_t i = 0; i < count; ++i) {
                        const XrCompositionLayerBaseHeader* hdr = frameEndInfo->layers[i];
                        if (hdr->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                            projLayer = reinterpret_cast<const XrCompositionLayerProjection*>(hdr);
                            if (projLayer->viewCount >= 2) {
                                if (_mirror->getEyeIndex() < 2) {
                                    const uint32_t eyeIndex = _mirror->getEyeIndex();
                                    projView = &projLayer->views[eyeIndex];
                                    if (isSwapchainHandled(projView->subImage.swapchain)) {
                                        auto& swapchainState = _swapchains[projView->subImage.swapchain];
                                        if (swapchainState._dx11LastTexture || swapchainState._dx12LastTexture) {
                                            const XrFovf& mirrorFov = eyeIndex < _projectionViews.size()
                                                                                ? _projectionViews[eyeIndex].fov
                                                                                : _projectionViews[0].fov;
                                            const bool drew =
                                                _mirror->Blend(projView,
                                                               mirrorFov,
                                                               (DXGI_FORMAT)swapchainState._createInfo.format,
                                                               projLayer->space,
                                                               frameEndInfo->displayTime);
                                            outcome = std::max(
                                                outcome,
                                                drew ? MirrorOutcome::Mirroring : MirrorOutcome::DrawFailed);
                                        } else {
                                            outcome = std::max(outcome, MirrorOutcome::TextureNotReady);
                                        }
                                    } else {
                                        outcome = std::max(outcome, MirrorOutcome::SwapchainNotTracked);
                                    }
                                } else if (_projectionViews.size() >= 2) {
                                    projView = &projLayer->views[0];
                                    const XrCompositionLayerProjectionView* projView2 = &projLayer->views[1];
                                    if (isSwapchainHandled(projView->subImage.swapchain) &&
                                        isSwapchainHandled(projView2->subImage.swapchain)) {
                                        auto& swapchainState = _swapchains[projView->subImage.swapchain];
                                        auto& swapchainState2 = _swapchains[projView2->subImage.swapchain];
                                        if ((swapchainState._dx11LastTexture || swapchainState._dx12LastTexture) &&
                                            (swapchainState2._dx11LastTexture || swapchainState2._dx12LastTexture)) {
                                            const bool drew =
                                                _mirror->Blend(projView,
                                                               _projectionViews[0].fov,
                                                               projView2,
                                                               _projectionViews[1].fov,
                                                               (DXGI_FORMAT)swapchainState._createInfo.format,
                                                               projLayer->space,
                                                               frameEndInfo->displayTime);
                                            outcome = std::max(
                                                outcome,
                                                drew ? MirrorOutcome::Mirroring : MirrorOutcome::DrawFailed);
                                        } else {
                                            outcome = std::max(outcome, MirrorOutcome::TextureNotReady);
                                        }
                                    } else {
                                        outcome = std::max(outcome, MirrorOutcome::SwapchainNotTracked);
                                    }
                                }
                            }
                        } else if (hdr->type == XR_TYPE_COMPOSITION_LAYER_QUAD && includeQuadLayers) {
                            const XrCompositionLayerQuad* quadLayer =
                                reinterpret_cast<const XrCompositionLayerQuad*>(hdr);
                            if (isSwapchainHandled(quadLayer->subImage.swapchain)) {
                                auto& swapchainState = _swapchains[quadLayer->subImage.swapchain];
                                if (swapchainState._lastAcquiredIndex != swapchainState._lastCopiedIndex) {
                                    // Probably missed an update to swap chain whilst waiting for OBS plugin
                                    // Swapchains don't need to be updated every frame so just copy the last one aquired
                                    updateSwapChainImages(quadLayer->subImage.swapchain, nullptr, false);
                                }
                                if (swapchainState._dx11LastTexture || swapchainState._dx12LastTexture) {
                                    if (projView) {
                                        _mirror->Blend(projView,
                                                       _projectionViews[0].fov,
                                                       quadLayer,
                                                       (DXGI_FORMAT)swapchainState._createInfo.format,
                                                       projLayer ? projLayer->space : XR_NULL_HANDLE,
                                                       frameEndInfo->displayTime);
                                    }
                                }
                            }
                        }
                    }
                    _mirror->copyToMirror();
                }
                if (outcome != MirrorOutcome::Unset)
                    noteMirrorOutcome(outcome);
            }

            // With overscan active, OBS has already been fed the full wide
            // image above; now restore the original FOV and submit only the
            // central crop to the runtime so the headset view is unchanged.
            const XrFrameEndInfo* submitInfo = frameEndInfo;
            XrFrameEndInfo patchedFrameEndInfo;
            if (overscanActive() && frameEndInfo->layerCount > 0 && !_originalViewFovs.empty() &&
                buildOverscanSubmission(frameEndInfo, patchedFrameEndInfo)) {
                submitInfo = &patchedFrameEndInfo;
            }

            return OpenXrApi::xrEndFrame(session, submitInfo);
        }

      private:
        // State associated with an OpenXR session.
        struct Session {
            XrSession _xrSession{XR_NULL_HANDLE};
        };

        struct Swapchain {
            XrSwapchain _xrSwapchain{XR_NULL_HANDLE};
            XrSession _xrSession{XR_NULL_HANDLE};
            XrSwapchainCreateInfo _createInfo{};
            std::vector<XrSwapchainImageD3D11KHR> _dx11SurfaceImages;
            std::vector<XrSwapchainImageD3D12KHR> _dx12SurfaceImages;
            std::deque<uint32_t> _acquiredIndices;
            uint32_t _lastAcquiredIndex = UINT32_MAX;
            uint32_t _lastCopiedIndex = UINT32_MAX;
            ComPtr<ID3D11Texture2D> _dx11LastTexture = nullptr;
            ComPtr<IDXGIKeyedMutex> _dx11KeyedMutex = nullptr;
            ComPtr<ID3D12Resource> _dx12LastTexture = nullptr;
            std::vector<ComPtr<ID3D12GraphicsCommandList>> _commandLists;
            std::vector<ComPtr<ID3D12CommandAllocator>> _commandAllocators;
            std::vector<UniqueHandle> _frameFenceEvents;
            std::vector<ComPtr<ID3D12Fence>> _frameFences;
            std::vector<UINT64> _fenceValues;
            UniqueHandle _sharedHandle;
            // Shared with the mirror device so it can wait for our copies.
            ComPtr<ID3D12Fence> _copyFence = nullptr;
            UniqueHandle _copyFenceHandle;
            UINT64 _copyFenceValue = 0;
            // Diagnostics: the mirror decision is logged once per swapchain,
            // and persistent copy-skip streaks are reported with recovery.
            bool _mirrorDecisionLogged = false;
            uint32_t _copySkipStreak = 0;
            bool _copySkipWarned = false;
        };

        // Why the most recent frame did not (or did) reach OBS, ordered by how
        // far the frame progressed through the mirror pipeline.
        enum class MirrorOutcome : uint32_t {
            Unset = 0,
            MirrorUnavailable,
            WaitingForObs,
            NoViewData,
            NoProjectionLayer,
            SwapchainNotTracked,
            TextureNotReady,
            DrawFailed,
            Mirroring,
        };

        static const char* describeMirrorOutcome(MirrorOutcome outcome) {
            switch (outcome) {
            case MirrorOutcome::MirrorUnavailable:
                return "mirror initialization failed - no capture possible";
            case MirrorOutcome::WaitingForObs:
                return "waiting for the OBS plugin heartbeat (is OBS running with an active OpenXR Mirror source?)";
            case MirrorOutcome::NoViewData:
                return "waiting for view data from xrLocateViews against a tracked reference space";
            case MirrorOutcome::NoProjectionLayer:
                return "no stereo projection layer in the game's frame submission";
            case MirrorOutcome::SwapchainNotTracked:
                return "the projection layer uses a swapchain the layer is not tracking";
            case MirrorOutcome::TextureNotReady:
                return "the swapchain has no mirror copy texture (see the swapchain mirroring decisions above)";
            case MirrorOutcome::DrawFailed:
                return "the mirror could not draw (source not registered or ring creation failed - see errors above)";
            case MirrorOutcome::Mirroring:
                return "actively mirroring frames to OBS";
            default:
                return "session start";
            }
        }

        // Logs mirror pipeline state transitions so a single log file explains
        // why OBS shows (or stops showing) frames. Capped in case a game flaps
        // between states every frame.
        void noteMirrorOutcome(MirrorOutcome outcome) {
            if (outcome == _lastMirrorOutcome) {
                ++_mirrorOutcomeFrames;
                const ULONGLONG now = GetTickCount64();
                if (_lastMirrorHealthLogTick == 0)
                    _lastMirrorHealthLogTick = now;
                else if (now - _lastMirrorHealthLogTick >= 30000) {
                    _lastMirrorHealthLogTick = now;
                    Log("Mirror health: %s for %u consecutive xrEndFrame calls. This confirms the pipeline state, "
                        "not whether the published pixels are non-black; use the Control Center Preview diagnostics "
                        "log for pixel sampling.\n",
                        describeMirrorOutcome(outcome),
                        _mirrorOutcomeFrames);
                }
                return;
            }
            if (_mirrorOutcomeTransitionLogs < 40) {
                ++_mirrorOutcomeTransitionLogs;
                Log("Mirror state: %s (after %u frames of: %s)\n",
                    describeMirrorOutcome(outcome),
                    _mirrorOutcomeFrames,
                    describeMirrorOutcome(_lastMirrorOutcome));
                if (_mirrorOutcomeTransitionLogs == 40) {
                    Log("Mirror state keeps changing; further transitions will not be logged\n");
                }
            }
            _lastMirrorOutcome = outcome;
            _mirrorOutcomeFrames = 0;
            _lastMirrorHealthLogTick = GetTickCount64();
        }

        static void noteMirrorCopyResult(Swapchain& state, bool copied, const char* reason) {
            if (copied) {
                if (state._copySkipWarned) {
                    Log("Mirror source copy recovered for swapchain %p after %u skipped frames\n",
                        state._xrSwapchain,
                        state._copySkipStreak);
                }
                state._copySkipStreak = 0;
                state._copySkipWarned = false;
                return;
            }
            ++state._copySkipStreak;
            if (!state._copySkipWarned && state._copySkipStreak >= 90) {
                state._copySkipWarned = true;
                Log("Mirror source copy skipped %u consecutive frames for swapchain %p (%s); the OBS capture will "
                    "be stale or blank\n",
                    state._copySkipStreak,
                    state._xrSwapchain,
                    reason ? reason : "unknown reason");
            }
        }

        // ---- Recording overscan (experimental) ----

        bool overscanActive() const {
            return _overscanRequested && _overscanScalesComputed &&
                   (_overscanHScale > 1.001f || _overscanVScale > 1.001f);
        }

        bool mirrorQuadLayers() {
            const ULONGLONG now = GetTickCount64();
            if (_quadLayerConfigInitialized && now - _lastQuadLayerConfigCheckTick < 250)
                return _mirrorQuadLayers;

            _lastQuadLayerConfigCheckTick = now;
            const bool visible = readConfigDword(L"MirrorQuadLayers", 1) != 0;
            if (!_quadLayerConfigInitialized || visible != _mirrorQuadLayers) {
                Log("Recording OpenXR quad layers: %s\n", visible ? "included" : "hidden");
            }
            _quadLayerConfigInitialized = true;
            _mirrorQuadLayers = visible;
            return _mirrorQuadLayers;
        }

        void computeOverscanScales(const XrViewConfigurationView* views, uint32_t viewCount) {
            if (_overscanScalesComputed)
                return;

            // Never exceed what the runtime allows for swapchain sizes; if the
            // limits remove all headroom, overscan disables itself rather than
            // degrading pixels-per-degree in the headset.
            float hScale = _overscanDesiredH;
            float vScale = _overscanDesiredV;
            for (uint32_t i = 0; i < viewCount; ++i) {
                if (views[i].recommendedImageRectWidth > 0 && views[i].maxImageRectWidth > 0) {
                    hScale = std::min(hScale,
                                      static_cast<float>(views[i].maxImageRectWidth) /
                                          static_cast<float>(views[i].recommendedImageRectWidth));
                }
                if (views[i].recommendedImageRectHeight > 0 && views[i].maxImageRectHeight > 0) {
                    vScale = std::min(vScale,
                                      static_cast<float>(views[i].maxImageRectHeight) /
                                          static_cast<float>(views[i].recommendedImageRectHeight));
                }
            }
            _overscanHScale = std::max(hScale, 1.0f);
            _overscanVScale = std::max(vScale, 1.0f);
            _overscanScalesComputed = true;

            if (overscanActive()) {
                Log("Recording overscan active: %.3fx horizontal, %.3fx vertical\n", _overscanHScale, _overscanVScale);
            } else {
                Log("Recording overscan disabled by runtime swapchain limits\n");
            }
        }

        // Widen by scaling the tangent of each half-angle, not the angle
        // itself, so the expansion is linear in render-target pixels.
        XrFovf widenFov(const XrFovf& fov) const {
            XrFovf wide;
            wide.angleLeft = atanf(tanf(fov.angleLeft) * _overscanHScale);
            wide.angleRight = atanf(tanf(fov.angleRight) * _overscanHScale);
            wide.angleUp = atanf(tanf(fov.angleUp) * _overscanVScale);
            wide.angleDown = atanf(tanf(fov.angleDown) * _overscanVScale);
            return wide;
        }

        // Sub-rect of `rect` (rendered with fov `wide`) covering exactly `orig`.
        static XrRect2Di computeCenterCrop(const XrRect2Di& rect, const XrFovf& wide, const XrFovf& orig) {
            const float wideLeft = tanf(wide.angleLeft);
            const float wideRight = tanf(wide.angleRight);
            const float wideUp = tanf(wide.angleUp);
            const float wideDown = tanf(wide.angleDown);
            const float origLeft = tanf(orig.angleLeft);
            const float origRight = tanf(orig.angleRight);
            const float origUp = tanf(orig.angleUp);
            const float origDown = tanf(orig.angleDown);

            const float uSpan = wideRight - wideLeft;
            const float vSpan = wideUp - wideDown;
            if (uSpan <= 0.0f || vSpan <= 0.0f || rect.extent.width <= 0 || rect.extent.height <= 0) {
                return rect;
            }

            const float u0 = (origLeft - wideLeft) / uSpan;
            const float u1 = (origRight - wideLeft) / uSpan;
            // D3D images have row 0 at the top, which maps to angleUp.
            const float v0 = (wideUp - origUp) / vSpan;
            const float v1 = (wideUp - origDown) / vSpan;

            XrRect2Di crop;
            crop.offset.x = rect.offset.x + static_cast<int32_t>(std::lroundf(u0 * rect.extent.width));
            crop.offset.y = rect.offset.y + static_cast<int32_t>(std::lroundf(v0 * rect.extent.height));
            crop.extent.width = static_cast<int32_t>(std::lroundf((u1 - u0) * rect.extent.width));
            crop.extent.height = static_cast<int32_t>(std::lroundf((v1 - v0) * rect.extent.height));

            // Keep the crop inside the source rect.
            crop.offset.x = std::clamp(crop.offset.x, rect.offset.x, rect.offset.x + rect.extent.width - 1);
            crop.offset.y = std::clamp(crop.offset.y, rect.offset.y, rect.offset.y + rect.extent.height - 1);
            crop.extent.width = std::clamp(crop.extent.width, 1, rect.offset.x + rect.extent.width - crop.offset.x);
            crop.extent.height = std::clamp(crop.extent.height, 1, rect.offset.y + rect.extent.height - crop.offset.y);
            return crop;
        }

        // The depth subimage must receive the identical angular crop or the
        // runtime would reproject with misaligned depth.
        void patchDepthInfo(XrCompositionLayerProjectionView& view, const XrFovf& wide, const XrFovf& orig) {
            const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(view.next);
            if (!entry)
                return;

            if (entry->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                const XrCompositionLayerDepthInfoKHR* depth =
                    reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(entry);
                _patchedDepthInfos.push_back(*depth);
                XrCompositionLayerDepthInfoKHR& newDepth = _patchedDepthInfos.back();
                newDepth.subImage.imageRect = computeCenterCrop(depth->subImage.imageRect, wide, orig);
                view.next = &newDepth;
                return;
            }

            // Depth chained behind another structure cannot be rewritten
            // without cloning the whole chain; warn once so incompatibilities
            // are diagnosable.
            for (; entry; entry = entry->next) {
                if (entry->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR && !_depthPatchWarned) {
                    _depthPatchWarned = true;
                    Log("Recording overscan: depth info is not first in the view chain and was not cropped\n");
                }
            }
        }

        // Deep-copies the projection layers with the original FOV and central
        // crop applied. Returns false when nothing needed patching.
        bool buildOverscanSubmission(const XrFrameEndInfo* frameEndInfo, XrFrameEndInfo& patched) {
            size_t projLayerCount = 0;
            size_t viewTotal = 0;
            for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
                const XrCompositionLayerBaseHeader* hdr = frameEndInfo->layers[i];
                if (hdr && hdr->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    ++projLayerCount;
                    viewTotal += reinterpret_cast<const XrCompositionLayerProjection*>(hdr)->viewCount;
                }
            }
            if (projLayerCount == 0)
                return false;

            _patchedLayerPtrs.clear();
            _patchedProjLayers.clear();
            _patchedProjViews.clear();
            _patchedDepthInfos.clear();
            // Reserve up front so the pointers taken below stay stable.
            _patchedLayerPtrs.reserve(frameEndInfo->layerCount);
            _patchedProjLayers.reserve(projLayerCount);
            _patchedProjViews.reserve(viewTotal);
            _patchedDepthInfos.reserve(viewTotal);

            bool anyPatched = false;
            for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
                const XrCompositionLayerBaseHeader* hdr = frameEndInfo->layers[i];
                if (!hdr || hdr->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    _patchedLayerPtrs.push_back(hdr);
                    continue;
                }

                const XrCompositionLayerProjection* projLayer =
                    reinterpret_cast<const XrCompositionLayerProjection*>(hdr);
                _patchedProjLayers.push_back(*projLayer);
                XrCompositionLayerProjection& newLayer = _patchedProjLayers.back();

                const size_t firstView = _patchedProjViews.size();
                for (uint32_t v = 0; v < projLayer->viewCount; ++v) {
                    _patchedProjViews.push_back(projLayer->views[v]);
                    XrCompositionLayerProjectionView& view = _patchedProjViews.back();

                    if (v >= _originalViewFovs.size())
                        continue;

                    const XrFovf orig = _originalViewFovs[v];
                    const XrFovf wide = widenFov(orig);
                    // Only patch views rendered with the FOV we handed out;
                    // anything else passes through untouched.
                    if (!fovNearEqual(view.fov, wide))
                        continue;

                    const XrRect2Di wideRect = view.subImage.imageRect;
                    const XrRect2Di croppedRect = computeCenterCrop(wideRect, wide, orig);
                    view.subImage.imageRect = croppedRect;
                    view.fov = orig;
                    patchDepthInfo(view, wide, orig);
                    if (!_overscanSubmissionLogged) {
                        Log("Recording overscan headset crop: view %u rect %d,%d %dx%d -> %d,%d %dx%d; "
                            "wide FOV %.6f,%.6f,%.6f,%.6f -> runtime FOV %.6f,%.6f,%.6f,%.6f\n",
                            v,
                            wideRect.offset.x,
                            wideRect.offset.y,
                            wideRect.extent.width,
                            wideRect.extent.height,
                            croppedRect.offset.x,
                            croppedRect.offset.y,
                            croppedRect.extent.width,
                            croppedRect.extent.height,
                            wide.angleLeft,
                            wide.angleRight,
                            wide.angleUp,
                            wide.angleDown,
                            orig.angleLeft,
                            orig.angleRight,
                            orig.angleUp,
                            orig.angleDown);
                        _overscanSubmissionLogged = true;
                    }
                    anyPatched = true;
                }
                newLayer.views = &_patchedProjViews[firstView];
                _patchedLayerPtrs.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&newLayer));
            }

            if (!anyPatched)
                return false;

            patched = *frameEndInfo;
            patched.layers = _patchedLayerPtrs.data();
            return true;
        }

        // Create the mirror on the adapter the game renders with; shared
        // resources cannot be opened across adapters on hybrid-GPU systems.
        void ensureMirror() {
            if (_mirror)
                return;

            ComPtr<IDXGIAdapter> adapter;
            if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR && _d3d11Device) {
                ComPtr<IDXGIDevice> dxgiDevice;
                if (SUCCEEDED(_d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                    dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf());
                }
            } else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR && _d3d12Device) {
                const LUID luid = _d3d12Device->GetAdapterLuid();
                ComPtr<IDXGIFactory4> factory;
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                    factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf()));
                }
            }

            _mirror = std::make_unique<D3D11Mirror>(adapter.Get());
            if (!_mirror->initialized()) {
                Log("Mirror initialization failed; OBS mirroring disabled\n");
            } else {
                _mirror->setApplicationInfo(GetApplicationName().c_str());
            }
        }

        bool isSystemHandled(XrSystemId systemId) const {
            return systemId == _systemId;
        }

        bool isSessionHandled(XrSession session) const {
            return _sessions.find(session) != _sessions.cend();
        }

        bool isSwapchainHandled(XrSwapchain swapchain) const {
            return _swapchains.find(swapchain) != _swapchains.cend();
        }

        std::unique_ptr<D3D11Mirror> _mirror;

        UINT64 _currentFenceValue{0};

        XrStructureType _xrGraphicsAPI = XR_TYPE_UNKNOWN;

        ID3D11Device* _d3d11Device = nullptr;
        ComPtr<ID3D11DeviceContext> _d3d11Context = nullptr;

        ID3D12Device* _d3d12Device = nullptr;
        ID3D12CommandQueue* _d3d12CommandQueue = nullptr;

        XrSystemId _systemId{XR_NULL_SYSTEM_ID};

        std::vector<XrViewConfigurationView> _xrViewsList{};
        std::vector<XrCompositionLayerProjectionView> _projectionViews{};

        std::map<XrSession, Session> _sessions;
        std::map<XrSwapchain, Swapchain> _swapchains;

        // Mirror pipeline diagnostics: last classified frame outcome plus
        // throttling counters for the transition log.
        MirrorOutcome _lastMirrorOutcome = MirrorOutcome::Unset;
        uint32_t _mirrorOutcomeFrames = 0;
        uint32_t _mirrorOutcomeTransitionLogs = 0;
        ULONGLONG _lastMirrorHealthLogTick = 0;
        bool _untrackedViewSpaceLogged = false;

        // Recording overscan state (experimental, latched at instance creation).
        bool _overscanRequested = false;
        float _overscanDesiredH = 1.0f;
        float _overscanDesiredV = 1.0f;
        float _overscanHScale = 1.0f;
        float _overscanVScale = 1.0f;
        bool _overscanScalesComputed = false;
        bool _depthPatchWarned = false;
        bool _overscanSubmissionLogged = false;
        std::vector<XrFovf> _originalViewFovs;
        // Recording-only OpenXR quad-layer visibility. Polled periodically so
        // Control Center changes apply live without touching headset submission.
        bool _mirrorQuadLayers = true;
        bool _quadLayerConfigInitialized = false;
        ULONGLONG _lastQuadLayerConfigCheckTick = 0;
        // Per-frame scratch for the patched runtime submission.
        std::vector<const XrCompositionLayerBaseHeader*> _patchedLayerPtrs;
        std::vector<XrCompositionLayerProjection> _patchedProjLayers;
        std::vector<XrCompositionLayerProjectionView> _patchedProjViews;
        std::vector<XrCompositionLayerDepthInfoKHR> _patchedDepthInfos;
    };

} // namespace

namespace layer_OBSMirror {
    // Defined in dispatch.gen.cpp; ResetInstance() clears it on
    // xrDestroyInstance so the layer (and its mirror) is destroyed with the
    // instance instead of lingering for the life of the process.
    extern std::unique_ptr<OpenXrApi> g_instance;

    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions;
} // namespace layer_OBSMirror

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(layer_OBSMirror::log::g_traceProvider);
        break;

    case DLL_PROCESS_DETACH:
        TraceLoggingUnregister(layer_OBSMirror::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
