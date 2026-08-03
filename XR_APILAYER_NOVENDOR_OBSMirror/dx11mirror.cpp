#include "pch.h"
#include "dx11mirror.h"
#include "log.h"
#include "util.h"
#include "layer.h"
#include "obs_mirror_ipc.h"

#include <directxmath.h> // Matrix math functions and objects
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <xr_linear.h>
#include <algorithm>
#include <new>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")

namespace {
#define CHECK_DX(expression)                                                                                           \
    do {                                                                                                               \
        HRESULT res = (expression);                                                                                    \
        if (FAILED(res)) {                                                                                             \
            Log("DX Call failed with: 0x%08x\n", res);                                                                 \
            Log("CHECK_DX failed on: " #expression " DirectX error - see log for details\n");                         \
        }                                                                                                              \
    } while (0);

    // How long to wait for the game device to release a shared texture before
    // skipping this frame's mirror update instead of stalling the render thread.
    constexpr DWORD kAcquireTimeoutMs = 8;

    // Camera smoothing: distance of the virtual reprojection plane (positional
    // smoothing error shrinks with distance) and the fraction of the crop
    // margin the smoothed camera may consume before being clamped back.
    constexpr float kSmoothingPlaneDistance = 2.0f;
    constexpr float kSmoothingMarginSafety = 0.85f;

    // RAII acquire of an optional keyed mutex. Passes through when the
    // resource has no keyed mutex (D3D12-created textures, older resources).
    class ScopedKeyedMutex {
      public:
        explicit ScopedKeyedMutex(IDXGIKeyedMutex* mutex) : _mutex(mutex) {
            // AcquireSync reports timeouts through a success HRESULT, so
            // compare against S_OK rather than using SUCCEEDED().
            _acquired = !_mutex || _mutex->AcquireSync(0, kAcquireTimeoutMs) == S_OK;
        }
        ~ScopedKeyedMutex() {
            if (_mutex && _acquired)
                _mutex->ReleaseSync(0);
        }
        ScopedKeyedMutex(const ScopedKeyedMutex&) = delete;
        ScopedKeyedMutex& operator=(const ScopedKeyedMutex&) = delete;

        bool acquired() const {
            return _acquired;
        }

      private:
        IDXGIKeyedMutex* _mutex;
        bool _acquired;
    };
} // namespace

namespace Mirror {
    using namespace layer_OBSMirror::log;
    using namespace DirectX; // Matrix math
    using obs_mirror_ipc::kMirrorTextureCount;

    XMMATRIX d3dXrProjection(XrFovf fov, float clip_near, float clip_far) {
        const float left = clip_near * tanf(fov.angleLeft);
        const float right = clip_near * tanf(fov.angleRight);
        const float down = clip_near * tanf(fov.angleDown);
        const float up = clip_near * tanf(fov.angleUp);

        return XMMatrixPerspectiveOffCenterRH(left, right, down, up, clip_near, clip_far);
    }

    struct quad_transform_buffer_t {
        XMFLOAT4X4 world;
        XMFLOAT4X4 viewproj;
    };

    // Matches the PSConstants cbuffer used by both pixel shaders.
    struct quad_blend_buffer_t {
        float blendStartX;
        float blendEndX;
        float texIndex; // Ignored by the non-array shader
        float alphaOverride;
    };

    constexpr char quad_vs_code[] = R"_(
cbuffer TransformBuffer : register(b0) {
	float4x4 world;
	float4x4 viewproj;
};

struct vsIn {
	float4 pos  : POSITION;
	float2 tex  : TEXCOORD0;
};

struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

psIn vs_quad(vsIn input)
{
	psIn output;
	output.pos = mul(mul(input.pos, world), viewproj);
	output.tex = input.tex;
	return output;
}

)_";

    constexpr char quad_ps_code[] = R"_(

cbuffer PSConstants : register(b1) // Use a different register for PS constants
{
    // Blend starts at blendStartX (0=left) and ends at blendEndX (1=right)
    // in normalized texture coordinates (UV space) of the quad.
    float blendStartX;
    float blendEndX;
    float texIndex; // Unused here; keeps the layout shared with the array shader
    float alphaOverride;
};

Texture2D shaderTexture : register(t0);
SamplerState SampleType : register(s0);


struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

float4 ps_quad(psIn inputPS) : SV_TARGET
{
	float4 textureColor = shaderTexture.Sample(SampleType, inputPS.tex);

    // Calculate the horizontal blend factor based on texture coordinate x
    // smoothstep provides a nice S-curve interpolation between the start and end points.
    // input.Tex.x ranges from 0.0 (left edge of quad) to 1.0 (right edge of quad)
    float horizontalBlend = smoothstep(blendStartX, blendEndX, inputPS.tex.x);

    // Modulate the texture's alpha component by the calculated horizontal blend factor.
    // The blend state will then use this resulting alpha.
    textureColor.a = (1.0 - alphaOverride) * horizontalBlend * textureColor.a + alphaOverride * horizontalBlend;

	return textureColor;
}
)_";

    constexpr char quad_array_ps_code[] = R"_(

cbuffer PSConstants : register(b1) // Use a different register for PS constants
{
    // Blend starts at blendStartX (0=left) and ends at blendEndX (1=right)
    // in normalized texture coordinates (UV space) of the quad.
    float blendStartX;
    float blendEndX;
    float texIndex;
    float alphaOverride;
};

Texture2DArray shaderTexture : register(t0);
SamplerState SampleType : register(s0);

struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

float4 ps_quad(psIn inputPS) : SV_TARGET
{
    // Combine UV coords with the desired array slice index
    float3 sampleCoord = float3(inputPS.tex.x, inputPS.tex.y, texIndex);

	float4 textureColor = shaderTexture.Sample(SampleType, sampleCoord);

    // Calculate the horizontal blend factor based on texture coordinate x
    // smoothstep provides a nice S-curve interpolation between the start and end points.
    // input.Tex.x ranges from 0.0 (left edge of quad) to 1.0 (right edge of quad)
    float horizontalBlend = smoothstep(blendStartX, blendEndX, inputPS.tex.x);

    // Modulate the texture's alpha component by the calculated horizontal blend factor.
    // The blend state will then use this resulting alpha.
    textureColor.a = (1.0 - alphaOverride) * horizontalBlend * textureColor.a + alphaOverride * horizontalBlend;

	return textureColor;
}
)_";

    float quad_verts[] = {
        // coord x,y,z,w  tex x,y,
        -0.5,  0.5, 0, 1,   0, 0,
        -0.5, -0.5, 0, 1,   0, 1,
         0.5,  0.5, 0, 1,   1, 0,
         0.5, -0.5, 0, 1,   1, 1};

    uint16_t quad_inds[] = {2, 1, 0,
                            2, 3, 1};

    ComPtr<ID3DBlob> d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target) {
        DWORD flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> compiled;
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(hlsl,
                                          strlen(hlsl),
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          entrypoint,
                                          target,
                                          flags,
                                          0,
                                          compiled.GetAddressOf(),
                                          errors.GetAddressOf());
        if (FAILED(result)) {
            Log("Error: D3DCompile failed (0x%08x)%s%s\n",
                result,
                errors ? ": " : "",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return nullptr;
        }

        return compiled;
    }

    using obs_mirror_ipc::MirrorSurfaceData;

    D3D11Mirror::D3D11Mirror(IDXGIAdapter* adapter) {
        UINT creationFlags =
#ifdef _DEBUG
            D3D11_CREATE_DEVICE_DEBUG |
#endif
            D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        // Create the mirror device on the same adapter as the game device so
        // shared resources can be opened (hybrid-GPU systems render on either).
        HRESULT hr = D3D11CreateDevice(adapter,
                                       adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                       0,
                                       creationFlags,
                                       nullptr,
                                       0,
                                       D3D11_SDK_VERSION,
                                       _d3d11MirrorDevice.ReleaseAndGetAddressOf(),
                                       nullptr,
                                       _d3d11MirrorContext.ReleaseAndGetAddressOf());
        if (FAILED(hr) && adapter) {
            Log("init: D3D11CreateDevice on the game adapter failed (0x%08x), falling back to default\n", hr);
            hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   0,
                                   creationFlags,
                                   nullptr,
                                   0,
                                   D3D11_SDK_VERSION,
                                   _d3d11MirrorDevice.ReleaseAndGetAddressOf(),
                                   nullptr,
                                   _d3d11MirrorContext.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) {
            Log("init: D3D11CreateDevice failed (0x%08x)\n", hr);
            return;
        }

        Log("init: D3D11CreateDevice created\n");

        // Optional: fences let us publish only fully-copied frames to OBS and
        // wait for D3D12 game copies. Unavailable before Windows 10 1703.
        ComPtr<ID3D11Device5> device5;
        if (SUCCEEDED(_d3d11MirrorDevice.As(&device5)) && SUCCEEDED(_d3d11MirrorContext.As(&_d3d11MirrorContext4))) {
            if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(_obsCopyFence.ReleaseAndGetAddressOf())))) {
                _obsCopyFence = nullptr;
            }
        }

        const auto vShaderBlob = d3d_compile_shader(quad_vs_code, "vs_quad", "vs_5_0");
        const auto pShaderBlob = d3d_compile_shader(quad_ps_code, "ps_quad", "ps_5_0");
        const auto psArrayBlob = d3d_compile_shader(quad_array_ps_code, "ps_quad", "ps_5_0");
        if (!vShaderBlob || !pShaderBlob || !psArrayBlob) {
            Log("init: shader compilation failed\n");
            return;
        }
        CHECK_DX(_d3d11MirrorDevice->CreateVertexShader(vShaderBlob->GetBufferPointer(),
                                                        vShaderBlob->GetBufferSize(),
                                                        nullptr,
                                                        _quadVShader.ReleaseAndGetAddressOf()));
        CHECK_DX(_d3d11MirrorDevice->CreatePixelShader(pShaderBlob->GetBufferPointer(),
                                                       pShaderBlob->GetBufferSize(),
                                                       nullptr,
                                                       _quadPShader.ReleaseAndGetAddressOf()));
        CHECK_DX(_d3d11MirrorDevice->CreatePixelShader(psArrayBlob->GetBufferPointer(),
                                                       psArrayBlob->GetBufferSize(),
                                                       nullptr,
                                                       _quadArrayPShader.ReleaseAndGetAddressOf()));

        D3D11_INPUT_ELEMENT_DESC q_vert_desc[] = {
            {"POSITION",
                0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA,
                0},
            {"TEXCOORD",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA,
                0},
        };
        CHECK_DX(_d3d11MirrorDevice->CreateInputLayout(q_vert_desc,
                                                        (UINT)_countof(q_vert_desc),
                                                        vShaderBlob->GetBufferPointer(),
                                                        vShaderBlob->GetBufferSize(),
                                                        _quadShaderLayout.ReleaseAndGetAddressOf()));

        D3D11_SUBRESOURCE_DATA qVertBufferData = {quad_verts};
        D3D11_SUBRESOURCE_DATA qIndBufferData = {quad_inds};
        CD3D11_BUFFER_DESC qVertBufferDesc(
            sizeof(quad_verts), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
        CD3D11_BUFFER_DESC qIndBufferDesc(sizeof(quad_inds), D3D11_BIND_INDEX_BUFFER);
        CD3D11_BUFFER_DESC qConstBufferDesc(sizeof(quad_transform_buffer_t), D3D11_BIND_CONSTANT_BUFFER);
        CHECK_DX(_d3d11MirrorDevice->CreateBuffer(
            &qVertBufferDesc, &qVertBufferData, _quadVertexBuffer.ReleaseAndGetAddressOf()));
        CHECK_DX(_d3d11MirrorDevice->CreateBuffer(
            &qIndBufferDesc, &qIndBufferData, _quadIndexBuffer.ReleaseAndGetAddressOf()));
        CHECK_DX(_d3d11MirrorDevice->CreateBuffer(
            &qConstBufferDesc, nullptr, _quadConstantBuffer.ReleaseAndGetAddressOf()));

        CD3D11_BUFFER_DESC qConstBlendBufferDesc(
            sizeof(quad_blend_buffer_t), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
        CHECK_DX(_d3d11MirrorDevice->CreateBuffer(
            &qConstBlendBufferDesc, nullptr, _quadConstantBlendBuffer.ReleaseAndGetAddressOf()));

        // Create a texture sampler state description.
        D3D11_SAMPLER_DESC samplerDesc;
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.BorderColor[0] = 1.0f;
        samplerDesc.BorderColor[1] = 1.0f;
        samplerDesc.BorderColor[2] = 1.0f;
        samplerDesc.BorderColor[3] = 1.0f;
        samplerDesc.MinLOD = -FLT_MAX;
        samplerDesc.MaxLOD = FLT_MAX;

        // Create the texture sampler state.
        CHECK_DX(_d3d11MirrorDevice->CreateSamplerState(&samplerDesc, _quadSampleState.ReleaseAndGetAddressOf()));

        D3D11_BLEND_DESC blendDesc;
        ZeroMemory(&blendDesc, sizeof(D3D11_BLEND_DESC));

        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        CHECK_DX(_d3d11MirrorDevice->CreateBlendState(&blendDesc, _quadBlendState.ReleaseAndGetAddressOf()));

        if (!_quadVShader || !_quadPShader || !_quadArrayPShader || !_quadShaderLayout || !_quadVertexBuffer ||
            !_quadIndexBuffer || !_quadConstantBuffer || !_quadConstantBlendBuffer || !_quadSampleState ||
            !_quadBlendState) {
            Log("init: D3D resource creation failed, mirror disabled\n");
            return;
        }

        _d3d11MirrorContext->VSSetConstantBuffers(0, 1, _quadConstantBuffer.GetAddressOf());
        _d3d11MirrorContext->VSSetShader(_quadVShader.Get(), nullptr, 0);

        UINT strides[4] = {sizeof(float) * 6, sizeof(float) * 6, sizeof(float) * 6, sizeof(float) * 6};
        UINT offsets[4] = {0, 0, 0, 0};
        _d3d11MirrorContext->IASetVertexBuffers(0, 1, _quadVertexBuffer.GetAddressOf(), strides, offsets);
        _d3d11MirrorContext->IASetIndexBuffer(_quadIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
        _d3d11MirrorContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _d3d11MirrorContext->IASetInputLayout(_quadShaderLayout.Get());

        if (!createMirrorSurface())
            return;

        _initialized = true;
    }

    D3D11Mirror::~D3D11Mirror() {
        if (_pMirrorSurfaceData) {
            Log("Unmapping file\n");
            _pMirrorSurfaceData->reset();
            UnmapViewOfFile(_pMirrorSurfaceData);
            _pMirrorSurfaceData = nullptr;
        }
        if (_mapFile) {
            CloseHandle(_mapFile);
            _mapFile = nullptr;
        }
    }

    bool D3D11Mirror::initialized() const {
        return _initialized;
    }

    bool D3D11Mirror::createSourceView(SourceData& srcData) {
        D3D11_TEXTURE2D_DESC srcDesc;
        srcData._texture->GetDesc(&srcDesc);

        Log("Creating shared mirror texture: W: %u H: %u Array: %u Format: %d\n",
            srcDesc.Width,
            srcDesc.Height,
            srcDesc.ArraySize,
            srcDesc.Format);

        // Prefer the gamma-correct view (sRGB for 8-bit formats) so sampling
        // into the sRGB compositor target round-trips correctly. Fully-typed
        // resources that reject the cast fall back to their own format.
        DXGI_FORMAT preferredFormat = srcDesc.Format;
        DxgiFormatInfo info = {};
        if (GetFormatInfo(srcDesc.Format, info)) {
            preferredFormat = info.bpc > 8 ? info.linear : info.srgb;
        } else {
            Log("Unknown DXGI texture format %d\n", srcDesc.Format);
        }

        srcData._isArray = srcDesc.ArraySize > 1;

        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        viewDesc.Format = preferredFormat;
        if (!srcData._isArray) {
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
            viewDesc.Texture2D.MostDetailedMip = 0;
        } else {
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.ArraySize = srcDesc.ArraySize;
            viewDesc.Texture2DArray.FirstArraySlice = 0;
            viewDesc.Texture2DArray.MipLevels = 1;
            viewDesc.Texture2DArray.MostDetailedMip = 0;
        }

        HRESULT hr = _d3d11MirrorDevice->CreateShaderResourceView(
            srcData._texture.Get(), &viewDesc, srcData._quadTextureView.ReleaseAndGetAddressOf());
        if (FAILED(hr) && viewDesc.Format != srcDesc.Format) {
            viewDesc.Format = srcDesc.Format;
            hr = _d3d11MirrorDevice->CreateShaderResourceView(
                srcData._texture.Get(), &viewDesc, srcData._quadTextureView.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) {
            Log("CreateShaderResourceView failed (0x%08x) for format %d\n", hr, srcDesc.Format);
            return false;
        }
        return true;
    }

    void D3D11Mirror::createSharedMirrorTexture(const XrSwapchain& swapchain, const ComPtr<ID3D11Texture2D>& tex) {
        if (!_initialized || !tex)
            return;

        SourceData srcData;

        ComPtr<IDXGIResource> pOtherResource;
        CHECK_DX(tex->QueryInterface(IID_PPV_ARGS(&pOtherResource)));
        if (!pOtherResource)
            return;

        HANDLE sharedHandle = nullptr;
        if (FAILED(pOtherResource->GetSharedHandle(&sharedHandle)) || !sharedHandle)
            return;

        ComPtr<IDXGIResource> openedResource;
        CHECK_DX(_d3d11MirrorDevice->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&openedResource)));
        if (!openedResource)
            return;

        CHECK_DX(openedResource->QueryInterface(IID_PPV_ARGS(&srcData._texture)));
        if (!srcData._texture)
            return;

        // The layer-side copy texture is created with a keyed mutex; both
        // devices must acquire it around access. Absence is tolerated.
        srcData._texture.As(&srcData._keyedMutex);

        if (!createSourceView(srcData))
            return;

        _sourceData[swapchain] = std::move(srcData);
    }

    void D3D11Mirror::createSharedMirrorTexture(const XrSwapchain& swapchain,
                                                const HANDLE& textureHandle,
                                                const HANDLE& fenceHandle) {
        if (!_initialized || !textureHandle)
            return;

        SourceData srcData;

        ComPtr<ID3D11Device1> pDevice;
        CHECK_DX(_d3d11MirrorDevice->QueryInterface(IID_PPV_ARGS(&pDevice)));
        if (!pDevice)
            return;
        CHECK_DX(pDevice->OpenSharedResource1(textureHandle, IID_PPV_ARGS(&srcData._texture)));
        if (!srcData._texture)
            return;

        // The game's D3D12 queue signals this fence when its copy into the
        // shared texture completes; we GPU-wait on it before sampling.
        if (fenceHandle) {
            ComPtr<ID3D11Device5> device5;
            if (SUCCEEDED(_d3d11MirrorDevice.As(&device5))) {
                if (FAILED(device5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&srcData._copyFence)))) {
                    Log("OpenSharedFence failed; mirroring without copy synchronization\n");
                    srcData._copyFence = nullptr;
                }
            }
        }

        if (!createSourceView(srcData))
            return;

        _sourceData[swapchain] = std::move(srcData);
    }

    void D3D11Mirror::removeSwapchain(const XrSwapchain swapchain) {
        _sourceData.erase(swapchain);
    }

    void D3D11Mirror::notifyFenceValue(const XrSwapchain swapchain, const UINT64 value) {
        auto it = _sourceData.find(swapchain);
        if (it != _sourceData.end())
            it->second._copyFenceValue = value;
    }

    void D3D11Mirror::syncToSource(const SourceData& srcData) {
        // GPU-side wait; does not block the CPU.
        if (srcData._copyFence && _d3d11MirrorContext4)
            _d3d11MirrorContext4->Wait(srcData._copyFence.Get(), srcData._copyFenceValue);
    }

    XrFovf D3D11Mirror::scaleFovTan(const XrFovf& fov, const float scale) {
        XrFovf out;
        out.angleLeft = atanf(tanf(fov.angleLeft) * scale);
        out.angleRight = atanf(tanf(fov.angleRight) * scale);
        out.angleUp = atanf(tanf(fov.angleUp) * scale);
        out.angleDown = atanf(tanf(fov.angleDown) * scale);
        return out;
    }

    bool D3D11Mirror::smoothingActive() const {
        // Without crop margin there is no room to pan, so smoothing is a no-op;
        // fall back to the cheaper unsmoothed paths.
        return _initialized && _pMirrorSurfaceData->smoothing > 0.5f && _pMirrorSurfaceData->smoothCrop > 0.5f;
    }

    void D3D11Mirror::computeSmoothedDelta(const XrTime displayTime, const XrPosef& pose, const XrFovf& fov) {
        if (_smoothRelTime == displayTime)
            return; // The second eye of this frame reuses the same delta.
        _smoothRelTime = displayTime;

        XMVECTOR qA = XMLoadFloat4((XMFLOAT4*)&pose.orientation);
        const XMVECTOR pA = XMLoadFloat3((XMFLOAT3*)&pose.position);

        // Some games submit zeroed poses; pass through rather than reproject
        // with garbage.
        if (XMVectorGetX(XMVector4LengthSq(qA)) < 0.25f) {
            _smoothRelQuat = {0.0f, 0.0f, 0.0f, 1.0f};
            _smoothRelPos = {0.0f, 0.0f, 0.0f};
            _smoothValid = false;
            return;
        }
        qA = XMQuaternionNormalize(qA);

        const float strength = std::clamp(_pMirrorSurfaceData->smoothing, 0.0f, 100.0f) / 100.0f;
        // Filter time constant from ~40 ms (subtle) to ~800 ms (very floaty);
        // quadratic mapping gives the slider a useful low end.
        const float tau = 0.04f + strength * strength * 0.76f;

        const float dt = _smoothValid
                             ? std::clamp(static_cast<float>(displayTime - _smoothLastTime) * 1e-9f, 0.0f, 0.1f)
                             : 0.0f;
        _smoothLastTime = displayTime;

        XMVECTOR qS = XMLoadFloat4(&_smoothQuat);
        XMVECTOR pS = XMLoadFloat3(&_smoothPos);

        bool snap = !_smoothValid;
        if (!snap) {
            // Snap turns, teleports and reference-space changes should be
            // followed instantly instead of sweeping the camera across them.
            const float dot = std::min(fabsf(XMVectorGetX(XMQuaternionDot(qS, qA))), 1.0f);
            const float angle = 2.0f * acosf(dot);
            const float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(pA, pS)));
            snap = angle > XMConvertToRadians(25.0f) || dist > 0.5f;
        }

        if (snap) {
            qS = qA;
            pS = pA;
            _smoothValid = true;
        } else {
            const float alpha = 1.0f - expf(-dt / tau);
            qS = XMQuaternionNormalize(XMQuaternionSlerp(qS, qA, alpha));
            pS = XMVectorLerp(pS, pA, alpha);
        }

        // Express the smoothed camera relative to the rendered pose: S = rel * A.
        const XMVECTOR qAInv = XMQuaternionInverse(qA);
        XMVECTOR relQ = XMQuaternionMultiply(qS, qAInv);
        XMVECTOR relP = XMVector3Rotate(XMVectorSubtract(pS, pA), qAInv);

        // Clamp the offset so the cropped view can never sample outside the
        // rendered image (no black edges).
        const float cropScale = 1.0f - std::clamp(_pMirrorSurfaceData->smoothCrop, 0.0f, 25.0f) / 100.0f;
        const XrFovf cropFov = scaleFovTan(fov, cropScale);
        const float marginL = cropFov.angleLeft - fov.angleLeft;
        const float marginR = fov.angleRight - cropFov.angleRight;
        const float marginU = fov.angleUp - cropFov.angleUp;
        const float marginD = cropFov.angleDown - fov.angleDown;
        const float marginH = std::max(std::min(marginL, marginR), 0.0f) * kSmoothingMarginSafety;
        const float marginV = std::max(std::min(marginU, marginD), 0.0f) * kSmoothingMarginSafety;
        const float marginRoll = std::min(marginH, marginV) * 0.5f;

        const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), relQ);
        const XMVECTOR right = XMVector3Rotate(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), relQ);
        const float yaw = atan2f(-XMVectorGetX(fwd), -XMVectorGetZ(fwd));
        const float pitch = asinf(std::clamp(XMVectorGetY(fwd), -1.0f, 1.0f));
        const float roll = atan2f(XMVectorGetY(right), XMVectorGetX(right));

        const float clampedYaw = std::clamp(yaw, -marginH, marginH);
        const float clampedPitch = std::clamp(pitch, -marginV, marginV);
        const float clampedRoll = std::clamp(roll, -marginRoll, marginRoll);

        bool rewound = false;
        if (clampedYaw != yaw || clampedPitch != pitch || clampedRoll != roll) {
            relQ = XMQuaternionRotationRollPitchYaw(clampedPitch, clampedYaw, clampedRoll);
            rewound = true;
        }

        // The rotation uses up part of the margin; positional offset, expressed
        // at the reprojection plane distance, may consume the remainder.
        const float posLimitX = tanf(std::max(marginH - fabsf(clampedYaw), 0.0f)) * kSmoothingPlaneDistance;
        const float posLimitY = tanf(std::max(marginV - fabsf(clampedPitch), 0.0f)) * kSmoothingPlaneDistance;
        XMFLOAT3 relPf;
        XMStoreFloat3(&relPf, relP);
        const XMFLOAT3 clampedP{std::clamp(relPf.x, -posLimitX, posLimitX),
                                std::clamp(relPf.y, -posLimitY, posLimitY),
                                std::clamp(relPf.z, -0.10f, 0.10f)};
        if (clampedP.x != relPf.x || clampedP.y != relPf.y || clampedP.z != relPf.z) {
            relP = XMLoadFloat3(&clampedP);
            rewound = true;
        }

        if (rewound) {
            // Anti-windup: keep the filter state on the clamped camera so it
            // catches up instead of lagging ever further behind.
            qS = XMQuaternionNormalize(XMQuaternionMultiply(relQ, qA));
            pS = XMVectorAdd(pA, XMVector3Rotate(relP, qA));
        }

        XMStoreFloat4(&_smoothQuat, qS);
        XMStoreFloat3(&_smoothPos, pS);
        XMStoreFloat4(&_smoothRelQuat, relQ);
        XMStoreFloat3(&_smoothRelPos, relP);
    }

    void D3D11Mirror::drawSmoothedEye(const XrCompositionLayerProjectionView* view,
                                      const SourceData& src,
                                      const XrRect2Di& targetRect,
                                      const bool seamBlend,
                                      const float alphaOverride,
                                      const XrTime displayTime) {
        D3D11_TEXTURE2D_DESC srcDesc;
        src._texture->GetDesc(&srcDesc);

        syncToSource(src);
        ScopedKeyedMutex lock(src._keyedMutex.Get());
        if (!lock.acquired())
            return;

        const UVRect uv = writeQuadUVs(view->subImage.imageRect, srcDesc);

        float blendStartX = 0.0f;
        float blendEndX = 0.0f;
        if (seamBlend) {
            const float blendOffset = _pMirrorSurfaceData->blend / 2.0f;
            const float blendWidth = (uv.endX - uv.startX) / 100.0f;
            blendStartX = std::max(uv.startX, uv.startX + ((_pMirrorSurfaceData->blendPos - blendOffset) * blendWidth));
            blendEndX = std::min(uv.endX, uv.startX + ((_pMirrorSurfaceData->blendPos + blendOffset) * blendWidth));
        }
        const float texIndex = srcDesc.ArraySize > 1 ? static_cast<float>(view->subImage.imageArrayIndex) : 0.0f;
        writeBlendConstants(blendStartX, blendEndX, texIndex, alphaOverride);
        bindQuadPipeline(src);
        setTargetRect(targetRect);

        computeSmoothedDelta(displayTime, view->pose, view->fov);

        // A quad covering the rendered FOV at a fixed distance in front of the
        // pose the game rendered with.
        const float tanL = tanf(view->fov.angleLeft);
        const float tanR = tanf(view->fov.angleRight);
        const float tanU = tanf(view->fov.angleUp);
        const float tanD = tanf(view->fov.angleDown);
        const float quadW = (tanR - tanL) * kSmoothingPlaneDistance;
        const float quadH = (tanU - tanD) * kSmoothingPlaneDistance;
        const float quadCX = (tanR + tanL) * 0.5f * kSmoothingPlaneDistance;
        const float quadCY = (tanU + tanD) * 0.5f * kSmoothingPlaneDistance;

        const XMVECTOR poseQ = XMQuaternionNormalize(XMLoadFloat4((XMFLOAT4*)&view->pose.orientation));
        const XMMATRIX poseA = XMMatrixAffineTransformation(
            DirectX::g_XMOne, DirectX::g_XMZero, poseQ, XMLoadFloat3((XMFLOAT3*)&view->pose.position));

        const XMMATRIX world = XMMatrixScaling(quadW, quadH, 1.0f) *
                               XMMatrixTranslation(quadCX, quadCY, -kSmoothingPlaneDistance) * poseA;

        // Render the quad from the smoothed camera with the cropped FOV; head
        // jitter moves the crop window instead of the image.
        const XMMATRIX relM = XMMatrixAffineTransformation(
            DirectX::g_XMOne, DirectX::g_XMZero, XMLoadFloat4(&_smoothRelQuat), XMLoadFloat3(&_smoothRelPos));
        const XMMATRIX viewM = XMMatrixInverse(nullptr, relM * poseA);

        const float cropScale = 1.0f - std::clamp(_pMirrorSurfaceData->smoothCrop, 0.0f, 25.0f) / 100.0f;
        const XMMATRIX projM = d3dXrProjection(scaleFovTan(view->fov, cropScale), 0.05f, 100.0f);

        quad_transform_buffer_t transform_buffer;
        XMStoreFloat4x4(&transform_buffer.viewproj, XMMatrixTranspose(viewM * projM));
        XMStoreFloat4x4(&transform_buffer.world, XMMatrixTranspose(world));
        _d3d11MirrorContext->UpdateSubresource(_quadConstantBuffer.Get(), 0, nullptr, &transform_buffer, 0, 0);
        _d3d11MirrorContext->DrawIndexed((UINT)_countof(quad_inds), 0, 0);
    }

    bool D3D11Mirror::enabled() const {
        return _initialized && _obsRunning;
    }

    void D3D11Mirror::flush() {
        if (!_initialized)
            return;

        // Restart the smoothing filter from the live pose when it re-enables.
        if (!smoothingActive())
            _smoothValid = false;

        _d3d11MirrorContext->Flush();
        if (_targetView) {
            _d3d11MirrorContext->OMSetRenderTargets(1, _targetView.GetAddressOf(), nullptr);
            float clearRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            _d3d11MirrorContext->ClearRenderTargetView(_targetView.Get(), clearRGBA);
        }
    }

    void D3D11Mirror::addSpace(const XrSpace space, const XrReferenceSpaceCreateInfo* createInfo) {
        _spaceInfo[space] = *createInfo;
    }

    void D3D11Mirror::removeSpace(const XrSpace space) {
        _spaceInfo.erase(space);
    }

    void D3D11Mirror::clearSpaces() {
        _spaceInfo.clear();
    }

    const XrReferenceSpaceCreateInfo* D3D11Mirror::getSpaceInfo(const XrSpace space) const {
        auto it = _spaceInfo.find(space);
        if (it != _spaceInfo.end())
            return &it->second;
        else
            return nullptr;
    }

    D3D11Mirror::UVRect D3D11Mirror::writeQuadUVs(const XrRect2Di& imgRect, const D3D11_TEXTURE2D_DESC& srcDesc) {
        UVRect uv{};
        uv.startX = static_cast<float>(imgRect.offset.x) / static_cast<float>(srcDesc.Width);
        uv.endX = static_cast<float>(imgRect.offset.x + imgRect.extent.width) / static_cast<float>(srcDesc.Width);
        uv.startY = static_cast<float>(imgRect.offset.y) / static_cast<float>(srcDesc.Height);
        uv.endY = static_cast<float>(imgRect.offset.y + imgRect.extent.height) / static_cast<float>(srcDesc.Height);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK_DX(_d3d11MirrorContext->Map(_quadVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        if (!mapped.pData)
            return uv;

        float* pBuffer = static_cast<float*>(mapped.pData);
        memcpy(pBuffer, quad_verts, sizeof(quad_verts));

        const uint32_t row = 6;
        // Top left
        pBuffer[0 * row + 4] = uv.startX;
        pBuffer[0 * row + 5] = uv.startY;
        // Bottom left
        pBuffer[1 * row + 4] = uv.startX;
        pBuffer[1 * row + 5] = uv.endY;
        // Top right
        pBuffer[2 * row + 4] = uv.endX;
        pBuffer[2 * row + 5] = uv.startY;
        // Bottom right
        pBuffer[3 * row + 4] = uv.endX;
        pBuffer[3 * row + 5] = uv.endY;

        _d3d11MirrorContext->Unmap(_quadVertexBuffer.Get(), 0);
        return uv;
    }

    void D3D11Mirror::writeBlendConstants(float blendStartX, float blendEndX, float texIndex, float alphaOverride) {
        quad_blend_buffer_t psCB1;
        psCB1.blendStartX = blendStartX;
        psCB1.blendEndX = blendEndX;
        psCB1.texIndex = texIndex;
        psCB1.alphaOverride = alphaOverride;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK_DX(_d3d11MirrorContext->Map(_quadConstantBlendBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        if (!mapped.pData)
            return;
        memcpy(mapped.pData, &psCB1, sizeof(psCB1));
        _d3d11MirrorContext->Unmap(_quadConstantBlendBuffer.Get(), 0);
    }

    void D3D11Mirror::bindQuadPipeline(const SourceData& srcData) {
        _d3d11MirrorContext->PSSetConstantBuffers(1, 1, _quadConstantBlendBuffer.GetAddressOf());
        // The shader must match the view dimension or sampling returns zeros.
        _d3d11MirrorContext->PSSetShader(srcData._isArray ? _quadArrayPShader.Get() : _quadPShader.Get(), nullptr, 0);
        _d3d11MirrorContext->PSSetSamplers(0, 1, _quadSampleState.GetAddressOf());

        ID3D11ShaderResourceView* views[] = {srcData._quadTextureView.Get()};
        _d3d11MirrorContext->PSSetShaderResources(0, 1, views);

        float blend_factor[4] = {1.f, 1.f, 1.f, 1.f};
        _d3d11MirrorContext->OMSetBlendState(_quadBlendState.Get(), blend_factor, 0xffffffff);
    }

    void D3D11Mirror::setTargetRect(const XrRect2Di& rect) {
        D3D11_VIEWPORT viewport = CD3D11_VIEWPORT(static_cast<float>(rect.offset.x),
                                                  static_cast<float>(rect.offset.y),
                                                  static_cast<float>(rect.extent.width),
                                                  static_cast<float>(rect.extent.height));
        _d3d11MirrorContext->RSSetViewports(1, &viewport);
        D3D11_RECT rects[1];
        rects[0].top = rect.offset.y;
        rects[0].left = rect.offset.x;
        rects[0].bottom = rect.offset.y + rect.extent.height;
        rects[0].right = rect.offset.x + rect.extent.width;
        _d3d11MirrorContext->RSSetScissorRects(1, rects);

        _d3d11MirrorContext->OMSetRenderTargets(1, _targetView.GetAddressOf(), nullptr);
    }

    void D3D11Mirror::drawOrthoQuad() {
        // Ratios from checkFOVs() scale the quad so the cropped view FOV maps
        // onto the full HMD FOV.
        XMMATRIX mat_projection = XMMatrixOrthographicLH(_fovHorizRatio, _fovVertRatio, 0.01f, 100.0f);

        quad_transform_buffer_t transform_buffer;
        XMStoreFloat4x4(&transform_buffer.viewproj, XMMatrixTranspose(mat_projection));
        XMStoreFloat4x4(&transform_buffer.world, XMMatrixTranspose(XMMatrixTranslation(0.0f, 0.0f, 0.5f)));

        _d3d11MirrorContext->UpdateSubresource(_quadConstantBuffer.Get(), 0, nullptr, &transform_buffer, 0, 0);
        _d3d11MirrorContext->DrawIndexed((UINT)_countof(quad_inds), 0, 0);
    }

    void D3D11Mirror::Blend(const XrCompositionLayerProjectionView* view,
                            const XrFovf& hmdFov,
                            const XrCompositionLayerQuad* quad,
                            const DXGI_FORMAT format,
                            const XrSpace viewSpace,
                            const XrTime displayTime) {
        if (!_initialized)
            return;

        auto it = _sourceData.find(quad->subImage.swapchain);
        if (it == _sourceData.end() || !it->second._texture)
            return;

        checkCopyTex(view->subImage.imageRect.extent.width, view->subImage.imageRect.extent.height, format);

        if (_compositorTexture == nullptr || _mirrorTextures.size() == 0)
            return;

        D3D11_TEXTURE2D_DESC srcDesc;
        it->second._texture->GetDesc(&srcDesc);

        syncToSource(it->second);
        ScopedKeyedMutex lock(it->second._keyedMutex.Get());
        if (!lock.acquired())
            return;

        writeQuadUVs(quad->subImage.imageRect, srcDesc);
        writeBlendConstants(0.0f, 0.0f, static_cast<float>(quad->subImage.imageArrayIndex), 0.0f);
        bindQuadPipeline(it->second);

        XrRect2Di rect = {{0, 0}, {static_cast<int32_t>(_comp_desc.Width), static_cast<int32_t>(_comp_desc.Height)}};
        setTargetRect(rect);

        // Set up camera matrices based on OpenXR's predicted viewpoint
        // information; when camera smoothing is active, draw the quad from the
        // same smoothed camera and cropped FOV as the eye image so overlays
        // stay registered with the background.
        XrFovf projectionFov = hmdFov;
        XMMATRIX cameraPose = XMMatrixAffineTransformation(DirectX::g_XMOne,
                                                           DirectX::g_XMZero,
                                                           XMLoadFloat4((XMFLOAT4*)&view->pose.orientation),
                                                           XMLoadFloat3((XMFLOAT3*)&view->pose.position));
        if (smoothingActive()) {
            computeSmoothedDelta(displayTime, view->pose, hmdFov);
            const XMMATRIX relM = XMMatrixAffineTransformation(
                DirectX::g_XMOne, DirectX::g_XMZero, XMLoadFloat4(&_smoothRelQuat), XMLoadFloat3(&_smoothRelPos));
            cameraPose = relM * cameraPose;
            const float cropScale = 1.0f - std::clamp(_pMirrorSurfaceData->smoothCrop, 0.0f, 25.0f) / 100.0f;
            projectionFov = scaleFovTan(hmdFov, cropScale);
        }
        XMMATRIX mat_projection = d3dXrProjection(projectionFov, 0.05f, 100.0f);
        XMMATRIX mat_view = XMMatrixInverse(nullptr, cameraPose);

        // Put camera matrices into the shader's constant buffer
        quad_transform_buffer_t transform_buffer;
        XMStoreFloat4x4(&transform_buffer.viewproj, XMMatrixTranspose(mat_view * mat_projection));

        XMFLOAT4 scalingVector = {
            quad->size.width * ((float)view->subImage.imageRect.extent.width / (float)_comp_desc.Width), quad->size.height, 1.f, 1.f};
        XMMATRIX mat_model = XMMatrixAffineTransformation(XMLoadFloat4(&scalingVector),
                                                          DirectX::g_XMZero,
                                                          XMLoadFloat4((XMFLOAT4*)&quad->pose.orientation),
                                                          XMLoadFloat3((XMFLOAT3*)&quad->pose.position));

        // Account for the quad layer's space. If the location is unavailable
        // (e.g. no projection layer space to locate against, or tracking loss)
        // draw the quad untransformed rather than with a garbage pose.
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
        constexpr XrSpaceLocationFlags requiredFlags =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (viewSpace != XR_NULL_HANDLE &&
            XR_SUCCEEDED(
                layer_OBSMirror::GetInstance()->xrLocateSpace(quad->space, viewSpace, displayTime, &location)) &&
            (location.locationFlags & requiredFlags) == requiredFlags) {
            XMMATRIX mat_space = XMMatrixAffineTransformation(DirectX::g_XMOne,
                                                              DirectX::g_XMZero,
                                                              XMLoadFloat4((XMFLOAT4*)&location.pose.orientation),
                                                              XMLoadFloat3((XMFLOAT3*)&location.pose.position));
            mat_model = XMMatrixMultiply(mat_model, mat_space);
        }

        // Update the shader's constant buffer with the transform matrix info, and then draw the quad
        XMStoreFloat4x4(&transform_buffer.world, XMMatrixTranspose(mat_model));
        _d3d11MirrorContext->UpdateSubresource(_quadConstantBuffer.Get(), 0, nullptr, &transform_buffer, 0, 0);
        _d3d11MirrorContext->DrawIndexed((UINT)_countof(quad_inds), 0, 0);
    }

    void D3D11Mirror::Blend(const XrCompositionLayerProjectionView* view,
                            const XrFovf& hmdFov,
                            const DXGI_FORMAT format,
                            const XrSpace viewSpace,
                            const XrTime displayTime) {
        if (!_initialized)
            return;

        const bool smoothing = smoothingActive();

        if (!smoothing && XMScalarNearEqual(hmdFov.angleDown, view->fov.angleDown, 0.001f) &&
            XMScalarNearEqual(hmdFov.angleUp, view->fov.angleUp, 0.001f) &&
            XMScalarNearEqual(hmdFov.angleLeft, view->fov.angleLeft, 0.001f) &&
            XMScalarNearEqual(hmdFov.angleRight, view->fov.angleRight, 0.001f))
        {
            // If FOV is the same then use fast copy
            copyPerspectiveTex(view->subImage.imageRect, view->subImage.imageArrayIndex, format, view->subImage.swapchain);
            return;
        }

        auto it = _sourceData.find(view->subImage.swapchain);
        if (it == _sourceData.end() || !it->second._texture)
            return;

        checkCopyTex(view->subImage.imageRect.extent.width, view->subImage.imageRect.extent.height, format);

        if (_compositorTexture == nullptr || _mirrorTextures.size() == 0)
            return;

        if (smoothing) {
            const XrRect2Di rect = {{0, 0}, view->subImage.imageRect.extent};
            drawSmoothedEye(view, it->second, rect, false, 1.0f, displayTime);
            return;
        }

        D3D11_TEXTURE2D_DESC srcDesc;
        it->second._texture->GetDesc(&srcDesc);

        syncToSource(it->second);
        ScopedKeyedMutex lock(it->second._keyedMutex.Get());
        if (!lock.acquired())
            return;

        writeQuadUVs(view->subImage.imageRect, srcDesc);

        const float texIndex = srcDesc.ArraySize > 1 ? static_cast<float>(view->subImage.imageArrayIndex) : 0.0f;
        writeBlendConstants(0.0f, 0.0f, texIndex, 1.0f);
        bindQuadPipeline(it->second);

        XrRect2Di rect = {{0, 0}, view->subImage.imageRect.extent};
        setTargetRect(rect);

        checkFOVs(hmdFov, view->fov);
        drawOrthoQuad();
    }

    void D3D11Mirror::Blend(const XrCompositionLayerProjectionView* view1,
                            const XrFovf& hmdFov1,
                            const XrCompositionLayerProjectionView* view2,
                            const XrFovf& hmdFov2,
                            const DXGI_FORMAT format,
                            const XrSpace viewSpace,
                            const XrTime displayTime) {
        if (!_initialized)
            return;

        auto it1 = _sourceData.find(view1->subImage.swapchain);
        if (it1 == _sourceData.end() || !it1->second._texture)
            return;

        auto it2 = _sourceData.find(view2->subImage.swapchain);
        if (it2 == _sourceData.end() || !it2->second._texture)
            return;

        checkCopyTex(view1->subImage.imageRect.extent.width,
                     view1->subImage.imageRect.extent.height,
                     format);

        if (_compositorTexture == nullptr || _mirrorTextures.size() == 0)
            return;

        const bool smoothing = smoothingActive();

        // First eye
        if (smoothing) {
            const XrRect2Di rect = {{0, 0}, view1->subImage.imageRect.extent};
            drawSmoothedEye(view1, it1->second, rect, false, 0.0f, displayTime);
        }
        else if (XMScalarNearEqual(hmdFov1.angleDown, view1->fov.angleDown, 0.001f) &&
            XMScalarNearEqual(hmdFov1.angleUp, view1->fov.angleUp, 0.001f) &&
            XMScalarNearEqual(hmdFov1.angleLeft, view1->fov.angleLeft, 0.001f) &&
            XMScalarNearEqual(hmdFov1.angleRight, view1->fov.angleRight, 0.001f))
        {
            // If FOV is the same then use fast copy
            copyPerspectiveTex(view1->subImage.imageRect, view1->subImage.imageArrayIndex, format, view1->subImage.swapchain);
        }
        else
        {
            D3D11_TEXTURE2D_DESC srcDesc;
            it1->second._texture->GetDesc(&srcDesc);

            syncToSource(it1->second);
            ScopedKeyedMutex lock(it1->second._keyedMutex.Get());
            if (lock.acquired()) {
                writeQuadUVs(view1->subImage.imageRect, srcDesc);

                const float texIndex =
                    srcDesc.ArraySize > 1 ? static_cast<float>(view1->subImage.imageArrayIndex) : 0.0f;
                writeBlendConstants(0.0f, 0.0f, texIndex, 0.0f);
                bindQuadPipeline(it1->second);

                XrRect2Di rect = {{0, 0}, view1->subImage.imageRect.extent};
                setTargetRect(rect);

                checkFOVs(hmdFov1, view1->fov);
                drawOrthoQuad();
            }
        }

        // Second eye, drawn at an offset with a smoothstep blend across the seam
        if (smoothing) {
            XrRect2Di rect = {{0, 0}, view2->subImage.imageRect.extent};
            rect.offset.x =
                rect.offset.x + static_cast<int32_t>((rect.extent.width * _pMirrorSurfaceData->overlap) / 100);
            drawSmoothedEye(view2, it2->second, rect, true, 1.0f, displayTime);
        } else {
            D3D11_TEXTURE2D_DESC srcDesc;
            it2->second._texture->GetDesc(&srcDesc);

            syncToSource(it2->second);
            ScopedKeyedMutex lock(it2->second._keyedMutex.Get());
            if (!lock.acquired())
                return;

            const UVRect uv = writeQuadUVs(view2->subImage.imageRect, srcDesc);

            const float blendOffset = _pMirrorSurfaceData->blend / 2.0f;
            const float blendWidth = (uv.endX - uv.startX) / 100.0f;
            const float blendStartX =
                std::max(uv.startX, uv.startX + ((_pMirrorSurfaceData->blendPos - blendOffset) * blendWidth));
            const float blendEndX =
                std::min(uv.endX, uv.startX + ((_pMirrorSurfaceData->blendPos + blendOffset) * blendWidth));
            const float texIndex = srcDesc.ArraySize > 1 ? static_cast<float>(view2->subImage.imageArrayIndex) : 0.0f;
            writeBlendConstants(blendStartX, blendEndX, texIndex, 1.0f);
            bindQuadPipeline(it2->second);

            XrRect2Di rect = {{0, 0}, view2->subImage.imageRect.extent};
            rect.offset.x =
                rect.offset.x + static_cast<int32_t>((rect.extent.width * _pMirrorSurfaceData->overlap) / 100);
            setTargetRect(rect);

            drawOrthoQuad();
        }
    }

    void D3D11Mirror::checkFOVs(const XrFovf& hmdFov, const XrFovf& viewFov)
    {
        if (hmdFov.angleDown != _hmdFov.angleDown || hmdFov.angleUp != _hmdFov.angleUp ||
            hmdFov.angleLeft != _hmdFov.angleLeft || hmdFov.angleRight != _hmdFov.angleRight ||
            viewFov.angleDown != _viewFov.angleDown || viewFov.angleUp != _viewFov.angleUp ||
            viewFov.angleLeft != _viewFov.angleLeft || viewFov.angleRight != _viewFov.angleRight)
        {
            _hmdFov = hmdFov;
            _viewFov = viewFov;

            const float hmdleft = tanf(hmdFov.angleLeft);
            const float hmdright = tanf(hmdFov.angleRight);
            const float hmddown = tanf(hmdFov.angleDown);
            const float hmdup = tanf(hmdFov.angleUp);

            const float viewleft = tanf(viewFov.angleLeft);
            const float viewright = tanf(viewFov.angleRight);
            const float viewdown = tanf(viewFov.angleDown);
            const float viewup = tanf(viewFov.angleUp);

            // Modified FOV handling
            _fovVertRatio = ((hmddown / viewdown) + (hmdup / viewup)) / 2.f;
            _fovHorizRatio = ((hmdleft / viewleft) + (hmdright / viewright)) / 2.f;
        }
    }

    void D3D11Mirror::copyPerspectiveTex(const XrRect2Di& imgRect,
                                         const uint32_t arraySlice,
                                         const DXGI_FORMAT format,
                                         const XrSwapchain& swapchain) {
        if (!_initialized)
            return;

        auto it = _sourceData.find(swapchain);
        if (it == _sourceData.end() || !it->second._texture)
            return;

        checkCopyTex(imgRect.extent.width, imgRect.extent.height, format);
        if (!_compositorTexture)
            return;

        D3D11_TEXTURE2D_DESC srcDesc;
        it->second._texture->GetDesc(&srcDesc);
        const UINT slice = arraySlice < srcDesc.ArraySize ? arraySlice : 0;
        const UINT subresource = D3D11CalcSubresource(0, slice, srcDesc.MipLevels);

        syncToSource(it->second);
        ScopedKeyedMutex lock(it->second._keyedMutex.Get());
        if (!lock.acquired())
            return;

        D3D11_BOX sourceRegion;
        sourceRegion.left = imgRect.offset.x;
        sourceRegion.right = imgRect.offset.x + imgRect.extent.width;
        sourceRegion.top = imgRect.offset.y;
        sourceRegion.bottom = imgRect.offset.y + imgRect.extent.height;
        sourceRegion.front = 0;
        sourceRegion.back = 1;
        _d3d11MirrorContext->CopySubresourceRegion(
            _compositorTexture.Get(), 0, 0, 0, 0, it->second._texture.Get(), subresource, &sourceRegion);
    }

    void D3D11Mirror::checkCopyTex(const uint32_t srcWidth, const uint32_t height, const DXGI_FORMAT format) {
        if (!_initialized || srcWidth == 0 || height == 0)
            return;

        DxgiFormatInfo info = {};
        if (!GetFormatInfo(format, info)) {
            Log("Unknown DXGI texture format %d\n", format);
            return;
        }

        const bool linear = info.bpc > 8;
        const DXGI_FORMAT renderFormat = linear ? info.linear : info.srgb;
        const float separation =
            _pMirrorSurfaceData->eyeIndex == 2 ? std::clamp(_pMirrorSurfaceData->overlap, 0.0f, 100.0f) / 100.0f : 0.0f;
        uint32_t targetWidth = static_cast<uint32_t>(srcWidth * (1.0f + separation));
        targetWidth += targetWidth % 2;

        bool needsRebuild = !_compositorTexture || !_targetView || _mirrorTextures.size() != kMirrorTextureCount;
        if (_compositorTexture) {
            D3D11_TEXTURE2D_DESC currentDesc{};
            _compositorTexture->GetDesc(&currentDesc);
            needsRebuild = needsRebuild || currentDesc.Width != targetWidth || currentDesc.Height != height ||
                           currentDesc.Format != renderFormat;
        }
        if (!needsRebuild)
            return;

        // Handle zero is the generation marker. Clear it before releasing or
        // replacing resources, then publish it last after all three handles exist.
        _pMirrorSurfaceData->sharedHandle[0] = 0;
        MemoryBarrier();
        for (uint32_t i = 1; i < kMirrorTextureCount; ++i)
            _pMirrorSurfaceData->sharedHandle[i] = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = targetWidth;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = renderFormat;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        Log("Use linear = %d Linear = %d sRGB = %d\n", linear, info.linear, info.srgb);
        Log("Creating mirror textures w %u h %u f %d\n", desc.Width, desc.Height, format);

        ComPtr<ID3D11Texture2D> newCompositorTexture;
        CHECK_DX(_d3d11MirrorDevice->CreateTexture2D(&desc, nullptr, newCompositorTexture.GetAddressOf()));
        if (!newCompositorTexture)
            return;

        desc.Format = info.linear;
        std::vector<ComPtr<ID3D11Texture2D>> newMirrorTextures(kMirrorTextureCount);
        uint64_t newSharedHandles[kMirrorTextureCount] = {};
        for (size_t i = 0; i < newMirrorTextures.size(); ++i) {
            auto& texture = newMirrorTextures[i];
            CHECK_DX(_d3d11MirrorDevice->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()));
            if (!texture)
                return;

            ComPtr<IDXGIResource> resource;
            CHECK_DX(texture->QueryInterface(IID_PPV_ARGS(&resource)));
            if (!resource)
                return;

            HANDLE sharedHandle = nullptr;
            if (FAILED(resource->GetSharedHandle(&sharedHandle)) || !sharedHandle)
                return;

            newSharedHandles[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
            Log("Shared handle: 0x%p\n", sharedHandle);
        }

        D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
        targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        targetDesc.Format = renderFormat;
        targetDesc.Texture2D.MipSlice = 0;
        ComPtr<ID3D11RenderTargetView> newTargetView;
        CHECK_DX(_d3d11MirrorDevice->CreateRenderTargetView(
            newCompositorTexture.Get(), &targetDesc, newTargetView.GetAddressOf()));
        if (!newTargetView)
            return;

        _compositorTexture = std::move(newCompositorTexture);
        _mirrorTextures = std::move(newMirrorTextures);
        _targetView = std::move(newTargetView);
        _compositorTexture->GetDesc(&_comp_desc);

        for (uint32_t i = 1; i < kMirrorTextureCount; ++i)
            _pMirrorSurfaceData->sharedHandle[i] = newSharedHandles[i];
        MemoryBarrier();
        _pMirrorSurfaceData->sharedHandle[0] = newSharedHandles[0];

        Log("Compositor texture description: %d x %d Format %d\n",
            _comp_desc.Width,
            _comp_desc.Height,
            _comp_desc.Format);
    }

    void D3D11Mirror::copyToMirror() {
        if (!_initialized || !_compositorTexture || _mirrorTextures.size() != kMirrorTextureCount)
            return;

        ++_frameCounter;
        auto& tex = _mirrorTextures[_frameCounter % kMirrorTextureCount];
        if (!tex)
            return;

        _d3d11MirrorContext->CopyResource(tex.Get(), _compositorTexture.Get());

        if (_obsCopyFence && _d3d11MirrorContext4) {
            _d3d11MirrorContext4->Signal(_obsCopyFence.Get(), _frameCounter);
            // Publish only frames whose GPU copy has completed, so the plugin
            // never samples a ring slot that is still being written.
            _pMirrorSurfaceData->lastProcessedIndex = static_cast<uint32_t>(_obsCopyFence->GetCompletedValue());
        } else {
            // Without fences, publish the previous frame; its copy was
            // submitted a full frame ago and is almost certainly complete.
            _pMirrorSurfaceData->lastProcessedIndex = _frameCounter > 0 ? _frameCounter - 1 : 0;
        }
    }

    void D3D11Mirror::checkOBSRunning() {
        if (!_initialized) {
            _obsRunning = false;
            return;
        }

        const uint32_t frameNumber = _pMirrorSurfaceData->frameNumber;
        if (_lastOBSFrameNumber == frameNumber) {
            if (_obsFrameCounter <= 10)
                _obsFrameCounter++;
        } else {
            _obsFrameCounter = 0;
        }

        _obsRunning = _obsFrameCounter <= 10;
        _lastOBSFrameNumber = frameNumber;
    }

    uint32_t D3D11Mirror::getEyeIndex() const {
        return _initialized ? std::min(_pMirrorSurfaceData->eyeIndex.load(), 2u) : 0u;
    }

    bool D3D11Mirror::createMirrorSurface() {
        Log("Mapping OBS mirror IPC surface.\n");
        _mapFile = CreateFileMappingW(INVALID_HANDLE_VALUE,      // use paging file
                                      NULL,                      // default security
                                      PAGE_READWRITE,            // read/write access
                                      0,                         // maximum object size (high-order DWORD)
                                      sizeof(MirrorSurfaceData), // maximum object size (low-order DWORD)
                                      obs_mirror_ipc::kSharedMemoryName); // name of mapping object

        if (_mapFile == nullptr) {
            Log("Could not create file mapping object (%d).\n", GetLastError());
            return false;
        }
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        _pMirrorSurfaceData = (MirrorSurfaceData*)MapViewOfFile(_mapFile,            // handle to map object
                                                                FILE_MAP_ALL_ACCESS, // read/write permission
                                                                0,
                                                                0,
                                                                sizeof(MirrorSurfaceData));

        if (_pMirrorSurfaceData == nullptr) {
            Log("Could not map view of file (%d).\n", GetLastError());
            CloseHandle(_mapFile);
            _mapFile = nullptr;
            return false;
        }
        if (created) {
            new (_pMirrorSurfaceData) MirrorSurfaceData();
        }
        return true;
    }
} // Mirror namespace
