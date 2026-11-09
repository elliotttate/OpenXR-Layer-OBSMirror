#pragma once
#include "pch.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// Vulkan sessions are mirrored by copying each released swapchain image, on
// the game's own queue, into a texture that a private D3D12 device created
// and shared by NT handle. The copy signals a shared D3D12 fence (imported
// into Vulkan as a timeline semaphore), so the existing D3D11 mirror device
// consumes it exactly like a D3D12 game's copy.
namespace Mirror {

    /// Records what the layer did to each Vulkan instance and device created
    /// through XR_KHR_vulkan_enable2, so a session can tell up front whether
    /// the device has what the copy path needs.
    namespace VulkanSetup {
        /// Adds the instance extensions external memory needs on Vulkan 1.0.
        /// Fills `storage`/`names` with the extension list to use.
        void PatchInstanceCreateInfo(const XrVulkanInstanceCreateInfoKHR& xrInfo,
                                     VkInstanceCreateInfo& ci,
                                     std::vector<const char*>& names);
        void OnInstanceCreated(const XrVulkanInstanceCreateInfoKHR& xrInfo,
                               const VkInstanceCreateInfo& ci,
                               VkInstance instance);

        /// Adds the device extensions (and the timeline semaphore feature) the
        /// copy path needs. The returned object must stay alive until the
        /// device has been created; destroying it undoes in-place feature edits.
        class DevicePatch {
          public:
            DevicePatch(const XrVulkanDeviceCreateInfoKHR& xrInfo, VkDeviceCreateInfo& ci);
            ~DevicePatch();
            DevicePatch(const DevicePatch&) = delete;
            DevicePatch& operator=(const DevicePatch&) = delete;

            /// Registers the device as mirror-capable when creation succeeded.
            void onDeviceCreated(VkDevice device, VkResult result);

          private:
            std::vector<const char*> _names;
            VkPhysicalDeviceTimelineSemaphoreFeatures _timelineFeatures{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            VkBool32* _restoreFeature = nullptr;
            bool _ready = false;
            std::string _reason;
            PFN_vkGetInstanceProcAddr _gipa = nullptr;
        };

        /// XR_KHR_vulkan_enable: the runtime's space-separated device
        /// extension list plus the ones the copy path needs.
        std::string AppendLegacyDeviceExtensions(const std::string& list);

        /// Empty when the device can be mirrored; otherwise why not.
        std::string DeviceProblem(VkDevice device);
        PFN_vkGetInstanceProcAddr DeviceGetInstanceProcAddr(VkDevice device);
        void ForgetDevices();
    } // namespace VulkanSetup

    /// DXGI format with the same memory layout as a Vulkan format, or
    /// DXGI_FORMAT_UNKNOWN when the mirror has no equivalent.
    DXGI_FORMAT DxgiFormatFromVulkan(int64_t vkFormat);

    class D3D11Mirror;

    /// Per-session Vulkan copy machinery.
    class VulkanBridge {
      public:
        VulkanBridge(const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr gipa);
        ~VulkanBridge();
        VulkanBridge(const VulkanBridge&) = delete;
        VulkanBridge& operator=(const VulkanBridge&) = delete;

        bool initialized() const {
            return _initialized;
        }

        VkDevice device() const {
            return _binding.device;
        }

        /// LUID of the adapter the game renders on (zero when unknown).
        LUID adapterLuid() const {
            return _luid;
        }

        /// (Re)creates the shared copy target for a swapchain and registers it
        /// with the mirror. Returns false when the swapchain cannot be mirrored.
        bool createTarget(XrSwapchain swapchain,
                          const XrSwapchainCreateInfo& createInfo,
                          VkFormat vkFormat,
                          DXGI_FORMAT dxgiFormat,
                          D3D11Mirror& mirror);

        bool hasTarget(XrSwapchain swapchain) const {
            return _targets.find(swapchain) != _targets.end();
        }

        /// Records and submits the copy of `image` (mip 0, every layer) into
        /// the swapchain's shared texture on the game's queue. Must be called
        /// while the application holds its queue (inside xrReleaseSwapchainImage).
        /// Returns false when the frame was skipped.
        bool copy(XrSwapchain swapchain, VkImage image, D3D11Mirror& mirror, const char** reason);

        void removeTarget(XrSwapchain swapchain);

      private:
        struct Slot {
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            bool submitted = false;
        };

        struct Target {
            ComPtr<ID3D12Resource> texture;
            ComPtr<ID3D12Fence> fence;
            HANDLE textureHandle = nullptr;
            HANDLE fenceHandle = nullptr;
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkSemaphore semaphore = VK_NULL_HANDLE;
            uint64_t value = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t layers = 1;
            VkFormat format = VK_FORMAT_UNDEFINED;
            std::vector<Slot> slots;
            uint32_t nextSlot = 0;
        };

        bool loadFunctions(PFN_vkGetInstanceProcAddr gipa);
        void destroyTarget(Target& target);
        bool findMemoryType(uint32_t bits, uint32_t& index) const;

        XrGraphicsBindingVulkanKHR _binding{};
        VkQueue _queue = VK_NULL_HANDLE;
        VkCommandPool _commandPool = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties _memoryProperties{};
        LUID _luid{};
        ComPtr<ID3D12Device> _d3d12Device;
        std::map<XrSwapchain, Target> _targets;
        bool _initialized = false;

#define VKMIRROR_FUNCTIONS(X)                                                                                          \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkFreeCommandBuffers)                                                                                            \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkResetCommandBuffer)                                                                                            \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyImage)                                                                                                  \
    X(vkQueueSubmit)                                                                                                   \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkResetFences)                                                                                                   \
    X(vkGetFenceStatus)                                                                                                \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindImageMemory)                                                                                               \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkGetMemoryWin32HandlePropertiesKHR)                                                                             \
    X(vkImportSemaphoreWin32HandleKHR)

#define VKMIRROR_DECLARE(name) PFN_##name name = nullptr;
        VKMIRROR_FUNCTIONS(VKMIRROR_DECLARE)
#undef VKMIRROR_DECLARE
    };

} // namespace Mirror
