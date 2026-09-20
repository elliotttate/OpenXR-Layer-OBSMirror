#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include "vulkan_readback.h"
#include "frame_ipc.h"
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace {
    constexpr const char* layerName = "XR_APILAYER_NOVENDOR_OBSMirror";
    std::recursive_mutex mutex;
    struct Instance {
        XrInstance handle;
        PFN_xrGetInstanceProcAddr getProc;
        template <class T>
        T function(const char* name) const {
            PFN_xrVoidFunction result = nullptr;
            getProc(handle, name, &result);
            return reinterpret_cast<T>(result);
        }
    };
    struct Session {
        Instance* instance;
        std::shared_ptr<Mirror::VulkanDispatch> vulkan;
        linux_mirror::FrameWriter writer;
        linux_mirror::Frame frame;
    };
    struct Swapchain {
        Session* session;
        XrSwapchainCreateInfo info;
        bool capture = false, waited = false;
        std::vector<VkImage> images;
        std::deque<uint32_t> acquired;
        std::unique_ptr<Mirror::VulkanReadback> readback;
        std::vector<uint8_t> pixels;
        void collect() {
            if (!readback || !capture)
                return;
            const void* data = nullptr;
            const auto result = readback->poll(data);
            if (result == VK_SUCCESS) {
                const auto* bytes = static_cast<const uint8_t*>(data);
                pixels.assign(bytes, bytes + size_t(readback->slicePitch()) * info.arraySize);
                readback->markConsumed();
            } else if (result < 0) {
                std::fprintf(stderr, "[OBSMirror] Vulkan readback failed: %d\n", result);
                capture = false;
            }
        }
    };
    std::map<XrInstance, std::unique_ptr<Instance>> instances;
    std::map<XrSession, std::unique_ptr<Session>> sessions;
    std::map<XrSwapchain, std::unique_ptr<Swapchain>> swapchains;

#define LOCK std::unique_lock<std::recursive_mutex> guard(mutex)
#define LOOKUP(container, handle)                                                                                      \
    auto found = container.find(handle);                                                                               \
    if (found == container.end())                                                                                      \
        return XR_ERROR_HANDLE_INVALID;                                                                                \
    auto& state = *found->second
#define NEXT(name) state.instance->function<PFN_##name>(#name)
#define SWAP_NEXT(name) state.session->instance->function<PFN_##name>(#name)

    XrResult XRAPI_CALL createSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
        LOCK;
        LOOKUP(instances, instance);
        const auto result = state.function<PFN_xrCreateSession>("xrCreateSession")(instance, info, session);
        if (XR_FAILED(result))
            return result;
        auto created = std::make_unique<Session>();
        created->instance = &state;
        for (auto entry = reinterpret_cast<const XrBaseInStructure*>(info->next); entry; entry = entry->next) {
            if (entry->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
                continue;
            const auto* binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(entry);
            auto dispatch = std::make_shared<Mirror::VulkanDispatch>();
            if (dispatch->initialize(vkGetInstanceProcAddr,
                                     binding->instance,
                                     binding->physicalDevice,
                                     binding->device,
                                     binding->queueFamilyIndex,
                                     binding->queueIndex)) {
                created->vulkan = std::move(dispatch);
                std::fprintf(stderr, "[OBSMirror] Linux Vulkan capture active (0.4.0-beta.2)\n");
            }
        }
        sessions.emplace(*session, std::move(created));
        return result;
    }
    XrResult XRAPI_CALL destroySession(XrSession session) {
        LOCK;
        LOOKUP(sessions, session);
        for (auto& item : swapchains)
            if (item.second->session == &state && item.second->readback)
                item.second->readback->wait();
        const auto result = NEXT(xrDestroySession)(session);
        if (XR_SUCCEEDED(result)) {
            for (auto it = swapchains.begin(); it != swapchains.end();)
                if (it->second->session == &state)
                    it = swapchains.erase(it);
                else
                    ++it;
            sessions.erase(session);
        }
        return result;
    }
    XrResult XRAPI_CALL destroyInstance(XrInstance instance) {
        LOCK;
        LOOKUP(instances, instance);
        // Drain before the runtime destroys its images, including implicit
        // destruction of child sessions. Never destroy the application's device.
        for (auto& item : swapchains)
            if (item.second->session->instance == &state && item.second->readback)
                item.second->readback->wait();
        const auto result = state.function<PFN_xrDestroyInstance>("xrDestroyInstance")(instance);
        if (XR_SUCCEEDED(result)) {
            for (auto it = swapchains.begin(); it != swapchains.end();)
                if (it->second->session->instance == &state)
                    it = swapchains.erase(it);
                else
                    ++it;
            for (auto it = sessions.begin(); it != sessions.end();)
                if (it->second->instance == &state)
                    it = sessions.erase(it);
                else
                    ++it;
            instances.erase(instance);
        }
        return result;
    }
    XrResult XRAPI_CALL createSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* chain) {
        LOCK;
        LOOKUP(sessions, session);
        if (!info || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO)
            return XR_ERROR_VALIDATION_FAILURE;
        auto request = *info;
        const bool capture = state.vulkan && info->faceCount == 1 &&
                             (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
                             !(info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) &&
                             Mirror::IsVulkanMirrorFormat(static_cast<VkFormat>(info->format));
        if (capture)
            request.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        auto result = NEXT(xrCreateSwapchain)(session, &request, chain);
        if (XR_FAILED(result) && request.usageFlags != info->usageFlags) {
            request = *info;
            result = NEXT(xrCreateSwapchain)(session, &request, chain);
        }
        if (XR_SUCCEEDED(result)) {
            auto created = std::make_unique<Swapchain>();
            created->session = &state;
            created->info = request;
            created->info.next = nullptr;
            created->capture = capture && (request.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT);
            swapchains.emplace(*chain, std::move(created));
        }
        return result;
    }
    XrResult XRAPI_CALL destroySwapchain(XrSwapchain chain) {
        LOCK;
        LOOKUP(swapchains, chain);
        if (state.readback)
            state.readback->wait();
        const auto result = SWAP_NEXT(xrDestroySwapchain)(chain);
        if (XR_SUCCEEDED(result))
            swapchains.erase(chain);
        return result;
    }
    XrResult XRAPI_CALL enumerateImages(XrSwapchain chain,
                                        uint32_t capacity,
                                        uint32_t* count,
                                        XrSwapchainImageBaseHeader* images) {
        LOCK;
        LOOKUP(swapchains, chain);
        const auto result = SWAP_NEXT(xrEnumerateSwapchainImages)(chain, capacity, count, images);
        if (XR_SUCCEEDED(result) && capacity && count && capacity >= *count && images && state.capture) {
            try {
                state.images.resize(*count);
                for (uint32_t i = 0; i < *count; ++i)
                    state.images[i] = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images)[i].image;
                if (!state.readback) {
                    auto readback = std::make_unique<Mirror::VulkanReadback>(state.session->vulkan);
                    const auto& info = state.info;
                    const uint64_t bytes = uint64_t(info.width) * info.height * info.arraySize * 4;
                    const auto init = bytes <= linux_mirror::kMaxFrameBytes
                                          ? readback->initialize(static_cast<VkFormat>(info.format),
                                                                 info.width,
                                                                 info.height,
                                                                 info.arraySize,
                                                                 static_cast<VkSampleCountFlagBits>(info.sampleCount))
                                          : VK_ERROR_OUT_OF_HOST_MEMORY;
                    if (init == VK_SUCCESS)
                        state.readback = std::move(readback);
                    else {
                        std::fprintf(stderr, "[OBSMirror] Cannot capture swapchain: %d\n", init);
                        state.capture = false;
                    }
                }
            } catch (...) {
                state.capture = false;
            }
        }
        return result;
    }
    XrResult XRAPI_CALL acquireImage(XrSwapchain chain, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
        LOCK;
        LOOKUP(swapchains, chain);
        const auto result = SWAP_NEXT(xrAcquireSwapchainImage)(chain, info, index);
        if (XR_SUCCEEDED(result))
            state.acquired.push_back(*index);
        return result;
    }
    XrResult XRAPI_CALL waitImage(XrSwapchain chain, const XrSwapchainImageWaitInfo* info) {
        LOCK;
        LOOKUP(swapchains, chain);
        const auto next = SWAP_NEXT(xrWaitSwapchainImage);
        // Waiting must not prevent another application thread from ending the
        // previous frame. OpenXR externally synchronizes this swapchain's life.
        guard.unlock();
        const auto result = next(chain, info);
        guard.lock();
        if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING)
            state.waited = true;
        return result;
    }
    XrResult XRAPI_CALL releaseImage(XrSwapchain chain, const XrSwapchainImageReleaseInfo* info) {
        LOCK;
        LOOKUP(swapchains, chain);
        if ((!info || info->type == XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) && state.capture && state.waited &&
            state.readback && !state.acquired.empty()) {
            try {
                state.collect();
                if (state.capture && state.acquired.front() < state.images.size()) {
                    const auto copy = state.readback->submit(state.images[state.acquired.front()]);
                    if (copy < 0) {
                        std::fprintf(stderr, "[OBSMirror] Vulkan copy failed: %d\n", copy);
                        state.capture = false;
                    }
                }
            } catch (...) {
                state.capture = false;
            }
        }
        const auto result = SWAP_NEXT(xrReleaseSwapchainImage)(chain, info);
        if (XR_SUCCEEDED(result)) {
            if (!state.acquired.empty())
                state.acquired.pop_front();
            state.waited = false;
        }
        return result;
    }

    bool validView(const XrSwapchainSubImage& view, const Swapchain& state) {
        const auto& rect = view.imageRect;
        return !state.pixels.empty() && rect.offset.x >= 0 && rect.offset.y >= 0 && rect.extent.width > 0 &&
               rect.extent.height > 0 && view.imageArrayIndex < state.info.arraySize &&
               uint64_t(rect.offset.x) + rect.extent.width <= state.info.width &&
               uint64_t(rect.offset.y) + rect.extent.height <= state.info.height;
    }
    void copyEye(const XrSwapchainSubImage& view, const Swapchain& state, linux_mirror::Frame& frame, uint32_t x) {
        const auto format = static_cast<VkFormat>(state.info.format);
        for (int32_t row = 0; row < view.imageRect.extent.height; ++row) {
            const auto* source =
                state.pixels.data() +
                ((size_t(view.imageArrayIndex) * state.info.height + view.imageRect.offset.y + row) * state.info.width +
                 view.imageRect.offset.x) *
                    4;
            auto* target = frame.pixels.data() + size_t(row) * frame.header.stride + x * 4;
            const uint32_t width = view.imageRect.extent.width;
            if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
                for (uint32_t col = 0; col < width; ++col) {
                    target[col * 4] = source[col * 4 + 2];
                    target[col * 4 + 1] = source[col * 4 + 1];
                    target[col * 4 + 2] = source[col * 4];
                    target[col * 4 + 3] = source[col * 4 + 3];
                }
            } else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                for (uint32_t col = 0; col < width; ++col) {
                    uint32_t pixel;
                    std::memcpy(&pixel, source + col * 4, 4);
                    target[col * 4] = (pixel & 1023) * 255 / 1023;
                    target[col * 4 + 1] = ((pixel >> 10) & 1023) * 255 / 1023;
                    target[col * 4 + 2] = ((pixel >> 20) & 1023) * 255 / 1023;
                    target[col * 4 + 3] = (pixel >> 30) * 85;
                }
            } else
                std::memcpy(target, source, size_t(width) * 4);
        }
    }
    XrResult XRAPI_CALL endFrame(XrSession session, const XrFrameEndInfo* info) {
        LOCK;
        LOOKUP(sessions, session);
        if (!info || info->type != XR_TYPE_FRAME_END_INFO)
            return XR_ERROR_VALIDATION_FAILURE;
        if (state.vulkan) {
            try {
                for (auto& item : swapchains)
                    if (item.second->session == &state)
                        item.second->collect();
                for (uint32_t i = 0; i < info->layerCount; ++i) {
                    const auto* base = info->layers[i];
                    if (!base || base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                        continue;
                    const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
                    if (!projection->viewCount)
                        continue;
                    const auto& left = projection->views[0].subImage;
                    const auto a = swapchains.find(left.swapchain);
                    if (a == swapchains.end() || a->second->session != &state || !validView(left, *a->second))
                        continue;
                    const auto& right = projection->views[projection->viewCount > 1 ? 1 : 0].subImage;
                    const auto b = swapchains.find(right.swapchain);
                    if (b == swapchains.end() || b->second->session != &state || !validView(right, *b->second))
                        continue;
                    auto& frame = state.frame;
                    frame.header.leftWidth = left.imageRect.extent.width;
                    frame.header.rightWidth = projection->viewCount > 1 ? right.imageRect.extent.width : 0;
                    frame.header.width = frame.header.leftWidth + frame.header.rightWidth;
                    frame.header.height = std::max(left.imageRect.extent.height,
                                                   projection->viewCount > 1 ? right.imageRect.extent.height : 0);
                    frame.header.stride = frame.header.width * 4;
                    const uint64_t size = uint64_t(frame.header.stride) * frame.header.height;
                    if (size > linux_mirror::kMaxFrameBytes)
                        continue;
                    frame.pixels.assign(size, 0);
                    copyEye(left, *a->second, frame, 0);
                    if (frame.header.rightWidth)
                        copyEye(right, *b->second, frame, frame.header.leftWidth);
                    state.writer.publish(frame);
                    break;
                }
            } catch (...) { /* Capture failure must not change headset submission. */
            }
        }
        const auto next = NEXT(xrEndFrame);
        guard.unlock();
        return next(session, info);
    }

    XrResult XRAPI_CALL getProc(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
        LOCK;
        if (!name || !function)
            return XR_ERROR_VALIDATION_FAILURE;
        *function = nullptr;
        if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(getProc);
            return XR_SUCCESS;
        }
        LOOKUP(instances, instance);
        const auto result = state.getProc(instance, name, function);
        if (XR_FAILED(result))
            return result;
#define OVERRIDE(api, implementation)                                                                                  \
    if (std::strcmp(name, #api) == 0)                                                                                  \
    *function = reinterpret_cast<PFN_xrVoidFunction>(implementation)
        OVERRIDE(xrDestroyInstance, destroyInstance);
        OVERRIDE(xrCreateSession, createSession);
        OVERRIDE(xrDestroySession, destroySession);
        OVERRIDE(xrCreateSwapchain, createSwapchain);
        OVERRIDE(xrDestroySwapchain, destroySwapchain);
        OVERRIDE(xrEnumerateSwapchainImages, enumerateImages);
        OVERRIDE(xrAcquireSwapchainImage, acquireImage);
        OVERRIDE(xrWaitSwapchainImage, waitImage);
        OVERRIDE(xrReleaseSwapchainImage, releaseImage);
        OVERRIDE(xrEndFrame, endFrame);
#undef OVERRIDE
        return result;
    }
    XrResult XRAPI_CALL createLayer(const XrInstanceCreateInfo* info,
                                    const XrApiLayerCreateInfo* layer,
                                    XrInstance* instance) {
        LOCK;
        if (!info || !layer || !instance || !layer->nextInfo ||
            layer->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
            layer->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
            layer->structSize != sizeof(XrApiLayerCreateInfo) ||
            std::strcmp(layer->nextInfo->layerName, layerName) != 0 || !layer->nextInfo->nextCreateApiLayerInstance ||
            !layer->nextInfo->nextGetInstanceProcAddr)
            return XR_ERROR_INITIALIZATION_FAILED;
        auto next = *layer;
        next.nextInfo = layer->nextInfo->next;
        const auto result = layer->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
        if (XR_SUCCEEDED(result)) {
            auto created = std::make_unique<Instance>();
            created->handle = *instance;
            created->getProc = layer->nextInfo->nextGetInstanceProcAddr;
            instances.emplace(*instance, std::move(created));
        }
        return result;
    }
} // namespace

extern "C" __attribute__((visibility("default"))) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char* name, XrNegotiateApiLayerRequest* request) {
    if (!loader || !request || !name || std::strcmp(name, layerName) != 0 ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(*loader) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || request->structSize != sizeof(*request) ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) || loader->minApiVersion > XR_CURRENT_API_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = std::min<XrVersion>(loader->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = getProc;
    request->createApiLayerInstance = createLayer;
    return XR_SUCCESS;
}
