using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Text;
using Vortice.D3DCompiler;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using static Vortice.Direct3D11.D3D11;
using static Vortice.DXGI.DXGI;

namespace OBSMirror.ControlCenter.Services;

public sealed record MirrorPreviewFrame(
    byte[] Pixels,
    int Width,
    int Height,
    uint SourceWidth,
    uint SourceHeight);

public sealed record MirrorPreviewResult(
    MirrorPreviewFrame? Frame,
    string Status,
    string Detail,
    bool IsLive,
    // True while the shared surface is mapped. The capture loop must keep
    // polling fast in that state (the layer needs the consumer heartbeat to
    // publish anything at all); only a missing surface allows slow polling.
    bool Connected);

public sealed record MirrorProducerIdentity(
    bool Connected,
    uint ProcessId,
    string ApplicationName);

/// <summary>
/// Reads the same triple-buffered D3D11 texture ring consumed by the OBS source
/// and GPU-downsamples it for the Control Center dashboard.
/// </summary>
public sealed class MirrorPreviewService : IDisposable
{
    private const string SharedMemoryName = "OpenXROBSMirrorSurface";
    // Layout constants mirror shared/obs_mirror_ipc.h; static_asserts there pin
    // every offset used here. The section is page-granular, so mapping a full
    // page works against both legacy (64-byte) and diagnostics-carrying layers.
    private const int LegacySurfaceSize = 64;
    private const int FullSurfaceViewSize = 4096;
    private const int LastProcessedIndexOffset = 0;
    private const int FrameNumberOffset = 4;
    private const int SharedHandlesOffset = 32;
    private const int SurfaceGenerationOffset = 56;
    private const uint DiagnosticsMagic = 0x4D52584F; // "OXRM"
    private const int DiagLayerMagicOffset = 64;
    private const int DiagLayerVersionOffset = 68;
    private const int DiagLayerPidOffset = 72;
    private const int DiagLayerAdapterLuidLowOffset = 76;
    private const int DiagLayerAdapterLuidHighOffset = 80;
    private const int DiagLayerHeartbeatOffset = 84;
    private const int DiagMirrorWidthOffset = 88;
    private const int DiagMirrorHeightOffset = 92;
    private const int DiagMirrorFormatOffset = 96;
    private const int DiagLayerVersionStringOffset = 100;
    private const int DiagLayerVersionStringLength = 32;
    private const int DiagApplicationNameOffset = 132;
    private const int DiagApplicationNameLength = 64;
    private const int DiagPreviewMagicOffset = 248;
    private const int DiagPreviewPidOffset = 252;
    private const int TextureCount = 3;
    // Staging readbacks are pipelined: each capture copies into one slot and
    // maps the copy issued two captures earlier, so Map never blocks the CPU
    // on the GPU (the old single-texture readback was a full sync per frame).
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

    private readonly object _gate = new();
    private MemoryMappedFile? _mapping;
    private MemoryMappedViewAccessor? _surface;
    private ID3D11Device? _device;
    private ID3D11DeviceContext? _context;
    private ID3D11Texture2D? _downsampleTexture;
    private ID3D11RenderTargetView? _downsampleTargetView;
    private ID3D11VertexShader? _previewVertexShader;
    private ID3D11PixelShader? _previewPixelShader;
    private ID3D11SamplerState? _previewSampler;
    private readonly ID3D11Texture2D?[] _readbackRing = new ID3D11Texture2D?[ReadbackRingDepth];
    private long _readbackIssued;
    private ID3D11Texture2D? _fullReadbackTexture;
    private readonly ID3D11Texture2D?[] _mirrorTextures = new ID3D11Texture2D?[TextureCount];
    private readonly ID3D11ShaderResourceView?[] _mirrorViews = new ID3D11ShaderResourceView?[TextureCount];
    private readonly ulong[] _sharedHandles = new ulong[TextureCount];
    private uint _surfaceGeneration;
    private bool _gpuPreviewVerified;
    private long _lastCpuFallbackTick;
    private bool _hasFrameIndex;
    private uint _lastFrameIndex;
    private long _lastFrameAdvanceTick;
    private long _blackSinceTick;
    private long _captureCount;
    private string _selectedAdapterName = "Not selected";
    private string _selectedAdapterLuid = "unknown";
    private string _lastPixelSummary = "No pixels have been read yet.";
    private string _lastDiagnosticKey = "starting";
    private string _lastDiagnosticMessage = "The preview has not attempted a capture yet.";
    private DateTime _lastDiagnosticAt = DateTime.Now;
    private long _lastDiagnosticLogTick;
    // False when attached to a pre-diagnostics layer that only provides the
    // legacy 64-byte surface.
    private bool _diagAvailable;
    private bool _disposed;

    public string LogPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "OpenXR-OBSMirror",
        "ControlCenter-preview.log");

    public MirrorPreviewService()
    {
        RecordDiagnostic(
            "starting",
            $"Control Center preview diagnostics started (pid {Environment.ProcessId}, version {GetType().Assembly.GetName().Version}).",
            forceLog: true);
    }

    public MirrorPreviewResult CaptureFrame()
    {
        lock (_gate)
        {
            if (_disposed)
                return DiagnosticWaiting(
                    "stopped",
                    "Preview stopped",
                    "Reopen the app to start the mirror preview again.");

            if (!EnsureSurface(out var mappingError))
                return DiagnosticWaiting(
                    mappingError.Contains("Start a VR app", StringComparison.OrdinalIgnoreCase)
                        ? "no-shared-surface"
                        : "shared-surface-error",
                    "Waiting for an OpenXR app",
                    mappingError,
                    warning: !mappingError.Contains("Start a VR app", StringComparison.OrdinalIgnoreCase));

            try
            {
                // The layer treats this counter as a consumer heartbeat. This
                // lets the dashboard preview work even when OBS is not open.
                // Interlocked so a concurrent OBS plugin increment is never
                // lost.
                IncrementConsumerHeartbeat();

                var generation = _surface!.ReadUInt32(SurfaceGenerationOffset);
                var handles = new ulong[TextureCount];
                for (var index = 0; index < TextureCount; index++)
                    handles[index] = _surface.ReadUInt64(SharedHandlesOffset + index * sizeof(ulong));

                if (generation == 0 || handles.Any(handle => handle == 0))
                {
                    ResetGraphics();
                    return DiagnosticWaiting(
                        "waiting-for-handles",
                        "Waiting for the first mirror frame",
                        $"The layer is connected, but only {handles.Count(handle => handle != 0)}/{TextureCount} shared texture handles are published " +
                        $"(generation {generation}). Start or resume the VR app to publish an image. {ReadLayerSummary()}",
                        connected: true);
                }

                if (generation != _surfaceGeneration || !handles.SequenceEqual(_sharedHandles))
                {
                    ResetGraphics();
                    if (!OpenSharedTextures(generation, handles, out var openError))
                        return DiagnosticWaiting(
                            "shared-texture-open-failed",
                            "Mirror image is not available",
                            $"{openError} Generation {generation}; {ReadLayerSummary()}",
                            warning: true,
                            connected: true);
                }

                var latestFrame = _surface.ReadUInt32(LastProcessedIndexOffset);
                TrackFrameIndex(latestFrame);
                var textureIndex = (int)(latestFrame % TextureCount);
                var texture = _mirrorTextures[textureIndex]
                              ?? throw new InvalidOperationException("The selected mirror texture is not open.");
                var description = texture.Description;
                if (!IsSupportedPreviewFormat(description.Format))
                {
                    return DiagnosticWaiting(
                        "unsupported-format",
                        "Preview format is not supported",
                        $"The mirror is using {description.Format} at {description.Width} × {description.Height}. OBS capture is unaffected.",
                        warning: true,
                        connected: true);
                }

                RenderPreview(textureIndex);
                // Copy into the next ring slot, then map the oldest slot (the
                // copy issued two captures ago): that copy has long completed,
                // so Map returns without stalling on the GPU. The first two
                // captures after a (re)connect map a fresher slot and may
                // block once while the ring warms up. Displayed pixels are two
                // captures old - invisible at preview rates.
                var writeSlot = (int)(_readbackIssued % ReadbackRingDepth);
                _context!.CopyResource(_readbackRing[writeSlot]!, _downsampleTexture!);
                _context.Flush();
                _readbackIssued++;
                var mapSlot = _readbackIssued >= ReadbackRingDepth
                    ? (int)(_readbackIssued % ReadbackRingDepth)
                    : 0;
                var readback = _readbackRing[mapSlot]!;
                var mapped = _context.Map(
                    readback,
                    0,
                    MapMode.Read,
                    Vortice.Direct3D11.MapFlags.None);
                MirrorPreviewFrame frame;
                try
                {
                    frame = ConvertToPreview(
                        mapped,
                        readback.Description,
                        description.Width,
                        description.Height);
                }
                finally
                {
                    _context.Unmap(readback, 0);
                }

                var usedCpuFallback = false;
                var gpuStats = AnalyzePixels(frame.Pixels);
                if (!gpuStats.IsBlack)
                {
                    _gpuPreviewVerified = true;
                }
                else if (!_gpuPreviewVerified &&
                         Environment.TickCount64 - _lastCpuFallbackTick >= 1000)
                {
                    // The fallback is a full-resolution readback; keep it to
                    // once per second while unverified, or dark loading
                    // screens would trigger it on every tick.
                    _lastCpuFallbackTick = Environment.TickCount64;
                    frame = CaptureFullResolutionFallback(texture, description);
                    usedCpuFallback = true;
                }
                var finalStats = usedCpuFallback ? AnalyzePixels(frame.Pixels) : gpuStats;
                _captureCount++;
                _lastPixelSummary = finalStats.ToString();

                var now = Environment.TickCount64;
                var frameAge = _hasFrameIndex ? Math.Max(0, now - _lastFrameAdvanceTick) : 0;
                var blackFor = 0L;
                if (finalStats.IsBlack)
                {
                    if (_blackSinceTick == 0)
                        _blackSinceTick = now;
                    blackFor = Math.Max(0, now - _blackSinceTick);
                }
                else
                {
                    _blackSinceTick = 0;
                }

                var appName = ReadApplicationName();
                var path = usedCpuFallback ? "CPU fallback" : "GPU downsample";
                var frameContext =
                    $"frame {latestFrame} (slot {textureIndex}, age {frameAge / 1000.0:0.0}s), generation {generation}, " +
                    $"source {description.Width} × {description.Height} {description.Format}, preview {frame.Width} × {frame.Height}, " +
                    $"path {path}, pixels {finalStats}, adapter {_selectedAdapterName} ({_selectedAdapterLuid}); {ReadLayerSummary()}";

                string status = "Live mirror";
                var detailSuffix = usedCpuFallback ? "  •  CPU fallback" : string.Empty;
                if (finalStats.IsBlack && blackFor >= 2000)
                {
                    status = "Black frames detected";
                    detailSuffix += "  •  See Preview diagnostics";
                    var likelyCause = frameAge >= 3000
                        ? "The producer frame index is also stale, so the VR app/layer stopped feeding new frames."
                        : usedCpuFallback
                            ? "Both the GPU preview and direct CPU readback contain only black pixels; the layer is publishing black content or copying the wrong source texture."
                            : "Frames are advancing, but the sampled preview pixels are black. A direct CPU verification is attempted once per second.";
                    RecordDiagnostic(
                        frameAge >= 3000 ? "black-and-stale" : "black-frames",
                        $"{likelyCause} {frameContext}",
                        warning: true);
                }
                else if (frameAge >= 5000)
                {
                    status = "Mirror frame is stale";
                    detailSuffix += "  •  See Preview diagnostics";
                    RecordDiagnostic(
                        "stale-frame",
                        $"No new producer frame has been published for {frameAge / 1000.0:0.0}s. {frameContext}",
                        warning: true);
                }
                else if (usedCpuFallback && !finalStats.IsBlack)
                {
                    RecordDiagnostic(
                        "cpu-fallback-visible",
                        $"The GPU downsample was black, but the direct CPU readback contains visible pixels. The shared source is valid; investigate the Control Center preview shader/device path. {frameContext}",
                        warning: true);
                }
                else if (!finalStats.IsBlack)
                {
                    RecordDiagnostic("live-visible", $"Visible frames are arriving normally. {frameContext}");
                }
                else
                {
                    RecordDiagnostic(
                        "brief-dark-frame",
                        $"A dark frame was sampled for {blackFor / 1000.0:0.0}s; waiting before classifying it as a fault. {frameContext}",
                        writeLog: false);
                }

                return new MirrorPreviewResult(
                    frame,
                    status,
                    (appName.Length > 0 ? $"{appName}  •  " : string.Empty) +
                    $"Source {description.Width} × {description.Height}  •  Preview {frame.Width} × {frame.Height}" +
                    detailSuffix,
                    true,
                    true);
            }
            catch (Exception ex)
            {
                ResetGraphics();
                return DiagnosticWaiting(
                    "capture-exception",
                    "Preview paused",
                    $"{FriendlyError(ex)} {ReadLayerSummary()}",
                    warning: true,
                    connected: _surface is not null);
            }
        }
    }

    private bool EnsureSurface(out string error)
    {
        if (_surface is not null)
        {
            error = string.Empty;
            return true;
        }

        try
        {
            _mapping = MemoryMappedFile.OpenExisting(SharedMemoryName, MemoryMappedFileRights.ReadWrite);
            try
            {
                _surface = _mapping.CreateViewAccessor(0, FullSurfaceViewSize, MemoryMappedFileAccess.ReadWrite);
                _diagAvailable = true;
            }
            catch (Exception)
            {
                // Sections created by pre-diagnostics layers may be smaller.
                _surface = _mapping.CreateViewAccessor(0, LegacySurfaceSize, MemoryMappedFileAccess.ReadWrite);
                _diagAvailable = false;
            }

            if (_diagAvailable)
            {
                // Identify this consumer so the layer log can tell the
                // dashboard preview apart from the real OBS plugin.
                _surface.Write(DiagPreviewPidOffset, (uint)Environment.ProcessId);
                _surface.Write(DiagPreviewMagicOffset, DiagnosticsMagic);
            }
            error = string.Empty;
            return true;
        }
        catch (FileNotFoundException)
        {
            ResetSurface();
            error = "Start a VR app after enabling the OpenXR layer. The preview will connect automatically.";
            return false;
        }
        catch (Exception ex)
        {
            ResetSurface();
            error = FriendlyError(ex);
            return false;
        }
    }

    private bool OpenSharedTextures(uint generation, ulong[] handles, out string error)
    {
        using var factory = CreateDXGIFactory1<IDXGIFactory1>();
        var featureLevels = new[]
        {
            FeatureLevel.Level_11_1,
            FeatureLevel.Level_11_0,
            FeatureLevel.Level_10_1,
            FeatureLevel.Level_10_0
        };
        string? lastError = null;
        var shaderFlags = ShaderFlags.EnableStrictness | ShaderFlags.OptimizationLevel3;
        var vertexBytecode = Compiler.Compile(
            PreviewShaderSource, "VSMain", "MirrorPreview.hlsl", "vs_5_0", shaderFlags, EffectFlags.None);
        var pixelBytecode = Compiler.Compile(
            PreviewShaderSource, "PSMain", "MirrorPreview.hlsl", "ps_5_0", shaderFlags, EffectFlags.None);

        var preferred = ReadLayerAdapterLuid();
        var adapters = new List<IDXGIAdapter1>();
        for (uint adapterIndex = 0; ; adapterIndex++)
        {
            if (factory.EnumAdapters1(adapterIndex, out var adapter).Failure)
                break;
            adapters.Add(adapter);
        }
        // The layer publishes which adapter the game renders on; the shared
        // textures can only open there, so try that adapter first instead of
        // probing every GPU (wrong-adapter opens can succeed but sample black).
        if (preferred.Known)
        {
            adapters.Sort((left, right) =>
                MatchesLayerAdapter(right, preferred).CompareTo(MatchesLayerAdapter(left, preferred)));
        }

        try
        {
            foreach (var adapter in adapters)
            {
                ID3D11Device? candidateDevice = null;
                ID3D11DeviceContext? candidateContext = null;
                var candidateTextures = new ID3D11Texture2D?[TextureCount];
                var candidateViews = new ID3D11ShaderResourceView?[TextureCount];
                ID3D11Texture2D? candidateDownsample = null;
                ID3D11RenderTargetView? candidateDownsampleTargetView = null;
                ID3D11VertexShader? candidateVertexShader = null;
                ID3D11PixelShader? candidatePixelShader = null;
                ID3D11SamplerState? candidateSampler = null;
                var candidateReadbacks = new ID3D11Texture2D?[ReadbackRingDepth];
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
                    {
                        lastError = $"Direct3D device creation failed on {adapter.Description1.Description}.";
                        continue;
                    }

                    for (var index = 0; index < TextureCount; index++)
                    {
                        candidateTextures[index] = candidateDevice.OpenSharedResource<ID3D11Texture2D>(
                            unchecked((nint)handles[index]));
                        candidateViews[index] = candidateDevice.CreateShaderResourceView(candidateTextures[index]!);
                    }

                    var sourceDescription = candidateTextures[0]!.Description;
                    if (sourceDescription.Width == 0 || sourceDescription.Height == 0)
                        throw new InvalidOperationException("The published mirror texture has no size.");
                    if (candidateTextures.Skip(1).Any(texture =>
                            texture!.Description.Width != sourceDescription.Width ||
                            texture.Description.Height != sourceDescription.Height ||
                            texture.Description.Format != sourceDescription.Format))
                    {
                        throw new InvalidOperationException("The mirror texture ring has inconsistent resources.");
                    }

                    var (previewWidth, previewHeight) = CalculatePreviewSize(
                        sourceDescription.Width,
                        sourceDescription.Height);
                    var downsampleDescription = new Texture2DDescription(
                        Format.B8G8R8A8_UNorm,
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
                    candidateDownsample = candidateDevice.CreateTexture2D(in downsampleDescription);
                    candidateDownsampleTargetView = candidateDevice.CreateRenderTargetView(candidateDownsample);
                    candidateVertexShader = candidateDevice.CreateVertexShader(vertexBytecode.Span, null);
                    candidatePixelShader = candidateDevice.CreatePixelShader(pixelBytecode.Span, null);
                    candidateSampler = candidateDevice.CreateSamplerState(new SamplerDescription(
                        Filter.MinMagLinearMipPoint,
                        TextureAddressMode.Clamp,
                        0.0f,
                        1,
                        ComparisonFunction.Never,
                        0.0f,
                        float.MaxValue));

                    var readbackDescription = new Texture2DDescription(
                        Format.B8G8R8A8_UNorm,
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
                        candidateReadbacks[index] = candidateDevice.CreateTexture2D(in readbackDescription);

                    _device = candidateDevice;
                    _context = candidateContext;
                    _downsampleTexture = candidateDownsample;
                    _downsampleTargetView = candidateDownsampleTargetView;
                    _previewVertexShader = candidateVertexShader;
                    _previewPixelShader = candidatePixelShader;
                    _previewSampler = candidateSampler;
                    for (var index = 0; index < ReadbackRingDepth; index++)
                        _readbackRing[index] = candidateReadbacks[index];
                    _readbackIssued = 0;
                    for (var index = 0; index < TextureCount; index++)
                    {
                        _mirrorTextures[index] = candidateTextures[index];
                        _mirrorViews[index] = candidateViews[index];
                    }
                    _selectedAdapterName = adapter.Description1.Description.Trim();
                    var selectedLuid = adapter.Description1.Luid;
                    _selectedAdapterLuid = $"{selectedLuid.HighPart:X8}:{selectedLuid.LowPart:X8}";
                    _surfaceGeneration = generation;
                    handles.CopyTo(_sharedHandles, 0);
                    error = string.Empty;
                    return true;
                }
                catch (Exception ex)
                {
                    lastError = $"{adapter.Description1.Description}: {FriendlyError(ex)}";
                    foreach (var readback in candidateReadbacks)
                        readback?.Dispose();
                    candidateSampler?.Dispose();
                    candidatePixelShader?.Dispose();
                    candidateVertexShader?.Dispose();
                    candidateDownsampleTargetView?.Dispose();
                    candidateDownsample?.Dispose();
                    foreach (var view in candidateViews)
                        view?.Dispose();
                    foreach (var texture in candidateTextures)
                        texture?.Dispose();
                    candidateContext?.Dispose();
                    candidateDevice?.Dispose();
                }
            }
        }
        finally
        {
            foreach (var adapter in adapters)
                adapter.Dispose();
        }

        error = string.IsNullOrWhiteSpace(lastError)
            ? "No Direct3D adapter could open the shared mirror texture."
            : $"No Direct3D adapter could open the shared mirror texture. {lastError}";
        return false;
    }

    private static bool IsSupportedPreviewFormat(Format format) => format is
        Format.B8G8R8A8_UNorm or
        Format.B8G8R8X8_UNorm or
        Format.R8G8B8A8_UNorm or
        Format.R10G10B10A2_UNorm or
        Format.R16G16B16A16_UNorm or
        Format.B5G6R5_UNorm or
        Format.B5G5R5A1_UNorm or
        Format.B4G4R4A4_UNorm;

    private void RenderPreview(int textureIndex)
    {
        var previewDescription = _downsampleTexture!.Description;
        _context!.OMSetRenderTargets(_downsampleTargetView!, null);
        _context.RSSetViewports([
            new Viewport(previewDescription.Width, previewDescription.Height)
        ]);
        _context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        _context.VSSetShader(_previewVertexShader!);
        _context.PSSetShader(_previewPixelShader!);
        _context.PSSetShaderResources(0, [_mirrorViews[textureIndex]!]);
        _context.PSSetSamplers(0, [_previewSampler!]);
        _context.Draw(3, 0);

        // Do not leave resources bound across the copy or the next ring slot.
        _context.PSSetShaderResources(0, [null!]);
        _context.OMSetRenderTargets(Array.Empty<ID3D11RenderTargetView>(), null);
    }

    private MirrorPreviewFrame CaptureFullResolutionFallback(
        ID3D11Texture2D sourceTexture,
        Texture2DDescription sourceDescription)
    {
        if (_fullReadbackTexture is null)
        {
            var description = new Texture2DDescription(
                sourceDescription.Format,
                sourceDescription.Width,
                sourceDescription.Height,
                1,
                1,
                BindFlags.None,
                ResourceUsage.Staging,
                CpuAccessFlags.Read,
                1,
                0,
                ResourceOptionFlags.None);
            _fullReadbackTexture = _device!.CreateTexture2D(in description);
        }

        _context!.CopyResource(_fullReadbackTexture, sourceTexture);
        _context.Flush();
        var mapped = _context.Map(
            _fullReadbackTexture,
            0,
            MapMode.Read,
            Vortice.Direct3D11.MapFlags.None);
        try
        {
            return DownsampleOnCpu(mapped, sourceDescription);
        }
        finally
        {
            _context.Unmap(_fullReadbackTexture, 0);
        }
    }

    private readonly record struct PixelStatistics(
        int Samples,
        int VisibleSamples,
        byte MaximumChannel,
        double AverageLuma)
    {
        public bool IsBlack => VisibleSamples == 0;

        public override string ToString() =>
            $"{VisibleSamples}/{Samples} visible samples ({(Samples == 0 ? 0 : VisibleSamples * 100.0 / Samples):0.0}%), " +
            $"max channel {MaximumChannel}, average luma {AverageLuma:0.0}/255";
    }

    private static PixelStatistics AnalyzePixels(byte[] pixels)
    {
        var pixelCount = pixels.Length / 4;
        if (pixelCount == 0)
            return new PixelStatistics(0, 0, 0, 0);

        var stride = Math.Max(1, pixelCount / 8192);
        var samples = 0;
        var visible = 0;
        byte maximum = 0;
        double lumaTotal = 0;
        for (var pixel = 0; pixel < pixelCount; pixel += stride)
        {
            var index = pixel * 4;
            var blue = pixels[index];
            var green = pixels[index + 1];
            var red = pixels[index + 2];
            var sampleMaximum = Math.Max(red, Math.Max(green, blue));
            if (sampleMaximum > 2)
                visible++;
            maximum = Math.Max(maximum, sampleMaximum);
            lumaTotal += red * 0.2126 + green * 0.7152 + blue * 0.0722;
            samples++;
        }

        return new PixelStatistics(samples, visible, maximum, lumaTotal / samples);
    }

    private static (uint Width, uint Height) CalculatePreviewSize(uint width, uint height)
    {
        var scale = Math.Min(
            1.0,
            Math.Min(
                PreviewMaxWidth / (double)width,
                PreviewMaxHeight / (double)height));
        return (
            Math.Max(1u, (uint)Math.Round(width * scale)),
            Math.Max(1u, (uint)Math.Round(height * scale)));
    }

    private static unsafe MirrorPreviewFrame ConvertToPreview(
        MappedSubresource mapped,
        Texture2DDescription description,
        uint sourceWidth,
        uint sourceHeight)
    {
        var width = checked((int)description.Width);
        var height = checked((int)description.Height);
        var pixels = GC.AllocateUninitializedArray<byte>(checked(width * height * 4));

        fixed (byte* outputStart = pixels)
        {
            if (description.Format == Format.B8G8R8A8_UNorm)
            {
                for (var y = 0; y < height; y++)
                {
                    var sourceRow = (uint*)((byte*)mapped.DataPointer + y * mapped.RowPitch);
                    var outputRow = (uint*)(outputStart + y * width * 4);
                    for (var x = 0; x < width; x++)
                        outputRow[x] = sourceRow[x] | 0xff000000u;
                }
                return new MirrorPreviewFrame(pixels, width, height, sourceWidth, sourceHeight);
            }

            for (var y = 0; y < height; y++)
            {
                var sourceRow = (byte*)mapped.DataPointer + y * mapped.RowPitch;
                var outputRow = outputStart + y * width * 4;

                for (var x = 0; x < width; x++)
                    WriteBgraPixel(outputRow + x * 4, sourceRow, x, description.Format);
            }
        }

        return new MirrorPreviewFrame(pixels, width, height, sourceWidth, sourceHeight);
    }

    private static unsafe MirrorPreviewFrame DownsampleOnCpu(
        MappedSubresource mapped,
        Texture2DDescription description)
    {
        var scale = Math.Min(
            1.0,
            Math.Min(
                PreviewMaxWidth / (double)description.Width,
                PreviewMaxHeight / (double)description.Height));
        var width = Math.Max(1, (int)Math.Round(description.Width * scale));
        var height = Math.Max(1, (int)Math.Round(description.Height * scale));
        var pixels = GC.AllocateUninitializedArray<byte>(checked(width * height * 4));

        fixed (byte* outputStart = pixels)
        {
            for (var y = 0; y < height; y++)
            {
                var sourceY = Math.Min(
                    (int)description.Height - 1,
                    (int)((y + 0.5) * description.Height / height));
                var sourceRow = (byte*)mapped.DataPointer + sourceY * mapped.RowPitch;
                var outputRow = outputStart + y * width * 4;

                for (var x = 0; x < width; x++)
                {
                    var sourceX = Math.Min(
                        (int)description.Width - 1,
                        (int)((x + 0.5) * description.Width / width));
                    WriteBgraPixel(outputRow + x * 4, sourceRow, sourceX, description.Format);
                }
            }
        }

        return new MirrorPreviewFrame(pixels, width, height, description.Width, description.Height);
    }

    private static unsafe void WriteBgraPixel(byte* output, byte* row, int x, Format format)
    {
        switch (format)
        {
            case Format.B8G8R8A8_UNorm:
            case Format.B8G8R8X8_UNorm:
            {
                var source = row + x * 4;
                output[0] = source[0];
                output[1] = source[1];
                output[2] = source[2];
                output[3] = 255;
                return;
            }
            case Format.R8G8B8A8_UNorm:
            {
                var source = row + x * 4;
                output[0] = source[2];
                output[1] = source[1];
                output[2] = source[0];
                output[3] = 255;
                return;
            }
            case Format.R16G16B16A16_UNorm:
            {
                var source = (ushort*)row + x * 4;
                output[0] = (byte)(source[2] >> 8);
                output[1] = (byte)(source[1] >> 8);
                output[2] = (byte)(source[0] >> 8);
                output[3] = 255;
                return;
            }
            case Format.R10G10B10A2_UNorm:
            {
                var packed = *((uint*)row + x);
                output[2] = ScaleToByte(packed & 0x3ff, 0x3ff);
                output[1] = ScaleToByte((packed >> 10) & 0x3ff, 0x3ff);
                output[0] = ScaleToByte((packed >> 20) & 0x3ff, 0x3ff);
                output[3] = 255;
                return;
            }
            case Format.B5G6R5_UNorm:
            {
                var packed = *((ushort*)row + x);
                output[0] = ScaleToByte((uint)(packed & 0x1f), 0x1f);
                output[1] = ScaleToByte((uint)((packed >> 5) & 0x3f), 0x3f);
                output[2] = ScaleToByte((uint)((packed >> 11) & 0x1f), 0x1f);
                output[3] = 255;
                return;
            }
            case Format.B5G5R5A1_UNorm:
            {
                var packed = *((ushort*)row + x);
                output[0] = ScaleToByte((uint)(packed & 0x1f), 0x1f);
                output[1] = ScaleToByte((uint)((packed >> 5) & 0x1f), 0x1f);
                output[2] = ScaleToByte((uint)((packed >> 10) & 0x1f), 0x1f);
                output[3] = 255;
                return;
            }
            case Format.B4G4R4A4_UNorm:
            {
                var packed = *((ushort*)row + x);
                output[0] = ScaleToByte((uint)(packed & 0xf), 0xf);
                output[1] = ScaleToByte((uint)((packed >> 4) & 0xf), 0xf);
                output[2] = ScaleToByte((uint)((packed >> 8) & 0xf), 0xf);
                output[3] = 255;
                return;
            }
            default:
                output[0] = output[1] = output[2] = 0;
                output[3] = 255;
                return;
        }
    }

    private static byte ScaleToByte(uint value, uint maximum) =>
        (byte)((value * 255u + maximum / 2u) / maximum);

    private MirrorPreviewResult DiagnosticWaiting(
        string key,
        string status,
        string detail,
        bool warning = false,
        bool connected = false)
    {
        RecordDiagnostic(key, $"{status}: {detail}", warning);
        return new MirrorPreviewResult(null, status, detail, false, connected);
    }

    private void TrackFrameIndex(uint frameIndex)
    {
        var now = Environment.TickCount64;
        if (!_hasFrameIndex || frameIndex != _lastFrameIndex)
        {
            _hasFrameIndex = true;
            _lastFrameIndex = frameIndex;
            _lastFrameAdvanceTick = now;
        }
    }

    private string ReadLayerSummary()
    {
        if (!_diagAvailable || _surface is null)
            return "Layer diagnostics are unavailable (the running layer may predate diagnostics).";

        try
        {
            if (_surface.ReadUInt32(DiagLayerMagicOffset) != DiagnosticsMagic)
                return "The shared surface is mapped, but the layer has not stamped its diagnostic block.";

            var version = ReadUtf8(DiagLayerVersionStringOffset, DiagLayerVersionStringLength);
            var application = ReadUtf8(DiagApplicationNameOffset, DiagApplicationNameLength);
            var pid = _surface.ReadUInt32(DiagLayerPidOffset);
            var heartbeat = _surface.ReadUInt32(DiagLayerHeartbeatOffset);
            var width = _surface.ReadUInt32(DiagMirrorWidthOffset);
            var height = _surface.ReadUInt32(DiagMirrorHeightOffset);
            var format = _surface.ReadUInt32(DiagMirrorFormatOffset);
            var low = _surface.ReadUInt32(DiagLayerAdapterLuidLowOffset);
            var high = _surface.ReadInt32(DiagLayerAdapterLuidHighOffset);
            return
                $"Layer app '{(application.Length > 0 ? application : "unknown")}', version {(version.Length > 0 ? version : "unknown")}, " +
                $"diagnostics v{_surface.ReadUInt32(DiagLayerVersionOffset)}, pid {pid}, heartbeat {heartbeat}, " +
                $"published mirror {width} × {height} format {format}, adapter {high:X8}:{low:X8}.";
        }
        catch (Exception ex)
        {
            return $"Layer diagnostics could not be read: {FriendlyError(ex)}";
        }
    }

    private string ReadUtf8(int offset, int length)
    {
        var bytes = new byte[length];
        _surface!.ReadArray(offset, bytes, 0, bytes.Length);
        var terminator = Array.IndexOf(bytes, (byte)0);
        return Encoding.UTF8.GetString(bytes, 0, terminator < 0 ? bytes.Length : terminator).Trim();
    }

    private void RecordDiagnostic(
        string key,
        string message,
        bool warning = false,
        bool forceLog = false,
        bool writeLog = true)
    {
        var now = Environment.TickCount64;
        var stateChanged = key != _lastDiagnosticKey;
        var shouldWrite = writeLog &&
                          (forceLog || stateChanged || now - _lastDiagnosticLogTick >= 30000);
        _lastDiagnosticKey = key;
        _lastDiagnosticMessage = message;
        if (stateChanged || forceLog)
            _lastDiagnosticAt = DateTime.Now;

        if (!shouldWrite)
            return;

        _lastDiagnosticLogTick = now;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
            var oneLine = message.Replace('\r', ' ').Replace('\n', ' ');
            File.AppendAllText(
                LogPath,
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff zzz} [{(warning ? "WARN" : "INFO")}] {key}: {oneLine}{Environment.NewLine}");
        }
        catch
        {
            // Logging must never interrupt the live preview.
        }
    }

    public string GetDiagnosticsReport()
    {
        lock (_gate)
        {
            var frameAge = _hasFrameIndex
                ? Math.Max(0, Environment.TickCount64 - _lastFrameAdvanceTick) / 1000.0
                : double.NaN;
            var report = new StringBuilder();
            report.AppendLine("[Mirror preview diagnostics]");
            report.AppendLine($"Captured: {DateTime.Now:yyyy-MM-dd HH:mm:ss zzz}");
            report.AppendLine($"State: {_lastDiagnosticKey}");
            report.AppendLine($"State since: {_lastDiagnosticAt:yyyy-MM-dd HH:mm:ss.fff zzz}");
            report.AppendLine($"Detail: {_lastDiagnosticMessage}");
            report.AppendLine($"Successful pixel samples: {_captureCount}");
            report.AppendLine($"Shared surface mapped: {_surface is not null}");
            report.AppendLine($"Extended diagnostics available: {_diagAvailable}");
            report.AppendLine($"Surface generation: {_surfaceGeneration}");
            report.AppendLine($"Latest frame index: {(_hasFrameIndex ? _lastFrameIndex.ToString() : "not observed")}");
            report.AppendLine($"Seconds since frame advance: {(double.IsNaN(frameAge) ? "not observed" : frameAge.ToString("0.0"))}");
            report.AppendLine($"Selected preview adapter: {_selectedAdapterName} ({_selectedAdapterLuid})");
            report.AppendLine($"Last pixel sample: {_lastPixelSummary}");
            report.AppendLine(ReadLayerSummary());
            report.AppendLine($"Preview log: {LogPath}");
            return report.ToString();
        }
    }

    public MirrorProducerIdentity GetProducerIdentity()
    {
        lock (_gate)
        {
            if (_surface is null)
                return new MirrorProducerIdentity(false, 0, string.Empty);

            if (!_diagAvailable || _surface.ReadUInt32(DiagLayerMagicOffset) != DiagnosticsMagic)
                return new MirrorProducerIdentity(true, 0, string.Empty);

            var pid = _surface.ReadUInt32(DiagLayerPidOffset);
            var application = ReadUtf8(DiagApplicationNameOffset, DiagApplicationNameLength);
            return new MirrorProducerIdentity(pid == 0 || IsProcessAlive(pid), pid, application);
        }
    }

    private static bool IsProcessAlive(uint pid)
    {
        try
        {
            using var process = Process.GetProcessById(checked((int)pid));
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch
        {
            // Access-denied and transient query failures do not prove exit.
            return true;
        }
    }

    public string GetLog()
    {
        try
        {
            if (!File.Exists(LogPath))
                return "The preview diagnostics log has not been created yet.";
            using var stream = new FileStream(LogPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream);
            var lines = new Queue<string>();
            while (reader.ReadLine() is { } line)
            {
                lines.Enqueue(line);
                if (lines.Count > 320)
                    lines.Dequeue();
            }
            return string.Join(Environment.NewLine, lines);
        }
        catch (IOException ex)
        {
            return $"The preview diagnostics log is currently busy: {ex.Message}";
        }
    }

    private static string FriendlyError(Exception ex) =>
        string.IsNullOrWhiteSpace(ex.Message) ? ex.GetType().Name : ex.Message;

    private void ResetGraphics()
    {
        foreach (var readback in _readbackRing)
            readback?.Dispose();
        Array.Clear(_readbackRing);
        _readbackIssued = 0;
        _fullReadbackTexture?.Dispose();
        _fullReadbackTexture = null;
        _previewSampler?.Dispose();
        _previewSampler = null;
        _previewPixelShader?.Dispose();
        _previewPixelShader = null;
        _previewVertexShader?.Dispose();
        _previewVertexShader = null;
        _downsampleTargetView?.Dispose();
        _downsampleTargetView = null;
        _downsampleTexture?.Dispose();
        _downsampleTexture = null;
        foreach (var view in _mirrorViews)
            view?.Dispose();
        Array.Clear(_mirrorViews);
        foreach (var texture in _mirrorTextures)
            texture?.Dispose();
        Array.Clear(_mirrorTextures);
        _context?.Dispose();
        _context = null;
        _device?.Dispose();
        _device = null;
        _surfaceGeneration = 0;
        _gpuPreviewVerified = false;
        Array.Clear(_sharedHandles);
    }

    /// <summary>
    /// The layer treats frameNumber changes as its consumer heartbeat;
    /// Interlocked so a concurrent OBS plugin increment is never lost.
    /// </summary>
    private unsafe void IncrementConsumerHeartbeat()
    {
        byte* pointer = null;
        _surface!.SafeMemoryMappedViewHandle.AcquirePointer(ref pointer);
        try
        {
            Interlocked.Increment(ref *(int*)(pointer + FrameNumberOffset));
        }
        finally
        {
            _surface.SafeMemoryMappedViewHandle.ReleasePointer();
        }
    }

    private string ReadApplicationName()
    {
        if (!_diagAvailable || _surface!.ReadUInt32(DiagLayerMagicOffset) != DiagnosticsMagic)
            return string.Empty;
        var bytes = new byte[DiagApplicationNameLength];
        _surface.ReadArray(DiagApplicationNameOffset, bytes, 0, bytes.Length);
        var terminator = Array.IndexOf(bytes, (byte)0);
        return Encoding.UTF8.GetString(bytes, 0, terminator < 0 ? bytes.Length : terminator).Trim();
    }

    private (uint Low, int High, bool Known) ReadLayerAdapterLuid()
    {
        if (!_diagAvailable || _surface!.ReadUInt32(DiagLayerMagicOffset) != DiagnosticsMagic)
            return (0, 0, false);
        var low = _surface.ReadUInt32(DiagLayerAdapterLuidLowOffset);
        var high = _surface.ReadInt32(DiagLayerAdapterLuidHighOffset);
        return (low, high, low != 0 || high != 0);
    }

    private static bool MatchesLayerAdapter(IDXGIAdapter1 adapter, (uint Low, int High, bool Known) preferred)
    {
        var luid = adapter.Description1.Luid;
        return luid.LowPart == preferred.Low && luid.HighPart == preferred.High;
    }

    private void ResetSurface()
    {
        ResetGraphics();
        if (_surface is not null && _diagAvailable)
        {
            // Withdraw our identity so the layer's consumer log stays accurate.
            _surface.Write(DiagPreviewMagicOffset, 0u);
        }
        _diagAvailable = false;
        _hasFrameIndex = false;
        _lastFrameAdvanceTick = 0;
        _blackSinceTick = 0;
        _surface?.Dispose();
        _surface = null;
        _mapping?.Dispose();
        _mapping = null;
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
                return;
            _disposed = true;
            ResetSurface();
        }
    }
}
