using OBSMirror.ControlCenter.Services;
using Xunit;

namespace OBSMirror.ControlCenter.Tests;

public sealed class VrModuleDetectorTests
{
    [Theory]
    [InlineData("openxr_loader.dll")]
    [InlineData("XR_APILAYER_NOVENDOR_OBSMirror.dll")]
    [InlineData("virtualdesktop-openxr.dll")]
    [InlineData("openxr-oculus-compatibility.dll")]
    [InlineData("OpenCompositeInput.dll")]
    [InlineData("OpenComposite.dll")]
    public void RecognizesOpenXrProofModules(string moduleName)
    {
        var result = VrModuleDetector.Analyze([moduleName]);

        Assert.True(result.OpenXrPath);
        Assert.Empty(result.NonOpenXrPath);
    }

    [Fact]
    public void RecognizesOcuUsingItsStaticLoaderThroughVdxr()
    {
        var result = VrModuleDetector.Analyze([
            "SkyrimVR.exe",
            "openvr_api.dll",
            "OpenCompositeInput.dll",
            "virtualdesktop-openxr.dll"
        ]);

        Assert.True(result.OpenXrPath);
        Assert.Empty(result.NonOpenXrPath);
    }

    [Fact]
    public void OpenXrProofWinsOverASecondaryLibOvrModule()
    {
        var result = VrModuleDetector.Analyze([
            "openvr_api.dll",
            "OpenCompositeInput.dll",
            "LibOVRRT64_1.dll"
        ]);

        Assert.True(result.OpenXrPath);
        Assert.Empty(result.NonOpenXrPath);
    }

    [Fact]
    public void KeepsDirectLibOvrClassifiedAsOculus()
    {
        var result = VrModuleDetector.Analyze(["LibOVRRT64_1.dll"]);

        Assert.False(result.OpenXrPath);
        Assert.Equal("Oculus/LibOVR", result.NonOpenXrPath);
    }

    [Fact]
    public void KeepsSteamVrFallbackClassification()
    {
        var result = VrModuleDetector.Analyze(["openvr_api.dll", "vrclient_x64.dll"]);

        Assert.False(result.OpenXrPath);
        Assert.Equal("OpenVR/SteamVR", result.NonOpenXrPath);
    }

    [Fact]
    public void DoesNotTreatOpenVrApiAloneAsAnActiveVrPath()
    {
        var result = VrModuleDetector.Analyze(["openvr_api.dll"]);

        Assert.False(result.OpenXrPath);
        Assert.Empty(result.NonOpenXrPath);
    }
}
