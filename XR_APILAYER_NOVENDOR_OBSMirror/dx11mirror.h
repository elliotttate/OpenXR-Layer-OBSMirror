#pragma once
#include "pch.h"
#include "obs_mirror_ipc.h"
#include "dxgi_format_info.h"
#include <DirectXMath.h>
#include <d3d11_4.h>
#include <map>
#include <vector>

namespace Mirror
{
    // Shared with the OBS plugin so both binaries make identical format choices.
    using obs_mirror_ipc::DxgiFormatInfo;
    using obs_mirror_ipc::GetFormatInfo;

    class D3D11Mirror {
      public:
        explicit D3D11Mirror(IDXGIAdapter* adapter = nullptr);
        ~D3D11Mirror();

        D3D11Mirror(const D3D11Mirror&) = delete;
        D3D11Mirror& operator=(const D3D11Mirror&) = delete;

        /// False when any part of initialization failed. Every entry point is a
        /// no-op in that state so a broken mirror can never crash the host game.
        bool initialized() const;

        /// Share a game-device D3D11 texture (created with a keyed mutex) with
        /// the mirror device.
        void createSharedMirrorTexture(const XrSwapchain& swapchain, const ComPtr<ID3D11Texture2D>& tex);

        /// Share a game D3D12 texture via its NT handle. fenceHandle (optional)
        /// is a shared fence the game's queue signals when its copy completes.
        void createSharedMirrorTexture(const XrSwapchain& swapchain,
                                       const HANDLE& textureHandle,
                                       const HANDLE& fenceHandle);

        void removeSwapchain(const XrSwapchain swapchain);

        /// Fence value the game's D3D12 queue will signal once its copy into
        /// the shared texture for this swapchain has completed.
        void notifyFenceValue(const XrSwapchain swapchain, const UINT64 value);

        bool enabled() const;

        void flush();

        void addSpace(const XrSpace space, const XrReferenceSpaceCreateInfo* createInfo);

        void removeSpace(const XrSpace space);

        void clearSpaces();

        const XrReferenceSpaceCreateInfo* getSpaceInfo(const XrSpace space) const;

        void Blend(const XrCompositionLayerProjectionView* view,
                   const XrFovf& hmdFov,
                   const XrCompositionLayerQuad* quad,
                   const DXGI_FORMAT format,
                   const XrSpace space,
                   const XrTime displayTime);

        /// The eye Blend overloads and copyPerspectiveTex return false only on
        /// structural failures (source never registered, ring creation failed)
        /// so xrEndFrame can report an honest mirror state. Transient skips
        /// such as keyed-mutex timeouts still return true.
        bool Blend(const XrCompositionLayerProjectionView* view,
                   const XrFovf& hmdFov,
                   const DXGI_FORMAT format,
                   const XrSpace space,
                   const XrTime displayTime);

        bool Blend(const XrCompositionLayerProjectionView* view1,
                   const XrFovf& hmdFov1,
                   const XrCompositionLayerProjectionView* view2,
                   const XrFovf& hmdFov2,
                   const DXGI_FORMAT format,
                   const XrSpace viewSpace,
                   const XrTime displayTime);

        bool copyPerspectiveTex(const XrRect2Di& imgRect,
                                const uint32_t arraySlice,
                                const DXGI_FORMAT format,
                                const XrSwapchain& swapchain);

        void copyToMirror();

        void checkOBSRunning();

        uint32_t getEyeIndex() const;

        /// Records the game's name in the shared diagnostics block so the OBS
        /// log can identify which application is (or is not) feeding frames.
        void setApplicationInfo(const char* applicationName);

        /// Publishes the runtime's recommended per-eye render size before
        /// recording overscan scales it, so the Control Center can work out
        /// what shape the recording will be at a given overscan setting.
        void setBaseViewSize(uint32_t width, uint32_t height);

      private:
        struct SourceData {
            ComPtr<ID3D11Texture2D> _texture = nullptr;
            ComPtr<ID3D11ShaderResourceView> _quadTextureView = nullptr;
            ComPtr<IDXGIKeyedMutex> _keyedMutex = nullptr; // D3D11 sessions only
            ComPtr<ID3D11Fence> _copyFence = nullptr;      // D3D12 sessions only
            UINT64 _copyFenceValue = 0;
            bool _isArray = false;
        };

        struct UVRect {
            float startX;
            float endX;
            float startY;
            float endY;
        };

        bool createMirrorSurface();

        /// Fills the layer's half of the shared diagnostics block (no-op when
        /// attached to a legacy-sized surface).
        void publishLayerDiagnostics();

        /// Logs which consumers (OBS plugin, Control Center preview) have
        /// stamped the diagnostics block, and raises a GPU-mismatch error when
        /// OBS renders on a different adapter.
        void logConsumerDetails();

        /// Tracks consecutive keyed-mutex acquire failures so a persistent
        /// stall is reported once instead of silently dropping every frame.
        void noteSourceAcquire(bool acquired);

        bool createSourceView(SourceData& srcData);

        /// True when the OBS-side camera smoothing sliders request smoothing
        /// and there is crop margin to pan within.
        bool smoothingActive() const;

        /// Advances the smoothed-camera filter for this display time (the
        /// second eye of a frame reuses the cached delta) and stores the
        /// clamped camera offset relative to the rendered pose.
        void computeSmoothedDelta(const XrTime displayTime, const XrPosef& pose, const XrFovf& fov);

        /// Reprojects one eye image from the smoothed camera with a slight
        /// tan-space crop, absorbing high-frequency head motion.
        void drawSmoothedEye(const XrCompositionLayerProjectionView* view,
                             const XrFovf& hmdFov,
                             const SourceData& src,
                             const XrRect2Di& targetRect,
                             const bool seamBlend,
                             const float alphaOverride,
                             const XrTime displayTime);

        static XrFovf scaleFovTan(const XrFovf& fov, const float scale);

        /// GPU-side wait for the game's copy into the shared texture before
        /// the mirror device reads from it.
        void syncToSource(const SourceData& srcData);

        void checkCopyTex(const uint32_t width, const uint32_t height, const DXGI_FORMAT format);

        void checkFOVs(const XrFovf& hmdFov, const XrFovf& viewFov);

        UVRect writeQuadUVs(const XrRect2Di& imgRect, const D3D11_TEXTURE2D_DESC& srcDesc);

        void writeBlendConstants(float blendStartX,
                                 float blendEndX,
                                 float texIndex,
                                 float alphaOverride);

        void bindQuadPipeline(const SourceData& srcData);

        void setTargetRect(const XrRect2Di& rect);

        void drawOrthoQuad();

        ComPtr<ID3D11Device> _d3d11MirrorDevice = nullptr;
        ComPtr<ID3D11DeviceContext> _d3d11MirrorContext = nullptr;
        ComPtr<ID3D11DeviceContext4> _d3d11MirrorContext4 = nullptr;

        /// Signalled with the frame counter after each copy into the mirror
        /// ring; lets us publish only frames whose copy has finished.
        ComPtr<ID3D11Fence> _obsCopyFence = nullptr;

        std::map<XrSwapchain, SourceData> _sourceData;
        obs_mirror_ipc::MirrorSurfaceData* _pMirrorSurfaceData = nullptr;
        // Null when attached to a legacy 64-byte surface from an older layer.
        obs_mirror_ipc::MirrorDiagnostics* _diag = nullptr;
        HANDLE _mapFile = nullptr;
        LUID _adapterLuid{};

        std::map<XrSpace, XrReferenceSpaceCreateInfo> _spaceInfo;

        ComPtr<ID3D11RenderTargetView> _targetView = nullptr;

        ComPtr<ID3D11VertexShader> _quadVShader = nullptr;
        ComPtr<ID3D11PixelShader> _quadPShader = nullptr;
        ComPtr<ID3D11PixelShader> _quadArrayPShader = nullptr;
        ComPtr<ID3D11InputLayout> _quadShaderLayout = nullptr;
        ComPtr<ID3D11Buffer> _quadConstantBuffer = nullptr;
        ComPtr<ID3D11Buffer> _quadConstantBlendBuffer = nullptr;
        ComPtr<ID3D11Buffer> _quadVertexBuffer = nullptr;
        ComPtr<ID3D11Buffer> _quadIndexBuffer = nullptr;
        ComPtr<ID3D11SamplerState> _quadSampleState = nullptr;
        ComPtr<ID3D11BlendState> _quadBlendState = nullptr;

        ComPtr<ID3D11Texture2D> _compositorTexture = nullptr;
        D3D11_TEXTURE2D_DESC _comp_desc{};
        std::vector<ComPtr<ID3D11Texture2D>> _mirrorTextures;

        uint32_t _frameCounter = 0;
        uint32_t _obsFrameCounter = 10;
        uint32_t _lastOBSFrameNumber = 0;
        bool _obsRunning = false;
        bool _initialized = false;
        uint32_t _acquireTimeoutStreak = 0;
        bool _acquireTimeoutWarned = false;
        // Non-zero while mirror ring creation is failing; retries (and their
        // log output) are limited to once per second instead of every frame.
        ULONGLONG _lastRingBuildFailTick = 0;

        float _fovVertRatio = 1.f;
        float _fovHorizRatio = 1.f;
        // Ratios last written to the log; zero until the first rescale so any
        // real scale is reported once.
        float _loggedVertRatio = 0.f;
        float _loggedHorizRatio = 0.f;
        XrFovf _hmdFov{0.0f, 0.0f, 0.0f, 0.0f};
        XrFovf _viewFov{0.0f, 0.0f, 0.0f, 0.0f};

        // Camera smoothing filter state (single virtual camera shared by both
        // eyes so stereo output stays fused).
        XrTime _smoothLastTime = 0;
        XrTime _smoothRelTime = 0;
        bool _smoothValid = false;
        DirectX::XMFLOAT4 _smoothQuat{0.0f, 0.0f, 0.0f, 1.0f};
        DirectX::XMFLOAT3 _smoothPos{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT4 _smoothRelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        DirectX::XMFLOAT3 _smoothRelPos{0.0f, 0.0f, 0.0f};

    };
}
