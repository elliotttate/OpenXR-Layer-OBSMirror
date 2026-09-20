#include "vulkan_readback.h"

#include <limits>
#include <utility>

namespace Mirror {
    bool IsVulkanMirrorFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return true;
        default:
            return false;
        }
    }

    bool VulkanDispatch::initialize(PFN_vkGetInstanceProcAddr getProc,
                                    VkInstance instance,
                                    VkPhysicalDevice physicalDevice,
                                    VkDevice logicalDevice,
                                    uint32_t family,
                                    uint32_t queueIndex) {
        if (!getProc || !instance || !physicalDevice || !logicalDevice)
            return false;
        const auto getDeviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getProc(instance, "vkGetDeviceProcAddr"));
        const auto getMemory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            getProc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (!getDeviceProc || !getMemory)
            return false;
        device = logicalDevice;
        queueFamily = family;
#define LOAD_PROC(name)                                                                                                \
    name = reinterpret_cast<PFN_vk##name>(getDeviceProc(device, "vk" #name));                                          \
    if (!name)                                                                                                         \
        return false;
        MIRROR_VULKAN_FUNCTIONS(LOAD_PROC)
#undef LOAD_PROC
        getMemory(physicalDevice, &memoryProperties);
        GetDeviceQueue(device, family, queueIndex, &queue);
        return queue != VK_NULL_HANDLE;
    }

    VulkanReadback::VulkanReadback(std::shared_ptr<VulkanDispatch> dispatch) : _vk(std::move(dispatch)) {
    }

    VulkanReadback::~VulkanReadback() {
        // The layer also drains before destroying runtime swapchains. This
        // wait covers partial initialization and instance teardown paths.
        wait();
        if (_pool)
            _vk->DestroyCommandPool(_vk->device, _pool, nullptr);
        if (_fence)
            _vk->DestroyFence(_vk->device, _fence, nullptr);
        if (_mapped)
            _vk->UnmapMemory(_vk->device, _memory);
        if (_buffer)
            _vk->DestroyBuffer(_vk->device, _buffer, nullptr);
        if (_memory)
            _vk->FreeMemory(_vk->device, _memory, nullptr);
        if (_resolveImage)
            _vk->DestroyImage(_vk->device, _resolveImage, nullptr);
        if (_resolveMemory)
            _vk->FreeMemory(_vk->device, _resolveMemory, nullptr);
    }

    uint32_t VulkanReadback::findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < _vk->memoryProperties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (_vk->memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        return UINT32_MAX;
    }

#define VK_TRY(call)                                                                                                   \
    do {                                                                                                               \
        const VkResult result = (call);                                                                                \
        if (result != VK_SUCCESS)                                                                                      \
            return result;                                                                                             \
    } while (0)

    VkResult VulkanReadback::initialize(
        VkFormat format, uint32_t width, uint32_t height, uint32_t layers, VkSampleCountFlagBits samples) {
        // A readback object is initialized once. Owners discard it on failure.
        if (_buffer || !width || !height || !layers || !IsVulkanMirrorFormat(format) || !samples ||
            (samples & (samples - 1)) || samples > VK_SAMPLE_COUNT_64_BIT)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (width > UINT32_MAX / 4 || height > UINT32_MAX / (width * 4))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        const uint64_t sliceSize = uint64_t(width) * height * 4;
        if (layers > std::numeric_limits<size_t>::max() / sliceSize)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        _width = width;
        _height = height;
        _layers = layers;
        _size = sliceSize * layers;

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = _size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_TRY(_vk->CreateBuffer(_vk->device, &buffer, nullptr, &_buffer));
        VkMemoryRequirements requirements;
        _vk->GetBufferMemoryRequirements(_vk->device, _buffer, &requirements);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize = requirements.size;
        // Cached host memory makes the subsequent D3D upload much cheaper.
        memory.memoryTypeIndex = findMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (memory.memoryTypeIndex == UINT32_MAX)
            memory.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (memory.memoryTypeIndex == UINT32_MAX)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        VK_TRY(_vk->AllocateMemory(_vk->device, &memory, nullptr, &_memory));
        VK_TRY(_vk->BindBufferMemory(_vk->device, _buffer, _memory, 0));
        VK_TRY(_vk->MapMemory(_vk->device, _memory, 0, VK_WHOLE_SIZE, 0, &_mapped));

        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = format;
            image.extent = {width, height, 1};
            image.mipLevels = 1;
            image.arrayLayers = layers;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VK_TRY(_vk->CreateImage(_vk->device, &image, nullptr, &_resolveImage));
            _vk->GetImageMemoryRequirements(_vk->device, _resolveImage, &requirements);
            memory.allocationSize = requirements.size;
            memory.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (memory.memoryTypeIndex == UINT32_MAX)
                memory.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, 0);
            if (memory.memoryTypeIndex == UINT32_MAX)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            VK_TRY(_vk->AllocateMemory(_vk->device, &memory, nullptr, &_resolveMemory));
            VK_TRY(_vk->BindImageMemory(_vk->device, _resolveImage, _resolveMemory, 0));
        }

        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = _vk->queueFamily;
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VK_TRY(_vk->CreateCommandPool(_vk->device, &pool, nullptr, &_pool));
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = _pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        VK_TRY(_vk->AllocateCommandBuffers(_vk->device, &commands, &_commands));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_TRY(_vk->CreateFence(_vk->device, &fence, nullptr, &_fence));
        _initialized = true;
        return VK_SUCCESS;
    }

    VkResult VulkanReadback::submit(VkImage source) {
        if (!_initialized || !source)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (_pending)
            return VK_NOT_READY;
        VK_TRY(_vk->ResetCommandPool(_vk->device, _pool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_TRY(_vk->BeginCommandBuffer(_commands, &begin));

        VkImageMemoryBarrier sourceBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sourceBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        sourceBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        sourceBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.image = source;
        sourceBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, _layers};
        _vk->CmdPipelineBarrier(_commands,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0,
                                0,
                                nullptr,
                                0,
                                nullptr,
                                1,
                                &sourceBarrier);

        if (_resolveImage) {
            VkImageMemoryBarrier resolveBarrier = sourceBarrier;
            resolveBarrier.image = _resolveImage;
            resolveBarrier.srcAccessMask = 0;
            resolveBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            // The previous submission is complete and its contents disposable.
            resolveBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            resolveBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            _vk->CmdPipelineBarrier(_commands,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0,
                                    0,
                                    nullptr,
                                    0,
                                    nullptr,
                                    1,
                                    &resolveBarrier);
            VkImageResolve resolve{};
            resolve.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, _layers};
            resolve.dstSubresource = resolve.srcSubresource;
            resolve.extent = {_width, _height, 1};
            _vk->CmdResolveImage(_commands,
                                 source,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 _resolveImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1,
                                 &resolve);
            resolveBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            resolveBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            resolveBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            resolveBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            _vk->CmdPipelineBarrier(_commands,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0,
                                    0,
                                    nullptr,
                                    0,
                                    nullptr,
                                    1,
                                    &resolveBarrier);
        }

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, _layers};
        copy.imageExtent = {_width, _height, 1};
        _vk->CmdCopyImageToBuffer(
            _commands, _resolveImage ? _resolveImage : source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _buffer, 1, &copy);

        // Restore OpenXR's required layout, including the dependency on future
        // runtime reads/writes. Copy only mip zero; other mips remain untouched.
        sourceBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        sourceBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        _vk->CmdPipelineBarrier(_commands,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                0,
                                0,
                                nullptr,
                                0,
                                nullptr,
                                1,
                                &sourceBarrier);
        VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.buffer = _buffer;
        hostBarrier.size = VK_WHOLE_SIZE;
        _vk->CmdPipelineBarrier(_commands,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT,
                                0,
                                0,
                                nullptr,
                                1,
                                &hostBarrier,
                                0,
                                nullptr);
        VK_TRY(_vk->EndCommandBuffer(_commands));
        VK_TRY(_vk->ResetFences(_vk->device, 1, &_fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &_commands;
        VK_TRY(_vk->QueueSubmit(_vk->queue, 1, &submit, _fence));
        _pending = true;
        _ready = false;
        return VK_SUCCESS;
    }

    VkResult VulkanReadback::poll(const void*& data) {
        data = nullptr;
        if (!_pending)
            return VK_NOT_READY;
        VK_TRY(_vk->GetFenceStatus(_vk->device, _fence));
        if (!_ready) {
            // Invalidate the whole mapped allocation, which also satisfies
            // nonCoherentAtomSize alignment on non-coherent memory heaps.
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = _memory;
            range.size = VK_WHOLE_SIZE;
            VK_TRY(_vk->InvalidateMappedMemoryRanges(_vk->device, 1, &range));
            _ready = true;
        }
        data = _mapped;
        return VK_SUCCESS;
    }

    void VulkanReadback::markConsumed() {
        if (_ready) {
            _pending = false;
            _ready = false;
        }
    }

    VkResult VulkanReadback::wait() {
        return _pending ? _vk->WaitForFences(_vk->device, 1, &_fence, VK_TRUE, UINT64_MAX) : VK_SUCCESS;
    }
#undef VK_TRY
} // namespace Mirror
