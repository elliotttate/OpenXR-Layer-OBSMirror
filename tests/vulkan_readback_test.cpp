#include "vulkan_test_support.h"

static VKAPI_ATTR VkResult VKAPI_CALL failAllocation(VkDevice,
                                                     const VkMemoryAllocateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkDeviceMemory*) {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static VKAPI_ATTR VkResult VKAPI_CALL failSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VKAPI_ATTR VkResult VKAPI_CALL notReady(VkDevice, VkFence) {
    return VK_NOT_READY;
}

static void testTransfer(Fixture& f, VkFormat format, uint32_t mips, uint32_t layers, VkSampleCountFlagBits samples) {
    Source source(f, format, mips, layers, samples);
    Mirror::VulkanReadback capture(f.dispatch);
    check(capture.initialize(format, 17, 9, layers, samples));
    require(capture.rowPitch() == 68 && capture.slicePitch() == 612, "packed pitches");
    const void* pixels = reinterpret_cast<void*>(1);
    require(capture.poll(pixels) == VK_NOT_READY && pixels == nullptr, "no uninitialized pixels");
    for (unsigned frame = 0; frame < 3; ++frame) {
        const bool alternate = (frame % 2) != 0;
        source.render(alternate);
        check(capture.submit(source.image));
        require(capture.submit(source.image) == VK_NOT_READY, "must not overwrite pending readback");
        capture.markConsumed(); // Cannot discard work before it has completed.
        require(capture.submit(source.image) == VK_NOT_READY, "early consume must not reset pending work");
        check(capture.wait());
        check(capture.poll(pixels));
        for (uint32_t layer = 0; layer < layers; ++layer) {
            uint32_t channel = (layer + (alternate ? 1 : 0)) % 3;
            uint32_t expected;
            if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                expected = 0xc0000000u | (0x3ffu << (channel * 10));
            } else {
                if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
                    channel = 2 - channel;
                expected = 0xff000000u | (0xffu << (channel * 8));
            }
            for (uint32_t i = 0; i < 17 * 9; ++i) {
                uint32_t actual;
                std::memcpy(&actual, static_cast<const uint8_t*>(pixels) + layer * capture.slicePitch() + i * 4, 4);
                require(actual == expected, "pixel/channel/layer/mip mismatch");
            }
        }
        capture.markConsumed();
        require(capture.poll(pixels) == VK_NOT_READY && !pixels, "consumed frame must not be published twice");
    }
    // The destructor must finish queued work before destroying its resources.
    source.render(false);
    check(capture.submit(source.image));
}

static void testFailures(Fixture& f) {
    Mirror::VulkanDispatch dispatch;
    require(!dispatch.initialize(nullptr, f.instance, f.physical, f.device, 0, 0), "missing loader");
    Mirror::VulkanReadback invalid(f.dispatch);
    require(invalid.initialize(VK_FORMAT_D32_SFLOAT, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT) == VK_ERROR_FORMAT_NOT_SUPPORTED,
            "depth rejected");
    require(invalid.initialize(VK_FORMAT_R16G16B16A16_SFLOAT, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_FORMAT_NOT_SUPPORTED,
            "unsupported HDR rejected");
    require(invalid.initialize(VK_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, UINT32_MAX, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_OUT_OF_HOST_MEMORY,
            "dimension overflow rejected");
    require(invalid.initialize(VK_FORMAT_R8G8B8A8_UNORM, 0, 9, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_FORMAT_NOT_SUPPORTED,
            "zero extent rejected");
    require(invalid.submit(VK_NULL_HANDLE) == VK_ERROR_INITIALIZATION_FAILED, "uninitialized submit rejected");

    // Exercise destruction with an allocated buffer but no memory/command pool.
    auto fault = std::make_shared<Mirror::VulkanDispatch>(*f.dispatch);
    fault->AllocateMemory = failAllocation;
    Mirror::VulkanReadback partial(fault);
    require(partial.initialize(VK_FORMAT_R8G8B8A8_UNORM, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "allocation error propagated");

    Source source(f, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_SAMPLE_COUNT_1_BIT);
    auto recoverable = std::make_shared<Mirror::VulkanDispatch>(*f.dispatch);
    Mirror::VulkanReadback capture(recoverable);
    check(capture.initialize(VK_FORMAT_R8G8B8A8_UNORM, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT));
    source.render(false);
    recoverable->QueueSubmit = failSubmit;
    require(capture.submit(source.image) == VK_ERROR_OUT_OF_HOST_MEMORY, "submission error propagated");
    const void* pixels = reinterpret_cast<void*>(1);
    require(capture.poll(pixels) == VK_NOT_READY && !pixels, "failed submission must not publish pixels");
    check(capture.wait()); // No work submitted: do not wait on the unsignalled fence.
    recoverable->QueueSubmit = f.dispatch->QueueSubmit;
    check(capture.submit(source.image));
    recoverable->GetFenceStatus = notReady;
    require(capture.poll(pixels) == VK_NOT_READY && !pixels, "pending GPU work must not expose host memory");
    recoverable->GetFenceStatus = f.dispatch->GetFenceStatus;
    check(capture.wait());
    check(capture.poll(pixels));
    require(pixels != nullptr, "submission recovers after failure");
    capture.markConsumed();
}

int main() {
    try {
        Fixture f;
        testFailures(f);
        for (auto format : {VK_FORMAT_R8G8B8A8_UNORM,
                            VK_FORMAT_R8G8B8A8_SRGB,
                            VK_FORMAT_B8G8R8A8_UNORM,
                            VK_FORMAT_B8G8R8A8_SRGB,
                            VK_FORMAT_A2B10G10R10_UNORM_PACK32}) {
            testTransfer(f, format, 1, 1, VK_SAMPLE_COUNT_1_BIT);
            testTransfer(f, format, 3, 2, VK_SAMPLE_COUNT_1_BIT);
            VkImageFormatProperties properties{};
            const auto supported = vkGetPhysicalDeviceImageFormatProperties(
                f.physical,
                format,
                VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                0,
                &properties);
            if (supported == VK_SUCCESS && (properties.sampleCounts & VK_SAMPLE_COUNT_4_BIT))
                testTransfer(f, format, 1, 2, VK_SAMPLE_COUNT_4_BIT);
            else
                std::cout << "MSAA unsupported for format " << format << ", skipped\n";
        }
        require(f.validationErrors == 0, "Vulkan validation errors");
        std::cout << "Vulkan readback tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
