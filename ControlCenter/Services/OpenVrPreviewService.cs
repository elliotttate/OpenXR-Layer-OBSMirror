using System.Diagnostics;
using System.Runtime.InteropServices;
using OBSMirror.ControlCenter.Localization;
using Valve.VR;
using Vortice.D3DCompiler;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using static Vortice.Direct3D11.D3D11;
using static Vortice.DXGI.DXGI;

namespace OBSMirror.ControlCenter.Services;

/// <summary>
/// Reads SteamVR's native right-eye compositor mirror directly. This is kept
/// separate from the OpenXR shared-surface reader so merely opening Control
/// Center can never start SteamVR or change the active OpenXR runtime.
/// </summary>
internal sealed class OpenVrPreviewService : IDisposable
{
    private const int ReadbackRingDepth = 3;
    private const int PreviewMaxWidth = 960;
    private const int PreviewMaxHeight = 540;
    private const string PreviewShaderSource = """
        struct VertexOutput
        {
            float4 Position : SV_Position;
            float2 TexCoord : TEXCOORD0;
        };

        VertexOutput VSMain(uint vertexId : SV_VertexID)
        {
            VertexOutput output;
            float2 texCoord = float2((vertexId << 1) & 2, vertexId & 2);
            output.Position = float4(texCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
            output.TexCoord = texCoord;
            return output;
        }

        Texture2D MirrorTexture : register(t0);
        SamplerState MirrorSampler : register(s0);

        float4 PSMain(VertexOutput input) : SV_Target
        {
            return float4(MirrorTexture.Sample(MirrorSampler, input.TexCoord).rgb, 1.0);
        }
        """;

    private CVRSystem? _system;
    private CVRCompositor? _compositor;
    private bool _runtimeAcquired;
    private IntPtr _openVrMirrorView;
    private ID3D11Device? _device;
    private ID3D11DeviceContext? _context;
    private ID3D11ShaderResourceView? _mirrorView;
    private ID3D11Texture2D? _mirrorTexture;
    private ID3D11Texture2D? _downsampleTexture;
    private ID3D11RenderTargetView? _downsampleTargetView;
    private ID3D11VertexShader? _vertexShader;
    private ID3D11PixelShader? _pixelShader;
    private ID3D11SamplerState? _sampler;
    private readonly ID3D11Texture2D?[] _readbackRing = new ID3D11Texture2D?[ReadbackRingDepth];
    private long _readbackIssued;
    private long _lastInitializeAttempt;
    private string _lastInitializeError = "SteamVR is not running.";
    private string _adapterName = "Not selected";
    private bool _disposed;

    public MirrorPreviewResult CaptureFrame()
    {
        if (_disposed)
            return Waiting(
                Loc.S("Code_Svc_SteamVrPreviewStopped", "SteamVR preview stopped"),
                Loc.S("Code_Svc_SteamVrPreviewStoppedDetail", "Reopen the app to restart the preview."));

        if (_device is null && !TryInitialize())
            return Waiting(
                Loc.S("Code_Svc_WaitingForSteamVrMirror", "Waiting for a SteamVR mirror"),
                _lastInitializeError);

        try
        {
            RenderPreview();
            var writeSlot = (int)(_readbackIssued % ReadbackRingDepth);
            _context!.CopyResource(_readbackRing[writeSlot]!, _downsampleTexture!);
            _context.Flush();
            _readbackIssued++;
            var mapSlot = _readbackIssued >= ReadbackRingDepth
                ? (int)(_readbackIssued % ReadbackRingDepth)
                : 0;
            var readback = _readbackRing[mapSlot]!;
            var mapped = _context.Map(readback, 0, MapMode.Read, Vortice.Direct3D11.MapFlags.None);
            MirrorPreviewFrame frame;
            try
            {
                frame = CopyBgraFrame(
                    mapped,
                    readback.Description,
                    _mirrorTexture!.Description.Width,
                    _mirrorTexture.Description.Height);
            }
            finally
            {
                _context.Unmap(readback, 0);
            }

            return new MirrorPreviewResult(
                frame,
                Loc.S("Code_Svc_LiveSteamVrMirror", "Live SteamVR mirror"),
                Loc.F(
                    "Code_Svc_SteamVrFrameDetail",
                    "Right eye  •  Source {0} × {1}  •  Preview {2} × {3}  •  {4}",
                    _mirrorTexture.Description.Width,
                    _mirrorTexture.Description.Height,
                    frame.Width,
                    frame.Height,
                    _adapterName),
                true,
                true);
        }
        catch (Exception ex)
        {
            Reset();
            _lastInitializeError = $"SteamVR preview paused: {FriendlyError(ex)}";
            return Waiting(
                Loc.S("Code_Svc_SteamVrPreviewPaused", "SteamVR preview paused"),
                _lastInitializeError);
        }
    }

    private bool TryInitialize()
    {
        var now = Environment.TickCount64;
        if (now - _lastInitializeAttempt < 2000)
            return false;
        _lastInitializeAttempt = now;
        Reset();

        if (!IsRuntimeRunning())
        {
            _lastInitializeError = Loc.S(
                "Code_Svc_StartSteamVrDetail",
                "Start SteamVR and an OpenVR application; the preview will connect automatically.");
            return false;
        }

        try
        {
            var initError = EVRInitError.None;
            _system = OpenVR.Init(ref initError, EVRApplicationType.VRApplication_Background);
            if (initError != EVRInitError.None || _system is null)
                throw new InvalidOperationException($"OpenVR initialization failed: {OpenVR.GetStringForHmdError(initError)}");
            _runtimeAcquired = true;
            _compositor = OpenVR.Compositor
                          ?? throw new InvalidOperationException("SteamVR did not expose its compositor interface.");

            using var factory = CreateDXGIFactory1<IDXGIFactory1>();
            var featureLevels = new[]
            {
                FeatureLevel.Level_11_1,
                FeatureLevel.Level_11_0,
                FeatureLevel.Level_10_1,
                FeatureLevel.Level_10_0
            };
            string? lastAdapterError = null;
            uint selectedWidth = 0;
            uint selectedHeight = 0;
            for (uint adapterIndex = 0; ; adapterIndex++)
            {
                if (factory.EnumAdapters1(adapterIndex, out var adapter).Failure)
                    break;
                using (adapter)
                {
                    ID3D11Device? candidateDevice = null;
                    ID3D11DeviceContext? candidateContext = null;
                    IntPtr candidateMirrorView = IntPtr.Zero;
                    ID3D11ShaderResourceView? candidateWrappedView = null;
                    ID3D11Texture2D? candidateTexture = null;
                    try
                    {
                        var createResult = D3D11CreateDevice(
                            adapter,
                            DriverType.Unknown,
                            DeviceCreationFlags.BgraSupport,
                            featureLevels,
                            out candidateDevice,
                            out candidateContext);
                        if (createResult.Failure || candidateDevice is null || candidateContext is null)
                            throw new InvalidOperationException("Direct3D device creation failed.");

                        var mirrorError = _compositor.GetMirrorTextureD3D11(
                            EVREye.Eye_Right,
                            candidateDevice.NativePointer,
                            ref candidateMirrorView);
                        if (mirrorError != EVRCompositorError.None || candidateMirrorView == IntPtr.Zero)
                            throw new InvalidOperationException($"SteamVR compositor error {(int)mirrorError}.");

                        // The Vortice wrapper owns the extra COM reference. The
                        // original OpenVR view is released separately through
                        // ReleaseMirrorTextureD3D11, as required by Valve.
                        Marshal.AddRef(candidateMirrorView);
                        candidateWrappedView = new ID3D11ShaderResourceView(candidateMirrorView);
                        using var resource = candidateWrappedView.Resource;
                        candidateTexture = resource.QueryInterface<ID3D11Texture2D>();
                        var description = candidateTexture.Description;
                        if (description.Width == 0 || description.Height == 0)
                            throw new InvalidOperationException("SteamVR returned an empty mirror texture.");

                        _device = candidateDevice;
                        candidateDevice = null;
                        _context = candidateContext;
                        candidateContext = null;
                        _openVrMirrorView = candidateMirrorView;
                        candidateMirrorView = IntPtr.Zero;
                        _mirrorView = candidateWrappedView;
                        candidateWrappedView = null;
                        _mirrorTexture = candidateTexture;
                        candidateTexture = null;
                        _adapterName = adapter.Description1.Description.Trim();
                        selectedWidth = description.Width;
                        selectedHeight = description.Height;
                        break;
                    }
                    catch (Exception ex)
                    {
                        lastAdapterError = $"{adapter.Description1.Description.Trim()}: {FriendlyError(ex)}";
                        candidateTexture?.Dispose();
                        candidateWrappedView?.Dispose();
                        if (candidateMirrorView != IntPtr.Zero)
                            _compositor.ReleaseMirrorTextureD3D11(candidateMirrorView);
                        candidateContext?.Dispose();
                        candidateDevice?.Dispose();
                    }
                }
            }

            if (_device is null)
                throw new InvalidOperationException(lastAdapterError ?? "No Direct3D adapter exposed the SteamVR mirror.");

            // Resource creation happens after the adapter-selection loop so a
            // failure is handled by the outer reset path. At this point the
            // selected device and mirror handle have transferred ownership to
            // this service and must not be retried as adapter-local objects.
            CreatePreviewResources(selectedWidth, selectedHeight);
            _lastInitializeError = string.Empty;
            return true;
        }
        catch (Exception ex)
        {
            _lastInitializeError = FriendlyError(ex);
            Reset();
            return false;
        }
    }

    private void CreatePreviewResources(uint sourceWidth, uint sourceHeight)
    {
        var (previewWidth, previewHeight) = CalculatePreviewSize(sourceWidth, sourceHeight);
        var shaderFlags = ShaderFlags.EnableStrictness | ShaderFlags.OptimizationLevel3;
        var vertexBytecode = Compiler.Compile(
            PreviewShaderSource, "VSMain", "OpenVrPreview.hlsl", "vs_5_0", shaderFlags, EffectFlags.None);
        var pixelBytecode = Compiler.Compile(
            PreviewShaderSource, "PSMain", "OpenVrPreview.hlsl", "ps_5_0", shaderFlags, EffectFlags.None);

        // SteamVR exposes the compositor mirror through an sRGB SRV. Sampling
        // decodes it to linear light, so the preview render target must be sRGB
        // as well to encode the pixels again before WinUI displays the BGRA
        // byte buffer. A linear target makes the preview visibly too dark.
        var downsampleDescription = new Texture2DDescription(
            Format.B8G8R8A8_UNorm_SRgb,
            previewWidth,
            previewHeight,
            1,
            1,
            BindFlags.RenderTarget,
            ResourceUsage.Default,
            CpuAccessFlags.None,
            1,
            0,
            ResourceOptionFlags.None);
        _downsampleTexture = _device!.CreateTexture2D(in downsampleDescription);
        _downsampleTargetView = _device.CreateRenderTargetView(_downsampleTexture);
        _vertexShader = _device.CreateVertexShader(vertexBytecode.Span, null);
        _pixelShader = _device.CreatePixelShader(pixelBytecode.Span, null);
        _sampler = _device.CreateSamplerState(new SamplerDescription(
            Filter.MinMagLinearMipPoint,
            TextureAddressMode.Clamp,
            0.0f,
            1,
            ComparisonFunction.Never,
            0.0f,
            float.MaxValue));

        var readbackDescription = new Texture2DDescription(
            Format.B8G8R8A8_UNorm_SRgb,
            previewWidth,
            previewHeight,
            1,
            1,
            BindFlags.None,
            ResourceUsage.Staging,
            CpuAccessFlags.Read,
            1,
            0,
            ResourceOptionFlags.None);
        for (var index = 0; index < ReadbackRingDepth; index++)
            _readbackRing[index] = _device.CreateTexture2D(in readbackDescription);
        _readbackIssued = 0;
    }

    private void RenderPreview()
    {
        var previewDescription = _downsampleTexture!.Description;
        _context!.OMSetRenderTargets(_downsampleTargetView!, null);
        _context.RSSetViewports([new Viewport(previewDescription.Width, previewDescription.Height)]);
        _context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        _context.VSSetShader(_vertexShader!);
        _context.PSSetShader(_pixelShader!);
        _context.PSSetShaderResources(0, [_mirrorView!]);
        _context.PSSetSamplers(0, [_sampler!]);
        _context.Draw(3, 0);
        _context.PSSetShaderResources(0, [null!]);
        _context.OMSetRenderTargets(Array.Empty<ID3D11RenderTargetView>(), null);
    }

    private static unsafe MirrorPreviewFrame CopyBgraFrame(
        MappedSubresource mapped,
        Texture2DDescription description,
        uint sourceWidth,
        uint sourceHeight)
    {
        var width = checked((int)description.Width);
        var height = checked((int)description.Height);
        var pixels = GC.AllocateUninitializedArray<byte>(checked(width * height * 4));
        fixed (byte* output = pixels)
        {
            for (var y = 0; y < height; y++)
            {
                var sourceRow = (uint*)((byte*)mapped.DataPointer + y * mapped.RowPitch);
                var outputRow = (uint*)(output + y * width * 4);
                for (var x = 0; x < width; x++)
                    outputRow[x] = sourceRow[x] | 0xff000000u;
            }
        }
        return new MirrorPreviewFrame(pixels, width, height, sourceWidth, sourceHeight);
    }

    private static (uint Width, uint Height) CalculatePreviewSize(uint width, uint height)
    {
        var scale = Math.Min(
            1.0,
            Math.Min(PreviewMaxWidth / (double)width, PreviewMaxHeight / (double)height));
        return (
            Math.Max(1u, (uint)Math.Round(width * scale)),
            Math.Max(1u, (uint)Math.Round(height * scale)));
    }

    private static MirrorPreviewResult Waiting(string status, string detail) =>
        new(null, status, detail, false, false);

    internal static bool IsRuntimeRunning()
    {
        var processes = Process.GetProcessesByName("vrserver");
        try
        {
            return processes.Length > 0;
        }
        finally
        {
            foreach (var process in processes)
                process.Dispose();
        }
    }

    private static string FriendlyError(Exception ex) =>
        string.IsNullOrWhiteSpace(ex.Message) ? ex.GetType().Name : ex.Message;

    private void Reset()
    {
        foreach (var readback in _readbackRing)
            readback?.Dispose();
        Array.Clear(_readbackRing);
        _readbackIssued = 0;
        _sampler?.Dispose();
        _sampler = null;
        _pixelShader?.Dispose();
        _pixelShader = null;
        _vertexShader?.Dispose();
        _vertexShader = null;
        _downsampleTargetView?.Dispose();
        _downsampleTargetView = null;
        _downsampleTexture?.Dispose();
        _downsampleTexture = null;
        _mirrorTexture?.Dispose();
        _mirrorTexture = null;
        _mirrorView?.Dispose();
        _mirrorView = null;
        if (_openVrMirrorView != IntPtr.Zero && _compositor is not null)
            _compositor.ReleaseMirrorTextureD3D11(_openVrMirrorView);
        _openVrMirrorView = IntPtr.Zero;

        _context?.Dispose();
        _context = null;
        _device?.Dispose();
        _device = null;
        if (_runtimeAcquired)
        {
            OpenVR.Shutdown();
            _runtimeAcquired = false;
        }
        _system = null;
        _compositor = null;
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        Reset();
    }
}
