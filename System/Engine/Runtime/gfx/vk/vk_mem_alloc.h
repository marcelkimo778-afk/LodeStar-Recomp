/* A small stand-in for the Vulkan Memory Allocator (AMD, MIT) interface that
 * the ported Vulkan back end uses.  Same names and structure layouts as VMA
 * 3.x for the subset below; every allocation is its own vkAllocateMemory
 * (dedicated), memory types are chosen with VMA's usage/flag rules.
 * Implementation: sw_vma.c. */
#pragma once
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaPool)

typedef enum VmaMemoryUsage {
    VMA_MEMORY_USAGE_UNKNOWN = 0,
    VMA_MEMORY_USAGE_GPU_ONLY = 1,
    VMA_MEMORY_USAGE_CPU_ONLY = 2,
    VMA_MEMORY_USAGE_CPU_TO_GPU = 3,
    VMA_MEMORY_USAGE_GPU_TO_CPU = 4,
    VMA_MEMORY_USAGE_CPU_COPY = 5,
    VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED = 6,
    VMA_MEMORY_USAGE_AUTO = 7,
    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE = 8,
    VMA_MEMORY_USAGE_AUTO_PREFER_HOST = 9,
} VmaMemoryUsage;

typedef enum VmaAllocationCreateFlagBits {
    VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT = 0x00000001,
    VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT = 0x00000002,
    VMA_ALLOCATION_CREATE_MAPPED_BIT = 0x00000004,
    VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT = 0x00000020,
    VMA_ALLOCATION_CREATE_UPPER_ADDRESS_BIT = 0x00000040,
    VMA_ALLOCATION_CREATE_DONT_BIND_BIT = 0x00000080,
    VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT = 0x00000100,
    VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT = 0x00000200,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT = 0x00000400,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT = 0x00000800,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT = 0x00001000,
} VmaAllocationCreateFlagBits;
typedef VkFlags VmaAllocationCreateFlags;

typedef enum VmaAllocatorCreateFlagBits {
    VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT = 0x00000001,
    VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT = 0x00000002,
    VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT = 0x00000004,
    VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT = 0x00000008,
    VMA_ALLOCATOR_CREATE_AMD_DEVICE_COHERENT_MEMORY_BIT = 0x00000010,
    VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT = 0x00000020,
} VmaAllocatorCreateFlagBits;
typedef VkFlags VmaAllocatorCreateFlags;

typedef void (VKAPI_PTR *PFN_vmaAllocateDeviceMemoryFunction)(VmaAllocator allocator, uint32_t memoryType,
                                                               VkDeviceMemory memory, VkDeviceSize size, void *pUserData);
typedef void (VKAPI_PTR *PFN_vmaFreeDeviceMemoryFunction)(VmaAllocator allocator, uint32_t memoryType,
                                                           VkDeviceMemory memory, VkDeviceSize size, void *pUserData);
typedef struct VmaDeviceMemoryCallbacks {
    PFN_vmaAllocateDeviceMemoryFunction pfnAllocate;
    PFN_vmaFreeDeviceMemoryFunction pfnFree;
    void *pUserData;
} VmaDeviceMemoryCallbacks;

typedef struct VmaVulkanFunctions VmaVulkanFunctions;

typedef struct VmaAllocatorCreateInfo {
    VmaAllocatorCreateFlags flags;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkDeviceSize preferredLargeHeapBlockSize;
    const VkAllocationCallbacks *pAllocationCallbacks;
    const VmaDeviceMemoryCallbacks *pDeviceMemoryCallbacks;
    const VkDeviceSize *pHeapSizeLimit;
    const VmaVulkanFunctions *pVulkanFunctions;
    VkInstance instance;
    uint32_t vulkanApiVersion;
    const VkExternalMemoryHandleTypeFlagsKHR *pTypeExternalMemoryHandleTypes;
} VmaAllocatorCreateInfo;

typedef struct VmaAllocationCreateInfo {
    VmaAllocationCreateFlags flags;
    VmaMemoryUsage usage;
    VkMemoryPropertyFlags requiredFlags;
    VkMemoryPropertyFlags preferredFlags;
    uint32_t memoryTypeBits;
    VmaPool pool;
    void *pUserData;
    float priority;
} VmaAllocationCreateInfo;

typedef struct VmaPoolCreateInfo {
    uint32_t memoryTypeIndex;
    VkFlags flags;
    VkDeviceSize blockSize;
    size_t minBlockCount;
    size_t maxBlockCount;
    float priority;
    VkDeviceSize minAllocationAlignment;
    void *pMemoryAllocateNext;
} VmaPoolCreateInfo;

typedef struct VmaAllocationInfo {
    uint32_t memoryType;
    VkDeviceMemory deviceMemory;
    VkDeviceSize offset;
    VkDeviceSize size;
    void *pMappedData;
    void *pUserData;
    const char *pName;
} VmaAllocationInfo;

typedef struct VmaStatistics {
    uint32_t blockCount;
    uint32_t allocationCount;
    VkDeviceSize blockBytes;
    VkDeviceSize allocationBytes;
} VmaStatistics;

typedef struct VmaBudget {
    VmaStatistics statistics;
    VkDeviceSize usage;
    VkDeviceSize budget;
} VmaBudget;

VkResult vmaCreateAllocator(const VmaAllocatorCreateInfo *pCreateInfo, VmaAllocator *pAllocator);
void vmaDestroyAllocator(VmaAllocator allocator);
VkResult vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo *pBufferCreateInfo,
                         const VmaAllocationCreateInfo *pAllocationCreateInfo, VkBuffer *pBuffer,
                         VmaAllocation *pAllocation, VmaAllocationInfo *pAllocationInfo);
void vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);
VkResult vmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo *pImageCreateInfo,
                        const VmaAllocationCreateInfo *pAllocationCreateInfo, VkImage *pImage,
                        VmaAllocation *pAllocation, VmaAllocationInfo *pAllocationInfo);
void vmaDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation);
VkResult vmaAllocateMemory(VmaAllocator allocator, const VkMemoryRequirements *pVkMemoryRequirements,
                           const VmaAllocationCreateInfo *pCreateInfo, VmaAllocation *pAllocation,
                           VmaAllocationInfo *pAllocationInfo);
void vmaFreeMemory(VmaAllocator allocator, VmaAllocation allocation);
VkResult vmaBindBufferMemory(VmaAllocator allocator, VmaAllocation allocation, VkBuffer buffer);
VkResult vmaBindImageMemory(VmaAllocator allocator, VmaAllocation allocation, VkImage image);
VkResult vmaBindImageMemory2(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize allocationLocalOffset,
                             VkImage image, const void *pNext);
VkResult vmaBindBufferMemory2(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize allocationLocalOffset,
                              VkBuffer buffer, const void *pNext);
void vmaGetAllocationInfo(VmaAllocator allocator, VmaAllocation allocation, VmaAllocationInfo *pAllocationInfo);
VkResult vmaMapMemory(VmaAllocator allocator, VmaAllocation allocation, void **ppData);
void vmaUnmapMemory(VmaAllocator allocator, VmaAllocation allocation);
VkResult vmaFlushAllocation(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size);
VkResult vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset,
                                 VkDeviceSize size);
void vmaGetHeapBudgets(VmaAllocator allocator, VmaBudget *pBudgets);
VkResult vmaFindMemoryTypeIndexForBufferInfo(VmaAllocator allocator, const VkBufferCreateInfo *pBufferCreateInfo,
                                             const VmaAllocationCreateInfo *pAllocationCreateInfo,
                                             uint32_t *pMemoryTypeIndex);
VkResult vmaCreatePool(VmaAllocator allocator, const VmaPoolCreateInfo *pCreateInfo, VmaPool *pPool);
void vmaDestroyPool(VmaAllocator allocator, VmaPool pool);

#ifdef __cplusplus
}
#endif
