#include "pch.h"
#include "vkmirror.h"
#include "dx11mirror.h"
#include "log.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstring>
#include <mutex>

namespace Mirror {
    using namespace layer_OBSMirror::log;

    namespace {
        // Copies a mirrored frame may have in flight before the next one is
        // skipped; matches a typical swapchain depth.
        constexpr uint32_t kCopySlots = 3;

        struct InstanceRecord {
            PFN_vkGetInstanceProcAddr gipa = nullptr;
            uint32_t apiVersion = VK_API_VERSION_1_0;
        };

        struct DeviceRecord {
            bool ready = false;
            std::string reason;
            PFN_vkGetInstanceProcAddr gipa = nullptr;
        };

        std::mutex g_lock;
        std::map<VkPhysicalDevice, std::pair<VkInstance, InstanceRecord>> g_physicalDevices;
        std::map<VkDevice, DeviceRecord> g_devices;

        // The Vulkan loader the application uses is already in the process;
        // its exported entry points dispatch through any of its handles.
        PFN_vkVoidFunction loaderExport(const char* name) {
            HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
            return loader ? reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(loader, name)) : nullptr;
        }

        bool hasName(const std::vector<const char*>& names, const char* name) {
            for (const char* n : names) {
                if (n && std::strcmp(n, name) == 0)
                    return true;
            }
            return false;
        }

        bool hasProperty(const std::vector<VkExtensionProperties>& available, const char* name) {
            for (const auto& e : available) {
                if (std::strcmp(e.extensionName, name) == 0)
                    return true;
            }
            return false;
        }
    } // namespace

    namespace VulkanSetup {
        void PatchInstanceCreateInfo(const XrVulkanInstanceCreateInfoKHR& xrInfo,
                                     VkInstanceCreateInfo& ci,
                                     std::vector<const char*>& names) {
            names.assign(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
            const uint32_t apiVersion =
                ci.pApplicationInfo && ci.pApplicationInfo->apiVersion ? ci.pApplicationInfo->apiVersion
                                                                        : VK_API_VERSION_1_0;
            // External memory queries are core from Vulkan 1.1.
            if (VK_API_VERSION_MAJOR(apiVersion) > 1 || VK_API_VERSION_MINOR(apiVersion) >= 1)
                return;

            auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                xrInfo.pfnGetInstanceProcAddr
                    ? xrInfo.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties")
                    : nullptr);
            if (!enumerate)
                return;
            uint32_t count = 0;
            std::vector<VkExtensionProperties> available;
            if (enumerate(nullptr, &count, nullptr) == VK_SUCCESS) {
                available.resize(count);
                if (enumerate(nullptr, &count, available.data()) != VK_SUCCESS)
                    available.clear();
                available.resize(std::min<size_t>(count, available.size()));
            }
            for (const char* wanted : {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME}) {
                if (!hasName(names, wanted) && hasProperty(available, wanted)) {
                    names.push_back(wanted);
                    Log("Vulkan: enabling instance extension %s for OBS mirroring\n", wanted);
                }
            }
            ci.enabledExtensionCount = static_cast<uint32_t>(names.size());
            ci.ppEnabledExtensionNames = names.data();
        }

        void OnInstanceCreated(const XrVulkanInstanceCreateInfoKHR& xrInfo,
                               const VkInstanceCreateInfo& ci,
                               VkInstance instance) {
            if (!instance || !xrInfo.pfnGetInstanceProcAddr)
                return;
            InstanceRecord record;
            record.gipa = xrInfo.pfnGetInstanceProcAddr;
            if (ci.pApplicationInfo && ci.pApplicationInfo->apiVersion)
                record.apiVersion = ci.pApplicationInfo->apiVersion;

            auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
                record.gipa(instance, "vkEnumeratePhysicalDevices"));
            if (!enumerate)
                return;
            uint32_t count = 0;
            if (enumerate(instance, &count, nullptr) != VK_SUCCESS || !count)
                return;
            std::vector<VkPhysicalDevice> devices(count);
            if (enumerate(instance, &count, devices.data()) != VK_SUCCESS)
                return;

            std::lock_guard lock(g_lock);
            for (uint32_t i = 0; i < count; ++i)
                g_physicalDevices[devices[i]] = {instance, record};
        }

        DevicePatch::DevicePatch(const XrVulkanDeviceCreateInfoKHR& xrInfo, VkDeviceCreateInfo& ci) {
            _gipa = xrInfo.pfnGetInstanceProcAddr;
            _names.assign(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);

            VkInstance instance = VK_NULL_HANDLE;
            uint32_t instanceVersion = VK_API_VERSION_1_0;
            PFN_vkGetInstanceProcAddr gipa = nullptr;
            {
                std::lock_guard lock(g_lock);
                auto it = g_physicalDevices.find(xrInfo.vulkanPhysicalDevice);
                if (it != g_physicalDevices.end()) {
                    instance = it->second.first;
                    instanceVersion = it->second.second.apiVersion;
                    gipa = it->second.second.gipa;
                }
            }

            PFN_vkEnumerateDeviceExtensionProperties enumerate = nullptr;
            PFN_vkGetPhysicalDeviceProperties getProperties = nullptr;
            if (gipa && instance) {
                enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    gipa(instance, "vkEnumerateDeviceExtensionProperties"));
                getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                    gipa(instance, "vkGetPhysicalDeviceProperties"));
            } else {
                // The instance was not created through xrCreateVulkanInstanceKHR
                // under this layer; use the loader's own exports.
                enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    loaderExport("vkEnumerateDeviceExtensionProperties"));
                getProperties =
                    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(loaderExport("vkGetPhysicalDeviceProperties"));
                instanceVersion = VK_API_VERSION_1_1;
            }
            if (!enumerate || !getProperties) {
                _reason = "could not query the Vulkan device's extensions";
                return;
            }

            VkPhysicalDeviceProperties properties{};
            getProperties(xrInfo.vulkanPhysicalDevice, &properties);
            const uint32_t deviceVersion = std::min(instanceVersion, properties.apiVersion);
            const bool core12 = VK_API_VERSION_MAJOR(deviceVersion) > 1 || VK_API_VERSION_MINOR(deviceVersion) >= 2;

            uint32_t count = 0;
            std::vector<VkExtensionProperties> available;
            if (enumerate(xrInfo.vulkanPhysicalDevice, nullptr, &count, nullptr) == VK_SUCCESS) {
                available.resize(count);
                if (enumerate(xrInfo.vulkanPhysicalDevice, nullptr, &count, available.data()) != VK_SUCCESS)
                    available.clear();
                available.resize(std::min<size_t>(count, available.size()));
            }

            for (const char* required :
                 {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME}) {
                if (!hasProperty(available, required)) {
                    _reason = std::string("the Vulkan driver does not offer ") + required;
                    return;
                }
            }
            if (!core12 && !hasProperty(available, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
                _reason = "the Vulkan device has no timeline semaphores (Vulkan 1.2 or VK_KHR_timeline_semaphore)";
                return;
            }

            // Promoted extensions are legal to enable on any version; listing
            // them covers Vulkan 1.0 devices without version bookkeeping.
            for (const char* wanted : {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
                                       VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                       VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                                       VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME}) {
                if (!hasName(_names, wanted) && hasProperty(available, wanted)) {
                    _names.push_back(wanted);
                    Log("Vulkan: enabling device extension %s for OBS mirroring\n", wanted);
                }
            }
            ci.enabledExtensionCount = static_cast<uint32_t>(_names.size());
            ci.ppEnabledExtensionNames = _names.data();

            // Timeline semaphores are a feature that must be switched on. Edit
            // an existing feature struct in place (restored after creation);
            // otherwise prepend one of our own.
            bool found = false;
            for (auto* node = reinterpret_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext) {
                VkBool32* flag = nullptr;
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                    flag = &const_cast<VkPhysicalDeviceVulkan12Features*>(
                                reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node))
                                ->timelineSemaphore;
                } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
                    flag = &const_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(
                                reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node))
                                ->timelineSemaphore;
                }
                if (flag) {
                    found = true;
                    if (!*flag) {
                        *flag = VK_TRUE;
                        _restoreFeature = flag;
                        Log("Vulkan: enabling the timelineSemaphore feature for OBS mirroring\n");
                    }
                    break;
                }
            }
            if (!found) {
                _timelineFeatures.pNext = const_cast<void*>(ci.pNext);
                _timelineFeatures.timelineSemaphore = VK_TRUE;
                ci.pNext = &_timelineFeatures;
                Log("Vulkan: enabling the timelineSemaphore feature for OBS mirroring\n");
            }
            _ready = true;
        }

        DevicePatch::~DevicePatch() {
            if (_restoreFeature)
                *_restoreFeature = VK_FALSE;
        }

        void DevicePatch::onDeviceCreated(VkDevice device, VkResult result) {
            if (result != VK_SUCCESS || !device)
                return;
            if (!_ready)
                Log("Vulkan device %p cannot be mirrored: %s\n", device, _reason.c_str());
            std::lock_guard lock(g_lock);
            g_devices[device] = DeviceRecord{_ready, _reason, _gipa};
        }

        std::string AppendLegacyDeviceExtensions(const std::string& list) {
            // Every Windows driver with Vulkan 1.2 offers these; the
            // timelineSemaphore feature itself is up to the application.
            std::string merged = list;
            std::vector<std::string> present;
            {
                std::istringstream stream(list);
                for (std::string name; stream >> name;)
                    present.push_back(name);
            }
            bool logged = false;
            for (const char* wanted : {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                                       VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
                                       VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                       VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                                       VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME}) {
                if (std::find(present.begin(), present.end(), wanted) != present.end())
                    continue;
                if (!merged.empty())
                    merged += ' ';
                merged += wanted;
                if (!logged) {
                    logged = true;
                    Log("Vulkan (XR_KHR_vulkan_enable): adding the device extensions OBS mirroring needs\n");
                }
            }
            return merged;
        }

        std::string DeviceProblem(VkDevice device) {
            std::lock_guard lock(g_lock);
            auto it = g_devices.find(device);
            if (it == g_devices.end()) {
                // Created without xrCreateVulkanDeviceKHR (XR_KHR_vulkan_enable,
                // or by the application itself). The runtime's own Vulkan
                // interop usually needs the same extensions, so the bridge
                // tries the device as it is and fails cleanly if they are absent.
                Log("Vulkan device %p was not created through xrCreateVulkanDeviceKHR; trying it with the extensions "
                    "it already has\n",
                    device);
                return std::string();
            }
            return it->second.ready ? std::string() : it->second.reason;
        }

        PFN_vkGetInstanceProcAddr DeviceGetInstanceProcAddr(VkDevice device) {
            std::lock_guard lock(g_lock);
            auto it = g_devices.find(device);
            if (it != g_devices.end() && it->second.gipa)
                return it->second.gipa;
            return reinterpret_cast<PFN_vkGetInstanceProcAddr>(loaderExport("vkGetInstanceProcAddr"));
        }

        void ForgetDevices() {
            std::lock_guard lock(g_lock);
            g_devices.clear();
            g_physicalDevices.clear();
        }
    } // namespace VulkanSetup

    DXGI_FORMAT DxgiFormatFromVulkan(int64_t vkFormat) {
        switch (static_cast<VkFormat>(vkFormat)) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R16G16B16A16_UNORM:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    VulkanBridge::VulkanBridge(const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr gipa)
        : _binding(binding) {
        _binding.next = nullptr;
        if (!gipa || !binding.instance || !binding.device) {
            Log("Vulkan bridge: missing Vulkan entry points or handles\n");
            return;
        }
        if (!loadFunctions(gipa))
            return;

        // The adapter LUID ties the private D3D12 device (and the mirror's
        // D3D11 device) to the GPU the game renders on.
        auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            gipa(binding.instance, "vkGetPhysicalDeviceProperties2"));
        if (!props2)
            props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                gipa(binding.instance, "vkGetPhysicalDeviceProperties2KHR"));
        auto memProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            gipa(binding.instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (!props2 || !memProps) {
            Log("Vulkan bridge: vkGetPhysicalDeviceProperties2 unavailable\n");
            return;
        }
        VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &idProperties;
        props2(binding.physicalDevice, &properties);
        memProps(binding.physicalDevice, &_memoryProperties);
        if (!idProperties.deviceLUIDValid) {
            Log("Vulkan bridge: the Vulkan device reports no adapter LUID; mirroring disabled\n");
            return;
        }
        std::memcpy(&_luid, idProperties.deviceLUID, sizeof(_luid));

        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(_luid, IID_PPV_ARGS(&adapter)))) {
            Log("Vulkan bridge: no DXGI adapter matches LUID %08x:%08x\n", _luid.HighPart, _luid.LowPart);
            return;
        }
        const HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_d3d12Device));
        if (FAILED(hr)) {
            Log("Vulkan bridge: D3D12CreateDevice failed (0x%08x)\n", hr);
            return;
        }

        vkGetDeviceQueue(binding.device, binding.queueFamilyIndex, binding.queueIndex, &_queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = binding.queueFamilyIndex;
        if (!_queue || vkCreateCommandPool(binding.device, &poolInfo, nullptr, &_commandPool) != VK_SUCCESS) {
            Log("Vulkan bridge: could not create a command pool on queue family %u\n", binding.queueFamilyIndex);
            return;
        }

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        Log("Vulkan bridge ready on '%ls' (LUID %08x:%08x), queue family %u index %u\n",
            desc.Description,
            _luid.HighPart,
            _luid.LowPart,
            binding.queueFamilyIndex,
            binding.queueIndex);
        _initialized = true;
    }

    VulkanBridge::~VulkanBridge() {
        for (auto& [swapchain, target] : _targets)
            destroyTarget(target);
        _targets.clear();
        if (_commandPool)
            vkDestroyCommandPool(_binding.device, _commandPool, nullptr);
    }

    bool VulkanBridge::loadFunctions(PFN_vkGetInstanceProcAddr gipa) {
        auto gdpa =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(_binding.instance, "vkGetDeviceProcAddr"));
        if (!gdpa) {
            Log("Vulkan bridge: vkGetDeviceProcAddr unavailable\n");
            return false;
        }
        bool ok = true;
#define VKMIRROR_LOAD(name)                                                                                            \
    name = reinterpret_cast<PFN_##name>(gdpa(_binding.device, #name));                                                \
    if (!name) {                                                                                                       \
        Log("Vulkan bridge: device function " #name " unavailable\n");                                                 \
        ok = false;                                                                                                    \
    }
        VKMIRROR_FUNCTIONS(VKMIRROR_LOAD)
#undef VKMIRROR_LOAD
        return ok;
    }

    bool VulkanBridge::findMemoryType(uint32_t bits, uint32_t& index) const {
        // Prefer device-local memory; any allowed type is acceptable.
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32_t i = 0; i < _memoryProperties.memoryTypeCount; ++i) {
                if (!(bits & (1u << i)))
                    continue;
                if (pass == 0 &&
                    !(_memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    continue;
                index = i;
                return true;
            }
        }
        return false;
    }

    bool VulkanBridge::createTarget(XrSwapchain swapchain,
                                    const XrSwapchainCreateInfo& createInfo,
                                    VkFormat vkFormat,
                                    DXGI_FORMAT dxgiFormat,
                                    D3D11Mirror& mirror) {
        if (!_initialized)
            return false;
        removeTarget(swapchain);

        const VkDevice device = _binding.device;
        Target target;
        target.width = createInfo.width;
        target.height = createInfo.height;
        target.layers = std::max(createInfo.arraySize, 1u);
        target.format = vkFormat;

        auto fail = [&](const char* what, long code) {
            Log("Vulkan bridge: %s failed (%ld) for swapchain %p; not mirrored\n", what, code, swapchain);
            destroyTarget(target);
            return false;
        };

        // Shared texture, created exactly like the D3D12 session path.
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = target.width;
        desc.Height = target.height;
        desc.DepthOrArraySize = static_cast<UINT16>(target.layers);
        desc.MipLevels = 1;
        desc.Format = dxgiFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = _d3d12Device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target.texture));
        if (FAILED(hr))
            return fail("CreateCommittedResource", hr);
        hr = _d3d12Device->CreateSharedHandle(target.texture.Get(), nullptr, GENERIC_ALL, nullptr, &target.textureHandle);
        if (FAILED(hr))
            return fail("CreateSharedHandle(texture)", hr);
        hr = _d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&target.fence));
        if (FAILED(hr))
            return fail("CreateFence", hr);
        hr = _d3d12Device->CreateSharedHandle(target.fence.Get(), nullptr, GENERIC_ALL, nullptr, &target.fenceHandle);
        if (FAILED(hr))
            return fail("CreateSharedHandle(fence)", hr);

        // Import the texture into Vulkan.
        VkExternalMemoryImageCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.pNext = &externalInfo;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = vkFormat;
        imageInfo.extent = {target.width, target.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = target.layers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult vr = vkCreateImage(device, &imageInfo, nullptr, &target.image);
        if (vr != VK_SUCCESS)
            return fail("vkCreateImage", vr);

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, target.image, &requirements);
        VkMemoryWin32HandlePropertiesKHR handleProperties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        vr = vkGetMemoryWin32HandlePropertiesKHR(
            device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, target.textureHandle, &handleProperties);
        if (vr != VK_SUCCESS)
            return fail("vkGetMemoryWin32HandlePropertiesKHR", vr);
        uint32_t memoryType = 0;
        if (!findMemoryType(requirements.memoryTypeBits & handleProperties.memoryTypeBits, memoryType))
            return fail("finding a memory type for the shared texture", 0);

        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = target.image;
        VkImportMemoryWin32HandleInfoKHR memoryImport{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        memoryImport.pNext = &dedicated;
        memoryImport.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        memoryImport.handle = target.textureHandle;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &memoryImport;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        vr = vkAllocateMemory(device, &allocation, nullptr, &target.memory);
        if (vr != VK_SUCCESS)
            return fail("vkAllocateMemory(import)", vr);
        vr = vkBindImageMemory(device, target.image, target.memory, 0);
        if (vr != VK_SUCCESS)
            return fail("vkBindImageMemory", vr);

        // Import the fence as a timeline semaphore.
        VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphoreInfo.pNext = &typeInfo;
        vr = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &target.semaphore);
        if (vr != VK_SUCCESS)
            return fail("vkCreateSemaphore", vr);
        VkImportSemaphoreWin32HandleInfoKHR semaphoreImport{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        semaphoreImport.semaphore = target.semaphore;
        semaphoreImport.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        semaphoreImport.handle = target.fenceHandle;
        vr = vkImportSemaphoreWin32HandleKHR(device, &semaphoreImport);
        if (vr != VK_SUCCESS)
            return fail("vkImportSemaphoreWin32HandleKHR", vr);

        target.slots.resize(kCopySlots);
        for (auto& slot : target.slots) {
            VkCommandBufferAllocateInfo bufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            bufferInfo.commandPool = _commandPool;
            bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            bufferInfo.commandBufferCount = 1;
            vr = vkAllocateCommandBuffers(device, &bufferInfo, &slot.commandBuffer);
            if (vr != VK_SUCCESS)
                return fail("vkAllocateCommandBuffers", vr);
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vr = vkCreateFence(device, &fenceInfo, nullptr, &slot.fence);
            if (vr != VK_SUCCESS)
                return fail("vkCreateFence", vr);
        }

        mirror.createSharedMirrorTexture(swapchain, target.textureHandle, target.fenceHandle);
        Log("Vulkan bridge: shared copy target %ux%u x%u (VkFormat %d -> DXGI %d) for swapchain %p\n",
            target.width,
            target.height,
            target.layers,
            vkFormat,
            dxgiFormat,
            swapchain);
        _targets.emplace(swapchain, std::move(target));
        return true;
    }

    bool VulkanBridge::copy(XrSwapchain swapchain, VkImage image, D3D11Mirror& mirror, const char** reason) {
        auto it = _targets.find(swapchain);
        if (!_initialized || it == _targets.end() || !image) {
            *reason = "no shared copy target";
            return false;
        }
        Target& target = it->second;
        const VkDevice device = _binding.device;
        Slot& slot = target.slots[target.nextSlot];
        if (slot.submitted) {
            if (vkGetFenceStatus(device, slot.fence) != VK_SUCCESS) {
                *reason = "previous Vulkan copy still in flight";
                return false;
            }
            vkResetFences(device, 1, &slot.fence);
            slot.submitted = false;
        }

        vkResetCommandBuffer(slot.commandBuffer, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(slot.commandBuffer, &begin) != VK_SUCCESS) {
            *reason = "vkBeginCommandBuffer failed";
            return false;
        }

        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, target.layers};

        // A released color image is in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable).
        VkImageMemoryBarrier before[2]{};
        before[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        before[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        before[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        before[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[0].image = image;
        before[0].subresourceRange = range;
        // The whole target is overwritten, so its old contents are discarded.
        before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        before[1].srcAccessMask = 0;
        before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[1].image = target.image;
        before[1].subresourceRange = range;
        vkCmdPipelineBarrier(slot.commandBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             2,
                             before);

        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, target.layers};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, target.layers};
        region.extent = {target.width, target.height, 1};
        vkCmdCopyImage(slot.commandBuffer,
                       image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1,
                       &region);

        VkImageMemoryBarrier after[2]{};
        after[0] = before[0];
        after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        after[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        after[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        after[1] = before[1];
        after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(slot.commandBuffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             2,
                             after);
        if (vkEndCommandBuffer(slot.commandBuffer) != VK_SUCCESS) {
            *reason = "vkEndCommandBuffer failed";
            return false;
        }

        const uint64_t value = target.value + 1;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &timeline;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.commandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &target.semaphore;
        const VkResult vr = vkQueueSubmit(_queue, 1, &submit, slot.fence);
        if (vr != VK_SUCCESS) {
            *reason = "vkQueueSubmit failed";
            return false;
        }
        slot.submitted = true;
        target.value = value;
        target.nextSlot = (target.nextSlot + 1) % static_cast<uint32_t>(target.slots.size());
        // The mirror device GPU-waits for this value before sampling.
        mirror.notifyFenceValue(swapchain, value);
        return true;
    }

    void VulkanBridge::removeTarget(XrSwapchain swapchain) {
        auto it = _targets.find(swapchain);
        if (it == _targets.end())
            return;
        destroyTarget(it->second);
        _targets.erase(it);
    }

    void VulkanBridge::destroyTarget(Target& target) {
        const VkDevice device = _binding.device;
        for (auto& slot : target.slots) {
            // Our copies must finish before the images they touch go away.
            if (slot.submitted && slot.fence)
                vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 1000000000ull);
            if (slot.fence)
                vkDestroyFence(device, slot.fence, nullptr);
            if (slot.commandBuffer)
                vkFreeCommandBuffers(device, _commandPool, 1, &slot.commandBuffer);
        }
        target.slots.clear();
        if (target.semaphore)
            vkDestroySemaphore(device, target.semaphore, nullptr);
        if (target.image)
            vkDestroyImage(device, target.image, nullptr);
        if (target.memory)
            vkFreeMemory(device, target.memory, nullptr);
        target.semaphore = VK_NULL_HANDLE;
        target.image = VK_NULL_HANDLE;
        target.memory = VK_NULL_HANDLE;
        if (target.textureHandle)
            CloseHandle(target.textureHandle);
        if (target.fenceHandle)
            CloseHandle(target.fenceHandle);
        target.textureHandle = nullptr;
        target.fenceHandle = nullptr;
        target.texture.Reset();
        target.fence.Reset();
    }

} // namespace Mirror
