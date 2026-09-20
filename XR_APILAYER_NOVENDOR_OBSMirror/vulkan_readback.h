#pragma once

// This backend uses only Vulkan 1.0. Keep it independent of Windows/OpenXR so
// its image transfers can also be exercised with a software Vulkan driver.
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>

namespace Mirror {

    // Formats whose bytes can be uploaded directly to the D3D11 compositor.
    bool IsVulkanMirrorFormat(VkFormat format);

    struct VulkanDispatch {
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkPhysicalDeviceMemoryProperties memoryProperties{};

#define MIRROR_VULKAN_FUNCTIONS(F)                                                                                     \
    F(GetDeviceQueue)                                                                                                  \
    F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(AllocateMemory) F(FreeMemory)                    \
        F(BindBufferMemory) F(MapMemory) F(UnmapMemory) F(InvalidateMappedMemoryRanges) F(CreateImage) F(DestroyImage) \
            F(GetImageMemoryRequirements) F(BindImageMemory) F(CreateCommandPool) F(DestroyCommandPool)                \
                F(AllocateCommandBuffers) F(ResetCommandPool) F(BeginCommandBuffer) F(EndCommandBuffer)                \
                    F(CmdPipelineBarrier) F(CmdCopyImageToBuffer) F(CmdResolveImage) F(CreateFence) F(DestroyFence)    \
                        F(ResetFences) F(GetFenceStatus) F(WaitForFences) F(QueueSubmit)
#define DECLARE_PROC(name) PFN_vk##name name = nullptr;
        MIRROR_VULKAN_FUNCTIONS(DECLARE_PROC)
#undef DECLARE_PROC

        bool initialize(PFN_vkGetInstanceProcAddr getProc,
                        VkInstance instance,
                        VkPhysicalDevice physicalDevice,
                        VkDevice logicalDevice,
                        uint32_t family,
                        uint32_t queueIndex);
    };

    class VulkanReadback {
      public:
        explicit VulkanReadback(std::shared_ptr<VulkanDispatch> dispatch);
        ~VulkanReadback();
        VulkanReadback(const VulkanReadback&) = delete;
        VulkanReadback& operator=(const VulkanReadback&) = delete;

        VkResult
        initialize(VkFormat format, uint32_t width, uint32_t height, uint32_t layers, VkSampleCountFlagBits samples);

        // Called only before xrReleaseSwapchainImage, on the binding's queue.
        // VK_NOT_READY means the previous capture has not been consumed yet.
        VkResult submit(VkImage source);

        // Never waits for the GPU. Data stays valid until the next submit.
        // Consume it before markConsumed(), including any CPU upload/copy.
        VkResult poll(const void*& data);
        void markConsumed();
        VkResult wait();

        uint32_t rowPitch() const {
            return _width * 4;
        }
        uint32_t slicePitch() const {
            return rowPitch() * _height;
        }

      private:
        uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
        std::shared_ptr<VulkanDispatch> _vk;
        uint32_t _width = 0, _height = 0, _layers = 0;
        VkDeviceSize _size = 0;
        VkBuffer _buffer = VK_NULL_HANDLE;
        VkDeviceMemory _memory = VK_NULL_HANDLE;
        VkImage _resolveImage = VK_NULL_HANDLE;
        VkDeviceMemory _resolveMemory = VK_NULL_HANDLE;
        VkCommandPool _pool = VK_NULL_HANDLE;
        VkCommandBuffer _commands = VK_NULL_HANDLE;
        VkFence _fence = VK_NULL_HANDLE;
        void* _mapped = nullptr;
        bool _pending = false;
        bool _ready = false;
        bool _initialized = false;
    };
} // namespace Mirror
