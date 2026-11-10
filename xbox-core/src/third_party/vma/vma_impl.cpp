// xbox-core: VulkanMemoryAllocator's implementation, with Vulkan's functions from volk (as xemu
// builds it: vmaImportVulkanFunctionsFromVolk).
#define VK_NO_PROTOTYPES
#include <volk.h>
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "vk_mem_alloc.h"
