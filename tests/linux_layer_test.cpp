#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include "vulkan_test_support.h"
#include "frame_ipc.h"
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <filesystem>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo*,
                                                                  const char*,
                                                                  XrNegotiateApiLayerRequest*);

namespace {
    const auto instance = reinterpret_cast<XrInstance>(0x1234);
    const auto session = reinterpret_cast<XrSession>(0x2345);
    const auto chain = reinterpret_cast<XrSwapchain>(0x3456);
    Source* source = nullptr;
    bool waited = false, timeout = false, rejectTransfer = false;
    uint32_t acquireIndex = 0, releases = 0, endFrames = 0, creates = 0;
    XrSwapchainUsageFlags lastUsage = 0;
    XrResult XRAPI_CALL createRuntime(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo* next, XrInstance* out) {
        require(!next->nextInfo, "layer chain not advanced");
        *out = instance;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL createSession(XrInstance, const XrSessionCreateInfo*, XrSession* out) {
        *out = session;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL destroyInstance(XrInstance) {
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL destroySession(XrSession) {
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL destroySwapchain(XrSwapchain) {
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL createSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
        ++creates;
        lastUsage = info->usageFlags;
        if (rejectTransfer && (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT))
            return XR_ERROR_FEATURE_UNSUPPORTED;
        *out = chain;
        acquireIndex = 0;
        waited = false;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL enumerate(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
        *count = 2;
        if (capacity == 0)
            return XR_SUCCESS;
        if (capacity < *count)
            return XR_ERROR_SIZE_INSUFFICIENT;
        for (uint32_t i = 0; i < *count; ++i)
            reinterpret_cast<XrSwapchainImageVulkanKHR*>(images)[i].image = source->image;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
        *index = acquireIndex++ % 2;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL wait(XrSwapchain, const XrSwapchainImageWaitInfo*) {
        if (timeout)
            return XR_TIMEOUT_EXPIRED;
        waited = true;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL release(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
        if (!waited)
            return XR_ERROR_CALL_ORDER_INVALID;
        ++releases;
        waited = false;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL end(XrSession, const XrFrameEndInfo*) {
        ++endFrames;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL passthrough(XrInstance, const XrSystemGetInfo*, XrSystemId*) {
        return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    XrResult XRAPI_CALL runtimeProc(XrInstance, const char* name, PFN_xrVoidFunction* out) {
        *out = nullptr;
#define PROC(api, impl)                                                                                                \
    if (std::strcmp(name, #api) == 0)                                                                                  \
    *out = reinterpret_cast<PFN_xrVoidFunction>(impl)
        PROC(xrCreateSession, createSession);
        PROC(xrDestroySession, destroySession);
        PROC(xrDestroyInstance, destroyInstance);
        PROC(xrCreateSwapchain, createSwapchain);
        PROC(xrDestroySwapchain, destroySwapchain);
        PROC(xrEnumerateSwapchainImages, enumerate);
        PROC(xrAcquireSwapchainImage, acquire);
        PROC(xrWaitSwapchainImage, wait);
        PROC(xrReleaseSwapchainImage, release);
        PROC(xrEndFrame, end);
        PROC(xrGetSystem, passthrough);
#undef PROC
        return *out ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    void xrCheck(XrResult result) {
        require(XR_SUCCEEDED(result), "OpenXR call failed");
    }
    template <class T>
    T proc(const XrNegotiateApiLayerRequest& api, const char* name) {
        PFN_xrVoidFunction result = nullptr;
        xrCheck(api.getInstanceProcAddr(instance, name, &result));
        require(result, "missing OpenXR entry point");
        return reinterpret_cast<T>(result);
    }
#define CALL(api) proc<PFN_##api>(request, #api)
} // namespace

int main() {
    char directory[] = "/tmp/obs-layer-test-XXXXXX";
    if (!mkdtemp(directory))
        return 1;
    setenv("OPENXR_OBS_MIRROR_DIR", directory, 1);
    try {
        Fixture f;
        Source image(f, VK_FORMAT_R8G8B8A8_SRGB, 3, 2, VK_SAMPLE_COUNT_1_BIT);
        source = &image;
        XrNegotiateLoaderInfo loader{};
        loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader.structSize = sizeof(loader);
        loader.minInterfaceVersion = 1;
        loader.maxInterfaceVersion = 1;
        loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader.maxApiVersion = XR_CURRENT_API_VERSION;
        XrNegotiateApiLayerRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
        request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        require(XR_FAILED(xrNegotiateLoaderApiLayerInterface(nullptr, "wrong", &request)),
                "invalid negotiation accepted");
        xrCheck(xrNegotiateLoaderApiLayerInterface(&loader, "XR_APILAYER_NOVENDOR_OBSMirror", &request));
        XrApiLayerNextInfo next{};
        next.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
        next.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
        next.structSize = sizeof(next);
        std::strcpy(next.layerName, "XR_APILAYER_NOVENDOR_OBSMirror");
        next.nextGetInstanceProcAddr = runtimeProc;
        next.nextCreateApiLayerInstance = createRuntime;
        XrApiLayerCreateInfo layer{};
        layer.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
        layer.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
        layer.structSize = sizeof(layer);
        layer.nextInfo = &next;
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        XrInstance created;
        xrCheck(request.createApiLayerInstance(&create, &layer, &created));
        require(created == instance, "runtime instance not preserved");
        require(CALL(xrGetSystem)(instance, nullptr, nullptr) == XR_ERROR_FORM_FACTOR_UNAVAILABLE,
                "pass-through changed result");
        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = f.instance;
        binding.physicalDevice = f.physical;
        binding.device = f.device;
        binding.queueFamilyIndex = f.dispatch->queueFamily;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        XrSession createdSession;
        xrCheck(CALL(xrCreateSession)(instance, &sessionInfo, &createdSession));
        XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chainInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        chainInfo.width = 17;
        chainInfo.height = 9;
        chainInfo.arraySize = 2;
        chainInfo.faceCount = 1;
        chainInfo.mipCount = 3;
        chainInfo.sampleCount = 1;
        chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        XrSwapchain createdChain;
        xrCheck(CALL(xrCreateSwapchain)(session, &chainInfo, &createdChain));
        require(lastUsage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT, "transfer usage was not requested");
        uint32_t count;
        xrCheck(CALL(xrEnumerateSwapchainImages)(chain, 0, &count, nullptr));
        std::array<XrSwapchainImageVulkanKHR, 2> images{
            {{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}}};
        require(CALL(xrEnumerateSwapchainImages)(
                    chain, 1, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) ==
                    XR_ERROR_SIZE_INSUFFICIENT,
                "enumeration error lost");
        xrCheck(CALL(xrEnumerateSwapchainImages)(
            chain, 2, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
        xrCheck(CALL(xrEnumerateSwapchainImages)(
            chain, 2, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
        XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        views[0].subImage = {chain, {{1, 2}, {5, 4}}, 0};
        views[1].subImage = {chain, {{3, 1}, {7, 5}}, 1};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = 2;
        projection.views = views;
        const XrCompositionLayerBaseHeader* base = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
        frame.layerCount = 1;
        frame.layers = &base;
        uint32_t index;
        xrCheck(CALL(xrAcquireSwapchainImage)(chain, nullptr, &index));
        xrCheck(CALL(xrAcquireSwapchainImage)(chain, nullptr, &index));
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        timeout = true;
        require(CALL(xrWaitSwapchainImage)(chain, &waitInfo) == XR_TIMEOUT_EXPIRED, "timeout lost");
        require(CALL(xrReleaseSwapchainImage)(chain, nullptr) == XR_ERROR_CALL_ORDER_INVALID, "release error lost");
        xrCheck(CALL(xrEndFrame)(session, &frame));
        require(linux_mirror::newestFrame().empty(), "image accessed before successful wait");
        timeout = false;
        xrCheck(CALL(xrWaitSwapchainImage)(chain, &waitInfo));
        image.render(false);
        xrCheck(CALL(xrReleaseSwapchainImage)(chain, nullptr));
        check(vkQueueWaitIdle(f.dispatch->queue));
        xrCheck(CALL(xrEndFrame)(session, &frame));
        linux_mirror::Frame captured;
        const auto path = linux_mirror::newestFrame();
        require(!path.empty() && linux_mirror::readFrame(path, captured), "frame not published");
        require(captured.header.width == 12 && captured.header.height == 5 && captured.header.leftWidth == 5,
                "projection rectangles or stereo dimensions lost");
        require(captured.pixels[0] == 255 && captured.pixels[1] == 0 && captured.pixels[5 * 4 + 1] == 255,
                "eye array slices/channel ordering incorrect");
        require(captured.pixels[4 * captured.header.stride] == 0, "short eye not padded");
        require(!linux_mirror::readFrame(path, captured, captured.header.sequence), "duplicate frame not filtered");
        const int fd = open(path.c_str(), O_RDONLY);
        require(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0, "test lock failed");
        require(!linux_mirror::readFrame(path, captured), "reader ignored producer lock");
        xrCheck(CALL(xrEndFrame)(session, &frame)); // writer must also skip, without blocking
        flock(fd, LOCK_UN);
        close(fd);
        xrCheck(CALL(xrWaitSwapchainImage)(chain, &waitInfo));
        image.render(true);
        xrCheck(CALL(xrReleaseSwapchainImage)(chain, nullptr));
        // Destroy while the copy may still be queued.
        xrCheck(CALL(xrDestroySwapchain)(chain));
        rejectTransfer = true;
        const auto before = creates;
        xrCheck(CALL(xrCreateSwapchain)(session, &chainInfo, &createdChain));
        require(creates == before + 2 && lastUsage == chainInfo.usageFlags, "runtime usage fallback failed");
        xrCheck(CALL(xrDestroySwapchain)(chain));
        rejectTransfer = false;
        chainInfo.createFlags = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
        xrCheck(CALL(xrCreateSwapchain)(session, &chainInfo, &createdChain));
        require(lastUsage == chainInfo.usageFlags, "protected image requested transfer usage");
        // Session destruction also cleans up this child swapchain and IPC file.
        xrCheck(CALL(xrDestroySession)(session));
        require(!std::filesystem::exists(path), "session left stale IPC endpoint");
        xrCheck(CALL(xrDestroyInstance)(instance));
        require(releases == 2 && endFrames == 3, "runtime calls were not forwarded exactly once");
        require(f.validationErrors == 0, "Vulkan validation errors");
        std::filesystem::remove_all(directory);
        std::cout << "Linux OpenXR layer and frame transport tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        // On test failure the process exits; avoid deleting files still owned
        // by the loaded layer's static state.
        return 1;
    }
}
