/* ============================================================================
 * Reverie Engine — render_vk.c
 * Vulkan renderer: GPU compute compositing (tier 3).
 *
 * Everything is loaded at runtime — libvulkan via dlopen, and the X11 WSI
 * via dlopen'd libxcb (no Xlib, no libxcb linkage). Wayland WSI uses the
 * optional wlshim (libwayland-client dlopen). Structure types, enum values
 * and struct layouts below are vendored by hand from the official
 * Khronos Vulkan headers (verified against vulkan_core.h 1.4.x).
 *
 * v0.2.0: the tier is a REAL GPU renderer. Every layer operation — the
 * W3C blend composite, the separable blur chain, gradient evaluation,
 * particle splats, video YUV->RGB, and the fluid + ripple solvers — runs
 * as a SPIR-V compute kernel (the .comp sources under src/shaders,
 * embedded in vk_spv.h)
 * on device images. The engine's CPU compositor is not in the frame path
 * anymore; the CPU only records dispatches and (on the platform present
 * path) copies the finished frame out for wl_shm/MIT-SHM present.
 *
 * Present paths (unchanged in mechanism since 0.1.1, fed by GPU frames):
 *   PLATFORM (default) — the final image is copied to a host-visible
 *     buffer and handed to the platform's proven presenter (wl_shm /
 *     MIT-SHM). Bit-identical presentation to the mechanism every field
 *     report confirmed; the GPU still does ALL rendering.
 *   WSI (REV_VK_PRESENT=wsi) — the final image is blitted into the
 *     swapchain and presented via the Vulkan WSI (zero-copy, FIFO).
 *
 * Synchronization: 3 frame slots per window, each with its own command
 * pool, descriptor pool, dynamic-UBO slice and upload arena; a slot is
 * reused only after its fence retired (and its descriptor sets were
 * reset). Images live in VK_IMAGE_LAYOUT_GENERAL for their whole
 * lifetime — correct on every driver, and the access masks carry the
 * ordering. Retired resources go through a graveyard freed three frames
 * later, so ping-pong reallocations never pull memory out from under an
 * in-flight frame.
 *
 * Timing: GPU timestamps (core Vulkan 1.0 query pools) bracket every
 * frame's compute; REV_PROFILE reports them asynchronously like the GL
 * tier's timer queries — several frames behind, never CPU wall time.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "render_vk.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "platform.h"
#include "util.h"
#include "vk_spv.h"

/* ---- minimal hand-vendored Vulkan declarations --------------------------
 * Values cross-checked against the official headers. */
typedef void *VkInstance_;       /* dispatchable */
typedef void *VkPhysicalDevice_; /* dispatchable */
typedef void *VkDevice_;         /* dispatchable */
typedef void *VkQueue_;          /* dispatchable */
typedef void *VkCommandBuffer_;  /* dispatchable */
typedef u64 VkSurfaceKHR_;
typedef u64 VkSwapchainKHR_;
typedef u64 VkImage_;
typedef u64 VkBuffer_;
typedef u64 VkDeviceMemory_;
typedef u64 VkFence_;
typedef u64 VkSemaphore_;
typedef u64 VkCommandPool_;
typedef u64 VkDescriptorPool_;
typedef u64 VkDescriptorSet_;
typedef u64 VkDescriptorSetLayout_;
typedef u64 VkPipelineLayout_;
typedef u64 VkPipeline_;
typedef u64 VkShaderModule_;
typedef u64 VkImageView_;
typedef u64 VkSampler_;
typedef u64 VkQueryPool_;
typedef u32 VkFlags32;

enum {
        VK_SUCCESS_ = 0,
        VK_TIMEOUT_ = 2,
        VK_INCOMPLETE_ = 5,
        VK_SUBOPTIMAL_KHR_ = 1000001003,
        VK_ERROR_OUT_OF_DATE_KHR_ = -1000001004,
        VK_ERROR_DEVICE_LOST_ = -4,
};

/* VkPhysicalDeviceType (vendored values, cross-checked against the header) */
#define VK_PHYSICAL_DEVICE_TYPE_OTHER_ 0
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_ 1
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_ 2
#define VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_ 3
#define VK_PHYSICAL_DEVICE_TYPE_CPU_ 4

enum {
        ST_APPLICATION_INFO = 0,
        ST_INSTANCE_CREATE_INFO = 1,
        ST_DEVICE_QUEUE_CREATE_INFO = 2,
        ST_DEVICE_CREATE_INFO = 3,
        ST_SUBMIT_INFO = 4,
        ST_MEMORY_ALLOCATE_INFO = 5,
        ST_MAPPED_MEMORY_RANGE = 6,
        ST_FENCE_CREATE_INFO = 8,
        ST_SEMAPHORE_CREATE_INFO = 9,
        ST_BUFFER_CREATE_INFO = 12,
        ST_IMAGE_CREATE_INFO = 14,
        ST_QUERY_POOL_CREATE_INFO = 11,
        ST_SAMPLER_CREATE_INFO = 31,
        ST_IMAGE_VIEW_CREATE_INFO = 15,
        ST_SHADER_MODULE_CREATE_INFO = 16,
        ST_COMMAND_BUFFER_ALLOCATE_INFO = 40,
        ST_COMMAND_BUFFER_BEGIN_INFO = 42,
        ST_PIPELINE_LAYOUT_CREATE_INFO = 30,
        ST_COMPUTE_PIPELINE_CREATE_INFO = 29,
        ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
        ST_DESCRIPTOR_POOL_CREATE_INFO = 33,
        ST_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
        ST_WRITE_DESCRIPTOR_SET = 35,
        ST_IMAGE_MEMORY_BARRIER = 45,
        ST_BUFFER_MEMORY_BARRIER = 44,
        ST_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
        ST_PRESENT_INFO_KHR = 1000001001,
        ST_XLIB_SURFACE_CREATE_INFO_KHR = 1000004000,
        ST_XCB_SURFACE_CREATE_INFO_KHR = 1000005000,
        ST_WAYLAND_SURFACE_CREATE_INFO_KHR = 1000006000,
};

#define VK_QUEUE_GRAPHICS_BIT_ 0x1u
#define VK_QUEUE_COMPUTE_BIT_ 0x2u
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT_ 0x1u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT_ 0x2u
#define VK_IMAGE_USAGE_SAMPLED_BIT_ 0x4u
#define VK_IMAGE_USAGE_STORAGE_BIT_ 0x8u
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_ 0x1u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ 0x2u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_ 0x4u
#define VK_MEMORY_PROPERTY_HOST_CACHED_BIT_ 0x8u
#define VK_ACCESS_TRANSFER_WRITE_BIT_ 0x1000u
#define VK_ACCESS_TRANSFER_READ_BIT_ 0x800u
#define VK_ACCESS_MEMORY_READ_BIT_ 0x8000u
#define VK_ACCESS_HOST_READ_BIT_ 0x2000u
#define VK_ACCESS_SHADER_READ_BIT_ 0x20u
#define VK_ACCESS_SHADER_WRITE_BIT_ 0x40u
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT_ 0x1u
#define VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT_ 0x800u
#define VK_PIPELINE_STAGE_TRANSFER_BIT_ 0x1000u
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT_ 0x2000u
#define VK_PIPELINE_STAGE_HOST_BIT_ 0x4000u
#define VK_IMAGE_ASPECT_COLOR_BIT_ 0x1u
#define VK_IMAGE_LAYOUT_UNDEFINED_ 0
#define VK_IMAGE_LAYOUT_GENERAL_ 1
#define VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_ 7
#define VK_IMAGE_LAYOUT_PRESENT_SRC_KHR_ 1000001002
#define VK_FORMAT_R8G8B8A8_UNORM_ 37
#define VK_FORMAT_B8G8R8A8_UNORM_ 44
#define VK_FORMAT_R8_UNORM_ 9
#define VK_FORMAT_R16G16B16A16_SFLOAT_ 97
#define VK_FORMAT_R32G32B32A32_SFLOAT_ 109
#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR_ 0
#define VK_PRESENT_MODE_FIFO_KHR_ 2
#define VK_IMAGE_TILING_OPTIMAL_ 0
#define VK_IMAGE_TYPE_2D_ 1
#define VK_SAMPLE_COUNT_1_BIT_ 0x1u
#define VK_SHARING_MODE_EXCLUSIVE_ 0
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT_ 0x1u
#define VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT_ 0x10u
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT_ 0x2u
#define VK_WHOLE_SIZE_ (~(u64)0)
#define VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR_ 0x1u
#define VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR_ 0x1u
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY_ 0
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT_ 0x1u
#define VK_API_VERSION_1_0_ 0x400000u /* (1<<22)|(0<<12)|0 */
#define VK_MAX_MEMORY_TYPES_ 32
#define VK_MAX_MEMORY_HEAPS_ 16

/* descriptor types / pool flags / stages (verified against the header) */
#define VK_DESCRIPTOR_TYPE_SAMPLER_ 0
#define VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER_ 1
#define VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE_ 2
#define VK_DESCRIPTOR_TYPE_STORAGE_IMAGE_ 3
#define VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_ 6
#define VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC_ 8
#define VK_FILTER_NEAREST_ 0
#define VK_FILTER_LINEAR_ 1
#define VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE_ 2
#define VK_SHADER_STAGE_COMPUTE_BIT_ 0x20u
#define VK_PIPELINE_BIND_POINT_COMPUTE_ 1
#define VK_QUERY_TYPE_TIMESTAMP_ 2
#define VK_QUERY_RESULT_64_BIT_ 0x1u
#define VK_QUERY_RESULTAvailability_ 0x2u /* VK_QUERY_RESULT_WITH_AVAILABILITY */

/* format feature bits (VkFormatFeatureFlagBits) */
#define VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT_ 0x1u
#define VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT_ 0x2u
#define VK_FORMAT_FEATURE_BLIT_SRC_BIT_ 0x400u
#define VK_FORMAT_FEATURE_BLIT_DST_BIT_ 0x800u

typedef struct { u32 width, height; } VkExtent2D_;
typedef struct { u32 width, height, depth; } VkExtent3D_;
typedef struct { i32 x, y, z; } VkOffset3D_;
typedef struct { char name[256]; u32 specVersion; } VkExtensionProperties_;

typedef struct {
        u32 sType_;
        const void *pNext;
        const char *appName;
        u32 appVersion;
        const char *engineName;
        u32 engineVersion;
        u32 apiVersion;
} VkApplicationInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        const VkApplicationInfo_ *pAppInfo;
        u32 enabledLayerCount;
        const char *const *ppEnabledLayerNames;
        u32 enabledExtensionCount;
        const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 queueFamilyIndex;
        u32 queueCount;
        const f32 *pQueuePriorities;
} VkDeviceQueueCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 queueCreateInfoCount;
        const VkDeviceQueueCreateInfo_ *pQueueCreateInfos;
        u32 enabledLayerCount;
        const char *const *ppEnabledLayerNames;
        u32 enabledExtensionCount;
        const char *const *ppEnabledExtensionNames;
        const void *pEnabledFeatures;
} VkDeviceCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u64 allocationSize;
        u32 memoryTypeIndex;
} VkMemoryAllocateInfo_;

typedef struct { u32 sType_; const void *pNext; u32 flags; } VkSemaphoreCreateInfo_;
typedef struct { u32 sType_; const void *pNext; u32 flags; } VkFenceCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u64 size;
        u32 usage;
        u32 sharingMode;
        u32 queueFamilyIndexCount;
        const u32 *pQueueFamilyIndices;
} VkBufferCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 imageType;
        u32 format;
        VkExtent3D_ extent;
        u32 mipLevels;
        u32 arrayLayers;
        u32 samples;
        u32 tiling;
        u32 usage;
        u32 sharingMode;
        u32 queueFamilyIndexCount;
        const u32 *pQueueFamilyIndices;
        u32 initialLayout;
} VkImageCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        VkCommandPool_ commandPool;
        u32 level;
        u32 commandBufferCount;
} VkCommandBufferAllocateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        const void *pInheritanceInfo;
} VkCommandBufferBeginInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 waitSemaphoreCount;
        const VkSemaphore_ *pWaitSemaphores;
        const u32 *pWaitDstStageMask;
        u32 commandBufferCount;
        const VkCommandBuffer_ *pCommandBuffers;
        u32 signalSemaphoreCount;
        const VkSemaphore_ *pSignalSemaphores;
} VkSubmitInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 srcAccessMask;
        u32 dstAccessMask;
        u32 oldLayout;
        u32 newLayout;
        u32 srcQueueFamilyIndex;
        u32 dstQueueFamilyIndex;
        VkImage_ image;
        struct { u32 aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; } subresourceRange;
} VkImageMemoryBarrier_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 srcAccessMask;
        u32 dstAccessMask;
        u32 srcQueueFamilyIndex;    /* VK_QUEUE_FAMILY_IGNORED_ = 0 */
        u32 dstQueueFamilyIndex;
        VkBuffer_ buffer;
        u64 offset;
        u64 size;
} VkBufferMemoryBarrier_;

typedef struct {
        u32 sType_;
        const void *pNext;
        VkDeviceMemory_ memory;
        u64 offset;
        u64 size;
} VkMappedMemoryRange_;

typedef struct {
        u64 bufferOffset;
        u32 bufferRowLength;
        u32 bufferImageHeight;
        struct { u32 aspectMask, mipLevel, baseArrayLayer, layerCount; } imageSubresource;
        VkOffset3D_ imageOffset;
        VkExtent3D_ imageExtent;
} VkBufferImageCopy_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        VkSurfaceKHR_ surface;
        u32 minImageCount;
        u32 imageFormat;
        u32 imageColorSpace;
        VkExtent2D_ imageExtent;
        u32 imageArrayLayers;
        u32 imageUsage;
        u32 imageSharingMode;
        u32 queueFamilyIndexCount;
        const u32 *pQueueFamilyIndices;
        u32 preTransform;
        u32 compositeAlpha;
        u32 presentMode;
        u32 clipped;
        VkSwapchainKHR_ oldSwapchain;
} VkSwapchainCreateInfoKHR_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 waitSemaphoreCount;
        const VkSemaphore_ *pWaitSemaphores;
        u32 swapchainCount;
        const VkSwapchainKHR_ *pSwapchains;
        const u32 *pImageIndices;
        i32 *pResults;
} VkPresentInfoKHR_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 queueFamilyIndex;
} VkCommandPoolCreateInfo_;

typedef struct { u32 propertyFlags, heapIndex; } VkMemoryType_;
typedef struct { u64 size; u32 flags; } VkMemoryHeap_;
typedef struct {
        u32 memoryTypeCount;
        VkMemoryType_ memoryTypes[VK_MAX_MEMORY_TYPES_];
        u32 memoryHeapCount;
        VkMemoryHeap_ memoryHeaps[VK_MAX_MEMORY_HEAPS_];
} VkPhysicalDeviceMemoryProperties_;

typedef struct {
        u32 queueFlags;
        u32 queueCount;
        u32 timestampValidBits;
        VkExtent3D_ minImageTransferGranularity;
} VkQueueFamilyProperties_;

typedef struct {
        u32 minImageCount, maxImageCount;
        VkExtent2D_ currentExtent, minImageExtent, maxImageExtent;
        u32 maxImageArrayLayers;
        u32 supportedTransforms, currentTransform;
        u32 supportedCompositeAlpha;
        u32 supportedUsageFlags;
} VkSurfaceCapabilitiesKHR_;

typedef struct { u32 format, colorSpace; } VkSurfaceFormatKHR_;
typedef struct { u32 linearTilingFeatures, optimalTilingFeatures, bufferFeatures; } VkFormatProperties_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        void *connection;   /* xcb_connection_t* */
        u32 window;         /* xcb_window_t */
} VkXcbSurfaceCreateInfoKHR_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        void *dpy;          /* Display* */
        u64 window;         /* Xlib Window */
} VkXlibSurfaceCreateInfoKHR_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        void *display;      /* struct wl_display* */
        void *surface;      /* struct wl_surface* */
} VkWaylandSurfaceCreateInfoKHR_;

/* VkPhysicalDeviceProperties head. The real structure continues with
 * VkPhysicalDeviceLimits (504 bytes) + sparseProperties; we only read the
 * head plus two limits fields (timestampPeriod @ 720, computed against
 * the header's member order), but the driver writes the FULL struct, so
 * the buffer must be sized for it — 1024 bytes covers the 1.x ABI. */
#define VKPDPROPS_SIZE_ 1024
typedef struct {
        u32 apiVersion;
        u32 driverVersion;
        u32 vendorID;
        u32 deviceID;
        u32 deviceType;
        char deviceName[256];
        u8 pipelineCacheUUID[16];
        u8 tail[VKPDPROPS_SIZE_ - 288];    /* limits + sparseProperties */
} VkPhysicalDeviceProperties_;
#define VKPD_TIMESTAMP_PERIOD_OFF_ 720     /* limits.timestampPeriod   */
#define VKPD_TIMESTAMP_CAG_OFF_ 716        /* limits.timestampComputeAndGraphics */

/* ---- new: descriptor / pipeline / query declarations --------------------- */
typedef struct {
        u32 binding;
        u32 descriptorType;
        u32 descriptorCount;
        u32 stageFlags;
        const VkSampler_ *pImmutableSamplers;
} VkDescriptorSetLayoutBinding_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 bindingCount;
        const VkDescriptorSetLayoutBinding_ *pBindings;
} VkDescriptorSetLayoutCreateInfo_;

typedef struct { u32 type, descriptorCount; } VkDescriptorPoolSize_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 maxSets;
        u32 poolSizeCount;
        const VkDescriptorPoolSize_ *pPoolSizes;
} VkDescriptorPoolCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        VkDescriptorPool_ descriptorPool;
        u32 descriptorSetCount;
        const VkDescriptorSetLayout_ *pSetLayouts;
} VkDescriptorSetAllocateInfo_;

typedef struct { VkSampler_ sampler; VkImageView_ imageView; u32 imageLayout; } VkDescriptorImageInfo_;
typedef struct { VkBuffer_ buffer; u64 offset; u64 range; } VkDescriptorBufferInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        VkDescriptorSet_ dstSet;
        u32 dstBinding;
        u32 dstArrayElement;
        u32 descriptorCount;
        u32 descriptorType;
        const VkDescriptorImageInfo_ *pImageInfo;
        const VkDescriptorBufferInfo_ *pBufferInfo;
        const void *pTexelBufferView;
} VkWriteDescriptorSet_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 magFilter, minFilter, mipmapMode;
        u32 addressModeU, addressModeV, addressModeW;
        f32 mipLodBias;
        u32 anisotropyEnable;
        f32 maxAnisotropy;
        u32 compareEnable;
        u32 compareOp;
        f32 minLod, maxLod;
        u32 borderColor;
        u32 unnormalizedCoordinates;
} VkSamplerCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        VkImage_ image;
        u32 viewType;
        u32 format;
        struct { u32 r, g, b, a; } components_; /* VkComponentMapping */
        struct { u32 aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; } subresourceRange;
} VkImageViewCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u64 codeSize;
        const u32 *pCode;
} VkShaderModuleCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 stage;
        VkShaderModule_ module;
        const char *pName;
        const void *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 setLayoutCount;
        const VkPipelineLayout_ *pSetLayouts;
        u32 pushConstantRangeCount;
        const void *pPushConstantRanges;
} VkPipelineLayoutCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        VkPipelineShaderStageCreateInfo_ stage;
        VkPipelineLayout_ layout;
        VkPipeline_ basePipelineHandle;
        i32 basePipelineIndex;
} VkComputePipelineCreateInfo_;

typedef struct {
        u32 sType_;
        const void *pNext;
        u32 flags;
        u32 queryType;
        u32 queryCount;
        u32 pipelineStatistics;
} VkQueryPoolCreateInfo_;

typedef struct {
        struct { u32 aspectMask, mipLevel, baseArrayLayer, layerCount; } srcSubresource;
        VkOffset3D_ srcOffsets[2];
        struct { u32 aspectMask, mipLevel, baseArrayLayer, layerCount; } dstSubresource;
        VkOffset3D_ dstOffsets[2];
} VkImageBlit_;

typedef union { f32 f32_[4]; i32 i32_[4]; u32 u32_[4]; } VkClearColorValue_;

/* ---- function pointer types ---------------------------------------------- */
typedef void *(*PFN_vkGetInstanceProcAddr_)(void *, const char *);
typedef void *(*PFN_vkGetDeviceProcAddr_)(void *, const char *);
typedef u32 (*PFN_createInstance_)(const VkInstanceCreateInfo_ *, const void *, VkInstance_ *);
typedef u32 (*PFN_enumInstExt_)(const char *, u32 *, VkExtensionProperties_ *);
typedef u32 (*PFN_enumPhysDev_)(VkInstance_, u32 *, VkPhysicalDevice_ *);
typedef u32 (*PFN_getSurfSupport_)(VkPhysicalDevice_, u32, VkSurfaceKHR_, u32 *);
typedef u32 (*PFN_getSurfCaps_)(VkPhysicalDevice_, VkSurfaceKHR_, VkSurfaceCapabilitiesKHR_ *);
typedef u32 (*PFN_getSurfFormats_)(VkPhysicalDevice_, VkSurfaceKHR_, u32 *, VkSurfaceFormatKHR_ *);
typedef void (*PFN_getQueueFamProps_)(VkPhysicalDevice_, u32 *, VkQueueFamilyProperties_ *);
typedef void (*PFN_getPhysDevProps_)(VkPhysicalDevice_, VkPhysicalDeviceProperties_ *);
typedef void (*PFN_getFormatProps_)(VkPhysicalDevice_, u32, VkFormatProperties_ *);
typedef u32 (*PFN_createDevice_)(VkPhysicalDevice_, const VkDeviceCreateInfo_ *, const void *, VkDevice_ *);
typedef void (*PFN_getDeviceQueue_)(VkDevice_, u32, u32, VkQueue_ *);
typedef u32 (*PFN_createXcbSurface_)(VkInstance_, const VkXcbSurfaceCreateInfoKHR_ *, const void *, VkSurfaceKHR_ *);
typedef u32 (*PFN_createXlibSurface_)(VkInstance_, const VkXlibSurfaceCreateInfoKHR_ *, const void *, VkSurfaceKHR_ *);
typedef u32 (*PFN_createWaylandSurface_)(VkInstance_, const VkWaylandSurfaceCreateInfoKHR_ *, const void *, VkSurfaceKHR_ *);
typedef u32 (*PFN_createSwapchain_)(VkDevice_, const VkSwapchainCreateInfoKHR_ *, const void *, VkSwapchainKHR_ *);
typedef u32 (*PFN_getSwapchainImages_)(VkDevice_, VkSwapchainKHR_, u32 *, VkImage_ *);
typedef void (*PFN_destroySwapchain_)(VkDevice_, VkSwapchainKHR_, const void *);
typedef u32 (*PFN_createCommandPool_)(VkDevice_, const void *, const void *, VkCommandPool_ *);
typedef u32 (*PFN_allocCmdBufs_)(VkDevice_, const VkCommandBufferAllocateInfo_ *, VkCommandBuffer_ *);
typedef u32 (*PFN_resetCommandPool_)(VkDevice_, VkCommandPool_, u32);
typedef u32 (*PFN_createBuffer_)(VkDevice_, const VkBufferCreateInfo_ *, const void *, VkBuffer_ *);
typedef u32 (*PFN_createImage_)(VkDevice_, const VkImageCreateInfo_ *, const void *, VkImage_ *);
typedef void (*PFN_getBufferMemReqs_)(VkDevice_, VkBuffer_, void *);
typedef void (*PFN_getImageMemReqs_)(VkDevice_, VkImage_, void *);
typedef void (*PFN_getPhysDevMemProps_)(VkPhysicalDevice_, VkPhysicalDeviceMemoryProperties_ *);
typedef u32 (*PFN_allocMemory_)(VkDevice_, const VkMemoryAllocateInfo_ *, const void *, VkDeviceMemory_ *);
typedef u32 (*PFN_mapMemory_)(VkDevice_, VkDeviceMemory_, u64, u64, u32, void **);
typedef void (*PFN_unmapMemory_)(VkDevice_, VkDeviceMemory_);
typedef u32 (*PFN_invalidateMemRanges_)(VkDevice_, u32, const VkMappedMemoryRange_ *);
typedef u32 (*PFN_bindBufferMemory_)(VkDevice_, VkBuffer_, VkDeviceMemory_, u64);
typedef u32 (*PFN_bindImageMemory_)(VkDevice_, VkImage_, VkDeviceMemory_, u64);
typedef u32 (*PFN_createFence_)(VkDevice_, const VkFenceCreateInfo_ *, const void *, VkFence_ *);
typedef u32 (*PFN_createSemaphore_)(VkDevice_, const VkSemaphoreCreateInfo_ *, const void *, VkSemaphore_ *);
typedef u32 (*PFN_waitForFences_)(VkDevice_, u32, const VkFence_ *, u32, u64);
typedef u32 (*PFN_resetFences_)(VkDevice_, u32, const VkFence_ *);
typedef u32 (*PFN_acquireNextImage_)(VkDevice_, VkSwapchainKHR_, u64, VkSemaphore_, VkFence_, u32 *);
typedef u32 (*PFN_resetCmdBuf_)(VkCommandBuffer_, u32);
typedef u32 (*PFN_beginCmdBuf_)(VkCommandBuffer_, const VkCommandBufferBeginInfo_ *);
typedef u32 (*PFN_endCmdBuf_)(VkCommandBuffer_);
typedef void (*PFN_cmdPipelineBarrier_)(VkCommandBuffer_, u32, u32, u32, u32, const void *, u32, const void *, u32, const VkImageMemoryBarrier_ *);
typedef void (*PFN_cmdCopyBufferToImage_)(VkCommandBuffer_, VkBuffer_, VkImage_, u32, u32, const VkBufferImageCopy_ *);
typedef void (*PFN_cmdCopyImageToBuffer_)(VkCommandBuffer_, VkImage_, u32, VkBuffer_, u32, const VkBufferImageCopy_ *);
typedef void (*PFN_cmdBlitImage_)(VkCommandBuffer_, VkImage_, u32, VkImage_, u32, u32, const VkImageBlit_ *, u32);
typedef void (*PFN_cmdClearColorImage_)(VkCommandBuffer_, VkImage_, u32, const VkClearColorValue_ *, u32, const void *);
typedef u32 (*PFN_queueSubmit_)(VkQueue_, u32, const VkSubmitInfo_ *, VkFence_);
typedef u32 (*PFN_queuePresent_)(VkQueue_, const VkPresentInfoKHR_ *);
typedef void (*PFN_destroyFence_)(VkDevice_, VkFence_, const void *);
typedef void (*PFN_destroySemaphore_)(VkDevice_, VkSemaphore_, const void *);
typedef void (*PFN_destroyBuffer_)(VkDevice_, VkBuffer_, const void *);
typedef void (*PFN_destroyImage_)(VkDevice_, VkImage_, const void *);
typedef void (*PFN_destroyImageView_)(VkDevice_, VkImageView_, const void *);
typedef void (*PFN_freeMemory_)(VkDevice_, VkDeviceMemory_, const void *);
typedef void (*PFN_destroyCommandPool_)(VkDevice_, VkCommandPool_, const void *);
typedef void (*PFN_destroySurface_)(VkInstance_, VkSurfaceKHR_, const void *);
typedef void (*PFN_destroyDevice_)(VkDevice_, const void *);
typedef void (*PFN_destroyInstance_)(VkInstance_, const void *);
typedef void (*PFN_deviceWaitIdle_)(VkDevice_);

/* compute / descriptor / query entry points */
typedef u32 (*PFN_createDescSetLayout_)(VkDevice_, const VkDescriptorSetLayoutCreateInfo_ *, const void *, VkDescriptorSetLayout_ *);
typedef void (*PFN_destroyDescSetLayout_)(VkDevice_, VkDescriptorSetLayout_, const void *);
typedef u32 (*PFN_createDescPool_)(VkDevice_, const VkDescriptorPoolCreateInfo_ *, const void *, VkDescriptorPool_ *);
typedef void (*PFN_destroyDescPool_)(VkDevice_, VkDescriptorPool_, const void *);
typedef u32 (*PFN_resetDescPool_)(VkDevice_, VkDescriptorPool_, u32);
typedef u32 (*PFN_allocDescSets_)(VkDevice_, const VkDescriptorSetAllocateInfo_ *, VkDescriptorSet_ *);
typedef void (*PFN_updateDescSets_)(VkDevice_, u32, const VkWriteDescriptorSet_ *, u32, const void *);
typedef u32 (*PFN_createSampler_)(VkDevice_, const VkSamplerCreateInfo_ *, const void *, VkSampler_ *);
typedef void (*PFN_destroySampler_)(VkDevice_, VkSampler_, const void *);
typedef u32 (*PFN_createImageView_)(VkDevice_, const VkImageViewCreateInfo_ *, const void *, VkImageView_ *);
typedef u32 (*PFN_createShaderModule_)(VkDevice_, const VkShaderModuleCreateInfo_ *, const void *, VkShaderModule_);
typedef void (*PFN_destroyShaderModule_)(VkDevice_, VkShaderModule_, const void *);
typedef u32 (*PFN_createPipelineLayout_)(VkDevice_, const VkPipelineLayoutCreateInfo_ *, const void *, VkPipelineLayout_ *);
typedef void (*PFN_destroyPipelineLayout_)(VkDevice_, VkPipelineLayout_, const void *);
typedef u32 (*PFN_createComputePipelines_)(VkDevice_, VkPipeline_, u32, const VkComputePipelineCreateInfo_ *, const void *, VkPipeline_ *);
typedef void (*PFN_destroyPipeline_)(VkDevice_, VkPipeline_, const void *);
typedef void (*PFN_cmdBindPipeline_)(VkCommandBuffer_, u32, VkPipeline_);
typedef void (*PFN_cmdBindDescriptorSets_)(VkCommandBuffer_, u32, VkPipelineLayout_, u32, u32, const VkDescriptorSet_ *, u32, const u32 *);
typedef void (*PFN_cmdDispatch_)(VkCommandBuffer_, u32, u32, u32);
typedef u32 (*PFN_createQueryPool_)(VkDevice_, const VkQueryPoolCreateInfo_ *, const void *, VkQueryPool_ *);
typedef void (*PFN_destroyQueryPool_)(VkDevice_, VkQueryPool_, const void *);
typedef u32 (*PFN_getQueryPoolResults_)(VkDevice_, VkQueryPool_, u32, u32, u64, void *, u64, u32);
typedef void (*PFN_cmdResetQueryPool_)(VkCommandBuffer_, VkQueryPool_, u32, u32);
typedef void (*PFN_cmdWriteTimestamp_)(VkCommandBuffer_, u32, VkQueryPool_, u32);

/* ---- per-frame sync slot (2 in flight for the swapchain path) ------------- */
typedef struct {
        VkSemaphore_ acq, rend;
        VkFence_ fence;
} VkSync_;

/* ---- device image (image + memory + view, GENERAL layout for life) ------- */
typedef struct {
        VkImage_ img;
        VkDeviceMemory_ mem;
        VkImageView_ view;
        u32 w, h;
        u32 format;
} VImg;

/* ---- deferred destruction (ping-pong reallocs stay valid 3+ frames) ------- */
#define VK_GRAVE 64
typedef struct {
        VkImage_ img;
        VkImageView_ view;
        VkBuffer_ buf;
        VkDeviceMemory_ mem;
        u64 frame;
} VGrave;

/* ---- GPU sim state (fluid OR ripple, keyed by descriptor address) -------- */
#define VK_MAX_SIMS 8

typedef struct {
        const void *key;     /* RSimFluid* / RSimRipple* (stable address) */
        u64 last_seen;
        bool is_ripple;
        bool ok;
        /* fluid fields (RGBA16F) */
        VImg vel[2], dye[2], press[2], curl, out8;
        u32 gw, gh, dw, dh;
        int vc, dc, pc;
        f64 amb_t, wisp_acc;
        /* ripple field (RGBA16F) */
        VImg field[2];
        u32 rgw, rgh;
        int cur;
} VKSim;

/* ---- content-addressed layer texture cache -------------------------------- */
#define VK_TEX_CACHE 16
typedef struct {
        u64 id, stamp;
        VImg im;
} VTexSlot;

/* ---- rendered-gradient cache ---------------------------------------------- */
#define VK_GRAD_CACHE 8
typedef struct {
        u64 id, stamp;
        VImg im;
} VGradSlot;

/* ---- per-window Vulkan state ---------------------------------------------- */
typedef struct {
        VkSurfaceKHR_ surface;
        VkSwapchainKHR_ swapchain;
        VkImage_ images[8];
        u32 nimages;
        u32 width, height;
        u32 format;              /* VK_FORMAT_* */
        bool srgb_format;        /* _SRGB variant: present via byte copy  */
        VImg accum[2];            /* ping-pong composite targets           */
        int accum_cur;
        VkBuffer_ readback;       /* host-visible final-frame download     */
        VkDeviceMemory_ readback_mem;
        u8 *rb_map;
        u64 rb_size;
        bool rb_coherent;        /* HOST_COHERENT? else invalidate needed */
        VkSync_ sync[2];
        int sync_slot;
        bool ready;
} VkWin_;

/* ---- per-frame recording slot (3 in flight per window) -------------------- */
#define VK_SLOTS 3
#define VK_UBO_SLOT 512          /* bytes per dispatch's uniform block     */
#define VK_UBO_CAP (512 * VK_UBO_SLOT)
#define VK_MAX_DISPATCHES 512

typedef struct {
        bool created;
        VkCommandPool_ cmdpool;
        VkCommandBuffer_ cmd;
        VkDescriptorPool_ dpool;
        VkBuffer_ ubo;
        VkDeviceMemory_ ubo_mem;
        u8 *ubo_map;
        u32 ubo_used;             /* bytes                                  */
        VkBuffer_ arena;          /* upload staging arena                  */
        VkDeviceMemory_ arena_mem;
        u8 *arena_map;
        u64 arena_cap, arena_used;
        VkFence_ fence;
        bool submitted;
        bool recording;
        u32 layers;               /* composites emitted this window-frame  */
        bool first_pending;       /* accum not yet initialized (u_first)   */
        bool ts_valid;            /* this slot's query pair fully armed    */
} VFrame;

/* Present-path selection (unchanged since 0.1.1):
 *   PLATFORM: frames flow through the platform's proven presenter — MIT-SHM
 *     on X11, wl_shm on Wayland. This is the DEFAULT: bit-identical to the
 *     mechanism that worked everywhere in field reports. The GPU does all
 *     the rendering; the finished frame is copied out and presented.
 *   WSI: the swapchain path — final image blitted into the swapchain and
 *     presented via the Vulkan WSI. Opt in with REV_VK_PRESENT=wsi. */
#define VK_PRESENT_PLATFORM 0
#define VK_PRESENT_WSI 1

/* compute kernels (index order matches the table in vk_init) */
enum {
        K_COMPOSITE = 0, K_COPYPREMUL, K_BLUR, K_GRADIENT, K_PARTICLES,
        K_VIDEO, K_DECODE, K_RIPPLE_SPLAT, K_RIPPLE_STEP, K_RIPPLE_COMPOSITE,
        K_F_VELF, K_F_DYEF, K_F_CURL, K_F_VORT, K_F_DIV, K_F_JACOBI,
        K_F_GRADSUB, K_F_ADV_VEL, K_F_ADV_DYE, K_F_TONEMAP,
        K_COUNT
};

typedef struct {
        void *lib;
        void *libxcb;            /* X11 WSI transport */
        void *xcb_conn;
        VkInstance_ inst;
        VkPhysicalDevice_ phys;
        VkDevice_ dev;
        VkQueue_ queue;
        u32 queue_family;
        u32 ts_valid_bits;       /* queue family timestampValidBits         */
        f64 ts_period;           /* limits.timestampPeriod (ns)             */
        int present_mode;        /* VK_PRESENT_PLATFORM | VK_PRESENT_WSI    */
        char dev_name[256];      /* selected physical device name           */
        int dev_type;            /* VK_PHYSICAL_DEVICE_TYPE_*               */
        bool dev_is_software;    /* CPU-type device (lavapipe-class)        */
        u32 api_version;         /* device apiVersion                       */
        u32 driver_version;      /* device driverVersion (raw)              */
        bool device_lost;        /* VK_ERROR_DEVICE_LOST seen at runtime    */
        VkWin_ win[REV_PLATFORM_MAX_OUTPUTS];
        int cur_win;
        int cur_slot;            /* frame slot being recorded               */
        u64 frame_no;

        /* compute tier */
        VkDescriptorSetLayout_ dsl;
        VkPipelineLayout_ pl;
        VkPipeline_ pipe[K_COUNT];
        VkSampler_ samp_nearest, samp_linear;
        VkQueryPool_ tq;
        bool tq_ok;
        bool fmt16_ok;           /* RGBA16F sampled+storage (sims)          */
        bool fmt32_ok;           /* RGBA32F sampled (particle bands)       */

        /* per-frame slots x windows (lazily created) */
        VFrame frame[VK_SLOTS][REV_PLATFORM_MAX_OUTPUTS];

        /* scratch blur chain (reused across windows, sequential) */
        VImg tex_a, tex_b, tex_c, tex_d;

        /* particle layer: RGBA32F band image + frame-size layer target   */
        VImg ptex, ptarget;

        /* video planes (R8) */
        VImg v_y, v_u, v_v;
        u32 v_w, v_h;

        VTexSlot texcache[VK_TEX_CACHE];
        u64 cache_clock;
        VGradSlot gcache[VK_GRAD_CACHE];
        u64 g_clock;

        VKSim sims[REV_PLATFORM_MAX_OUTPUTS][VK_MAX_SIMS];

        /* deferred destruction */
        VGrave grave[VK_GRAVE];
        int grave_n;

        /* perf counters (read-and-reset through ->perf, GL-tier semantics) */
        f64 perf_upload_ms;
        f64 perf_gpu_ms_sum;
        u32 perf_gpu_samples;
        u32 dispatches;          /* this frame-window (overflow guard)     */
        bool dispatch_warned;

        /* instance / device / WSI entry points */
        PFN_vkGetInstanceProcAddr_ gipa;
        PFN_vkGetDeviceProcAddr_ gdpa;
        PFN_createInstance_ vkCreateInstance;
        PFN_enumInstExt_ vkEnumerateInstanceExtensionProperties;
        PFN_enumPhysDev_ vkEnumeratePhysicalDevices;
        PFN_getSurfSupport_ vkGetPhysicalDeviceSurfaceSupportKHR;
        PFN_getSurfCaps_ vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
        PFN_getSurfFormats_ vkGetPhysicalDeviceSurfaceFormatsKHR;
        PFN_getQueueFamProps_ vkGetPhysicalDeviceQueueFamilyProperties;
        PFN_getPhysDevProps_ vkGetPhysicalDeviceProperties;
        PFN_getFormatProps_ vkGetPhysicalDeviceFormatProperties;
        PFN_createDevice_ vkCreateDevice;
        PFN_getDeviceQueue_ vkGetDeviceQueue;
        PFN_createXcbSurface_ vkCreateXcbSurfaceKHR;
        PFN_createXlibSurface_ vkCreateXlibSurfaceKHR;
        PFN_createWaylandSurface_ vkCreateWaylandSurfaceKHR;
        PFN_createSwapchain_ vkCreateSwapchainKHR;
        PFN_getSwapchainImages_ vkGetSwapchainImagesKHR;
        void (*vkDestroySwapchainKHR)(VkDevice_, VkSwapchainKHR_, const void *);
        PFN_createCommandPool_ vkCreateCommandPool;
        PFN_allocCmdBufs_ vkAllocateCommandBuffers;
        PFN_resetCommandPool_ vkResetCommandPool;
        PFN_createBuffer_ vkCreateBuffer;
        PFN_createImage_ vkCreateImage;
        void (*vkGetBufferMemoryRequirements)(VkDevice_, VkBuffer_, void *);
        PFN_getImageMemReqs_ vkGetImageMemoryRequirements;
        PFN_getPhysDevMemProps_ vkGetPhysicalDeviceMemoryProperties;
        PFN_allocMemory_ vkAllocateMemory;
        PFN_mapMemory_ vkMapMemory;
        void (*vkUnmapMemory)(VkDevice_, VkDeviceMemory_);
        PFN_invalidateMemRanges_ vkInvalidateMappedMemoryRanges;
        PFN_bindBufferMemory_ vkBindBufferMemory;
        u32 (*vkBindImageMemory)(VkDevice_, VkImage_, VkDeviceMemory_, u64);
        PFN_createFence_ vkCreateFence;
        PFN_createSemaphore_ vkCreateSemaphore;
        PFN_waitForFences_ vkWaitForFences;
        PFN_resetFences_ vkResetFences;
        PFN_acquireNextImage_ vkAcquireNextImageKHR;
        PFN_resetCmdBuf_ vkResetCommandBuffer;
        PFN_beginCmdBuf_ vkBeginCommandBuffer;
        PFN_endCmdBuf_ vkEndCommandBuffer;
        PFN_cmdPipelineBarrier_ vkCmdPipelineBarrier;
        PFN_cmdCopyBufferToImage_ vkCmdCopyBufferToImage;
        void (*vkCmdCopyImageToBuffer)(VkCommandBuffer_, VkImage_, u32, VkBuffer_, u32, const VkBufferImageCopy_ *);
        void (*vkCmdBlitImage)(VkCommandBuffer_, VkImage_, u32, VkImage_, u32, u32, const VkImageBlit_ *, u32);
        void (*vkCmdClearColorImage)(VkCommandBuffer_, VkImage_, u32, const VkClearColorValue_ *, u32, const void *);
        PFN_queueSubmit_ vkQueueSubmit;
        PFN_queuePresent_ vkQueuePresentKHR;
        void (*vkDestroyFence)(VkDevice_, VkFence_, const void *);
        void (*vkDestroySemaphore)(VkDevice_, VkSemaphore_, const void *);
        void (*vkDestroyBuffer)(VkDevice_, VkBuffer_, const void *);
        void (*vkDestroyImage)(VkDevice_, VkImage_, const void *);
        void (*vkDestroyImageView)(VkDevice_, VkImageView_, const void *);
        void (*vkFreeMemory)(VkDevice_, VkDeviceMemory_, const void *);
        void (*vkDestroyCommandPool)(VkDevice_, VkCommandPool_, const void *);
        void (*vkDestroySurfaceKHR)(VkInstance_, VkSurfaceKHR_, const void *);
        void (*vkDestroyDevice)(VkDevice_, const void *);
        void (*vkDestroyInstance)(VkInstance_, const void *);
        void (*vkDeviceWaitIdle)(VkDevice_);

        /* v0.2 compute tier */
        PFN_createDescSetLayout_ vkCreateDescriptorSetLayout;
        void (*vkDestroyDescriptorSetLayout)(VkDevice_, VkDescriptorSetLayout_, const void *);
        PFN_createDescPool_ vkCreateDescriptorPool;
        void (*vkDestroyDescriptorPool)(VkDevice_, VkDescriptorPool_, const void *);
        PFN_resetDescPool_ vkResetDescriptorPool;
        PFN_allocDescSets_ vkAllocateDescriptorSets;
        PFN_updateDescSets_ vkUpdateDescriptorSets;
        PFN_createSampler_ vkCreateSampler;
        void (*vkDestroySampler)(VkDevice_, VkSampler_, const void *);
        u32 (*vkCreateImageView)(VkDevice_, const VkImageViewCreateInfo_ *, const void *, VkImageView_ *);
        u32 (*vkCreateShaderModule)(VkDevice_, const VkShaderModuleCreateInfo_ *, const void *, VkShaderModule_ *);
        void (*vkDestroyShaderModule)(VkDevice_, VkShaderModule_, const void *);
        u32 (*vkCreatePipelineLayout)(VkDevice_, const VkPipelineLayoutCreateInfo_ *, const void *, VkPipelineLayout_ *);
        void (*vkDestroyPipelineLayout)(VkDevice_, VkPipelineLayout_, const void *);
        u32 (*vkCreateComputePipelines)(VkDevice_, VkPipeline_, u32, const VkComputePipelineCreateInfo_ *, const void *, VkPipeline_ *);
        void (*vkDestroyPipeline)(VkDevice_, VkPipeline_, const void *);
        void (*vkCmdBindPipeline)(VkCommandBuffer_, u32, VkPipeline_);
        void (*vkCmdBindDescriptorSets)(VkCommandBuffer_, u32, VkPipelineLayout_, u32, u32, const VkDescriptorSet_ *, u32, const u32 *);
        void (*vkCmdDispatch)(VkCommandBuffer_, u32, u32, u32);
        u32 (*vkCreateQueryPool)(VkDevice_, const VkQueryPoolCreateInfo_ *, const void *, VkQueryPool_ *);
        void (*vkDestroyQueryPool)(VkDevice_, VkQueryPool_, const void *);
        u32 (*vkGetQueryPoolResults)(VkDevice_, VkQueryPool_, u32, u32, u64, void *, u64, u32);
        void (*vkCmdResetQueryPool)(VkCommandBuffer_, VkQueryPool_, u32, u32);
        void (*vkCmdWriteTimestamp)(VkCommandBuffer_, u32, VkQueryPool_, u32);

        RPlatform *plat;
} RenderVK;

#define VKSYM(field, name) \
        *(void **)(&v->field) = (void *)v->gipa(v->dev ? (void *)v->dev : (void *)v->inst, name)

static u32 vkres_ok(u32 r)
{
        return r == VK_SUCCESS_ || (i32)r == VK_SUBOPTIMAL_KHR_;
}

/* forward decls (vk_end references the WSI window machinery) */
static void vkwin_destroy(RenderVK *v, VkWin_ *w);
static bool vkwin_create(RenderVK *v, int idx, char *err, size_t errcap);
static bool vk_init(RRenderer *rr, RPlatform *p, char *err, size_t errcap);

/* ========================================================================== *
 * Low-level GPU resource helpers
 * ========================================================================== */

static i32 vk_pick_memtype(RenderVK *v, u32 type_bits, u32 need, u32 want)
{
        VkPhysicalDeviceMemoryProperties_ mp;
        v->vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
        i32 want_idx = -1;
        for (u32 i = 0; i < mp.memoryTypeCount && i < VK_MAX_MEMORY_TYPES_; i++) {
                if (!(type_bits & (1u << i)))
                        continue;
                u32 props = mp.memoryTypes[i].propertyFlags;
                if ((props & need) != need)
                        continue;
                if (props & want) {
                        want_idx = (i32)i;
                        break;
                }
                if (want_idx < 0)
                        want_idx = (i32)i;
        }
        return want_idx;
}

/* CPU-read staging (frame download) memory: HOST_CACHED dominates the
 * cost model. An uncached host-visible heap — e.g. a discrete GPU's BAR
 * window, which vk_pick_memtype's DEVICE_LOCAL preference used to select —
 * serializes every CPU read over the bus: ~500ns per 4-byte load turned
 * the wl_shm premultiply loop into 1.1 s per 1080p frame on a GTX 1050 Ti
 * (field report #4: render=1112ms, gpu=2.1ms, one core pegged). Tiers:
 *   0. HOST_VISIBLE|HOST_CACHED|HOST_COHERENT  (sysmem: fast + simple)
 *   1. HOST_VISIBLE|HOST_CACHED                (needs invalidate after fence)
 *   2. HOST_VISIBLE|HOST_COHERENT, non-DEVICE_LOCAL first (uncached, but
 *      system RAM beats a BAR window; never prefer DEVICE_LOCAL here)
 *   3. HOST_VISIBLE (any)
 * Returns the type index and reports whether it is coherent. */
static i32 vk_pick_download_memtype(RenderVK *v, u32 type_bits, bool *coherent)
{
        static const u32 tiers[4] = {
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT_ |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT_,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_,
        };
        VkPhysicalDeviceMemoryProperties_ mp;
        v->vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
        for (int t = 0; t < 4; t++) {
                for (int pass = 0; pass < 2; pass++) {
                        for (u32 i = 0; i < mp.memoryTypeCount && i < VK_MAX_MEMORY_TYPES_; i++) {
                                if (!(type_bits & (1u << i)))
                                        continue;
                                u32 props = mp.memoryTypes[i].propertyFlags;
                                if ((props & tiers[t]) != tiers[t])
                                        continue;
                                /* tiers 2-3: avoid DEVICE_LOCAL host-visible
                                 * types (BAR windows) unless nothing else
                                 * matches */
                                if (pass == 0 && t >= 2 &&
                                    (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_))
                                        continue;
                                if (coherent)
                                        *coherent = (props &
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_) != 0;
                                return (i32)i;
                        }
                }
        }
        return -1;
}

static void vk_grave_add(RenderVK *v, VkImage_ img, VkImageView_ view,
                         VkBuffer_ buf, VkDeviceMemory_ mem)
{
        if (v->grave_n >= VK_GRAVE) {
                /* overflow (pathological churn): destroy immediately — the
                 * oldest entries are >= 3 frames stale by construction */
                int old = -1;
                for (int i = 0; i < v->grave_n; i++)
                        if (old < 0 || v->grave[i].frame < v->grave[old].frame)
                                old = i;
                if (old >= 0) {
                        VGrave *g = &v->grave[old];
                        if (g->view && v->vkDestroyImageView)
                                v->vkDestroyImageView(v->dev, g->view, NULL);
                        if (g->img && v->vkDestroyImage)
                                v->vkDestroyImage(v->dev, g->img, NULL);
                        if (g->buf && v->vkDestroyBuffer)
                                v->vkDestroyBuffer(v->dev, g->buf, NULL);
                        if (g->mem && v->vkFreeMemory)
                                v->vkFreeMemory(v->dev, g->mem, NULL);
                        *g = v->grave[--v->grave_n];
                }
        }
        VGrave *g = &v->grave[v->grave_n++];
        g->img = img;
        g->view = view;
        g->buf = buf;
        g->mem = mem;
        g->frame = v->frame_no;
}

static void vk_grave_sweep(RenderVK *v)
{
        int n = 0;
        for (int i = 0; i < v->grave_n; i++) {
                VGrave *g = &v->grave[i];
                if (v->frame_no < g->frame + VK_SLOTS + 1) {
                        if (i != n)
                                v->grave[n] = *g;
                        n++;
                        continue;
                }
                if (g->view && v->vkDestroyImageView)
                        v->vkDestroyImageView(v->dev, g->view, NULL);
                if (g->img && v->vkDestroyImage)
                        v->vkDestroyImage(v->dev, g->img, NULL);
                if (g->buf && v->vkDestroyBuffer)
                        v->vkDestroyBuffer(v->dev, g->buf, NULL);
                if (g->mem && v->vkFreeMemory)
                        v->vkFreeMemory(v->dev, g->mem, NULL);
        }
        v->grave_n = n;
}

static void vkimg_retire(RenderVK *v, VImg *im)
{
        if (!im->img)
                return;
        vk_grave_add(v, im->img, im->view, 0, im->mem);
        memset(im, 0, sizeof *im);
}

/* Create a 2D image (+ memory + view). The initial UNDEFINED->GENERAL
 * barrier is recorded into the CURRENT command buffer — images are only
 * created mid-recording (begin/layer), which is when the transition
 * belongs. `clear` additionally records a clear (sim fields start at 0
 * like the GL tier's cleared FBO pair). */
static bool vkimg_new(RenderVK *v, int win, VImg *im, u32 w, u32 h, u32 format,
                      u32 usage, bool clear)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        memset(im, 0, sizeof *im);

        VkImageCreateInfo_ ici;
        memset(&ici, 0, sizeof ici);
        ici.sType_ = ST_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D_;
        ici.format = format;
        ici.extent.width = w;
        ici.extent.height = h;
        ici.extent.depth = 1;
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT_;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL_;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE_;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED_;
        u32 r = v->vkCreateImage(v->dev, &ici, NULL, &im->img);
        if (r != VK_SUCCESS_) {
                im->img = 0;
                return false;
        }

        struct { u64 size, alignment; u32 memoryTypeBits; } reqs;
        memset(&reqs, 0, sizeof reqs);
        v->vkGetImageMemoryRequirements(v->dev, im->img, &reqs);
        i32 mt = vk_pick_memtype(v, reqs.memoryTypeBits,
                                 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_);
        if (mt < 0)
                mt = vk_pick_memtype(v, reqs.memoryTypeBits, 0, 0);
        if (mt < 0) {
                v->vkDestroyImage(v->dev, im->img, NULL);
                im->img = 0;
                return false;
        }
        VkMemoryAllocateInfo_ mai;
        memset(&mai, 0, sizeof mai);
        mai.sType_ = ST_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = (u32)mt;
        r = v->vkAllocateMemory(v->dev, &mai, NULL, &im->mem);
        if (r != VK_SUCCESS_) {
                v->vkDestroyImage(v->dev, im->img, NULL);
                im->img = 0;
                return false;
        }
        r = v->vkBindImageMemory(v->dev, im->img, im->mem, 0);
        if (r != VK_SUCCESS_) {
                v->vkFreeMemory(v->dev, im->mem, NULL);
                v->vkDestroyImage(v->dev, im->img, NULL);
                im->img = 0;
                im->mem = 0;
                return false;
        }

        VkImageViewCreateInfo_ vci;
        memset(&vci, 0, sizeof vci);
        vci.sType_ = ST_IMAGE_VIEW_CREATE_INFO;
        vci.image = im->img;
        vci.viewType = 1;                 /* VK_IMAGE_VIEW_TYPE_2D */
        vci.format = format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        r = v->vkCreateImageView(v->dev, &vci, NULL, &im->view);
        if (r != VK_SUCCESS_) {
                v->vkFreeMemory(v->dev, im->mem, NULL);
                v->vkDestroyImage(v->dev, im->img, NULL);
                im->img = 0;
                im->mem = 0;
                return false;
        }
        im->w = w;
        im->h = h;
        im->format = format;

        /* initial transition into GENERAL (recorded where it belongs) */
        VkImageMemoryBarrier_ b;
        memset(&b, 0, sizeof b);
        b.sType_ = ST_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT_ | VK_ACCESS_SHADER_WRITE_BIT_ |
                          VK_ACCESS_TRANSFER_WRITE_BIT_ | VK_ACCESS_TRANSFER_READ_BIT_;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED_;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL_;
        b.image = im->img;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        v->vkCmdPipelineBarrier(fr->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT_,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT_ |
                                VK_PIPELINE_STAGE_TRANSFER_BIT_,
                                0, 0, NULL, 0, NULL, 1, &b);
        if (clear) {
                VkClearColorValue_ cc;
                memset(&cc, 0, sizeof cc);
                struct { u32 aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; } sr;
                memset(&sr, 0, sizeof sr);
                sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
                sr.levelCount = 1;
                sr.layerCount = 1;
                v->vkCmdClearColorImage(fr->cmd, im->img, VK_IMAGE_LAYOUT_GENERAL_,
                                        &cc, 1, &sr);
        }
        return true;
}

/* GENERAL->GENERAL access barrier. srcStage covers compute+transfer
 * (whatever produced the previous state); dstStage must match the
 * dstAccess bits (COMPUTE for shader access, TRANSFER for copies). */
static void vk_img_barrier(RenderVK *v, VkCommandBuffer_ cmd, VkImage_ img,
                           u32 srcAccess, u32 dstAccess, u32 dstStage)
{
        VkImageMemoryBarrier_ b;
        memset(&b, 0, sizeof b);
        b.sType_ = ST_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL_;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL_;
        b.image = img;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        v->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT_ |
                                       VK_PIPELINE_STAGE_TRANSFER_BIT_,
                                dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ========================================================================== *
 * Frame-slot machinery (UBO slice, upload arena, command recording)
 * ========================================================================== */

static bool vkbuf_new(RenderVK *v, VkBuffer_ *buf, VkDeviceMemory_ *mem,
                      u8 **map, u64 size, u32 usage, bool cpu_read,
                      bool *coherent_out)
{
        VkBufferCreateInfo_ bci;
        memset(&bci, 0, sizeof bci);
        bci.sType_ = ST_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE_;
        u32 r = v->vkCreateBuffer(v->dev, &bci, NULL, buf);
        if (r != VK_SUCCESS_)
                return false;
        struct { u64 size, alignment; u32 memoryTypeBits; } reqs;
        memset(&reqs, 0, sizeof reqs);
        v->vkGetBufferMemoryRequirements(v->dev, *buf, &reqs);
        i32 mt;
        if (cpu_read) {
                bool coh = true;
                mt = vk_pick_download_memtype(v, reqs.memoryTypeBits, &coh);
                if (coherent_out)
                        *coherent_out = coh;
        } else {
                mt = vk_pick_memtype(v, reqs.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_);
                if (mt < 0)
                        mt = vk_pick_memtype(v, reqs.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_, 0);
        }
        if (mt < 0) {
                v->vkDestroyBuffer(v->dev, *buf, NULL);
                *buf = 0;
                return false;
        }
        VkMemoryAllocateInfo_ mai;
        memset(&mai, 0, sizeof mai);
        mai.sType_ = ST_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = (u32)mt;
        r = v->vkAllocateMemory(v->dev, &mai, NULL, mem);
        if (r != VK_SUCCESS_) {
                v->vkDestroyBuffer(v->dev, *buf, NULL);
                *buf = 0;
                return false;
        }
        if (v->vkBindBufferMemory(v->dev, *buf, *mem, 0) != VK_SUCCESS_ ||
            v->vkMapMemory(v->dev, *mem, 0, VK_WHOLE_SIZE_, 0, (void **)map) != VK_SUCCESS_ ||
            !*map) {
                v->vkFreeMemory(v->dev, *mem, NULL);
                v->vkDestroyBuffer(v->dev, *buf, NULL);
                *buf = 0;
                *mem = 0;
                return false;
        }
        return true;
}

static bool vk_frame_create(RenderVK *v, int win)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        if (fr->created)
                return true;

        VkCommandPoolCreateInfo_ pci;
        memset(&pci, 0, sizeof pci);
        pci.sType_ = 39;    /* VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO */
        pci.queueFamilyIndex = v->queue_family;
        if (v->vkCreateCommandPool(v->dev, &pci, NULL, &fr->cmdpool) != VK_SUCCESS_)
                return false;

        VkCommandBufferAllocateInfo_ cai;
        memset(&cai, 0, sizeof cai);
        cai.sType_ = ST_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = fr->cmdpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY_;
        cai.commandBufferCount = 1;
        if (v->vkAllocateCommandBuffers(v->dev, &cai, &fr->cmd) != VK_SUCCESS_)
                return false;

        /* descriptor pool: per-dispatch sets, reset with the slot */
        VkDescriptorPoolSize_ sizes[3];
        memset(sizes, 0, sizeof sizes);
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER_;
        sizes[0].descriptorCount = VK_MAX_DISPATCHES * 4;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE_;
        sizes[1].descriptorCount = VK_MAX_DISPATCHES * 4;
        sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC_;
        sizes[2].descriptorCount = VK_MAX_DISPATCHES;
        VkDescriptorPoolCreateInfo_ dpi;
        memset(&dpi, 0, sizeof dpi);
        dpi.sType_ = ST_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = VK_MAX_DISPATCHES;
        dpi.poolSizeCount = 3;
        dpi.pPoolSizes = sizes;
        if (v->vkCreateDescriptorPool(v->dev, &dpi, NULL, &fr->dpool) != VK_SUCCESS_)
                return false;

        if (!vkbuf_new(v, &fr->ubo, &fr->ubo_mem, &fr->ubo_map, VK_UBO_CAP,
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT_, false, NULL))
                return false;

        if (!vkbuf_new(v, &fr->arena, &fr->arena_mem, &fr->arena_map,
                       8u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT_, false, NULL))
                return false;
        fr->arena_cap = 8u << 20;

        /* UNSIGNALED: this fence is submitted exactly once per use and
         * waited at slot reuse (or immediately on the platform path) */
        VkFenceCreateInfo_ fci = { ST_FENCE_CREATE_INFO, NULL, 0 };
        if (v->vkCreateFence(v->dev, &fci, NULL, &fr->fence) != VK_SUCCESS_)
                return false;
        fr->created = true;
        return true;
}

/* grow the upload arena: the current buffer is retired (its copies may be
 * referenced by commands recorded earlier THIS frame; the graveyard keeps
 * it alive until those frames retired) */
static bool vk_arena_grow(RenderVK *v, int win, u64 need)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        u64 cap = fr->arena_cap ? fr->arena_cap : (8u << 20);
        while (cap < need)
                cap *= 2;
        if (cap > (256u << 20))
                return false;
        VkBuffer_ nb = 0;
        VkDeviceMemory_ nm = 0;
        u8 *nmap = NULL;
        if (!vkbuf_new(v, &nb, &nm, &nmap, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT_,
                       false, NULL))
                return false;
        if (fr->arena) {
                vk_grave_add(v, 0, 0, fr->arena, fr->arena_mem);
                fr->arena = 0;
                fr->arena_mem = 0;
                fr->arena_map = NULL;
        }
        fr->arena = nb;
        fr->arena_mem = nm;
        fr->arena_map = nmap;
        fr->arena_cap = cap;
        return true;
}

/* reserve `bytes` in the upload arena (4-byte aligned) */
static u8 *vk_arena_alloc(RenderVK *v, int win, u64 bytes, u64 *off_out)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        u64 off = (fr->arena_used + 3) & ~(u64)3;
        if (off + bytes > fr->arena_cap) {
                if (!vk_arena_grow(v, win, off + bytes))
                        return NULL;
                off = (fr->arena_used + 3) & ~(u64)3;
        }
        fr->arena_used = off + bytes;
        *off_out = off;
        return fr->arena_map + off;
}

/* reserve one 512-byte UBO slot; returns the byte offset (dynamic) */
static u32 vk_ubo_alloc(RenderVK *v, int win, const f32 *par, u32 nvec4)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        if (fr->ubo_used + VK_UBO_SLOT > VK_UBO_CAP)
                return 0xFFFFFFFFu;
        u32 off = fr->ubo_used;
        fr->ubo_used += VK_UBO_SLOT;
        if (nvec4 > VK_UBO_SLOT / 16)
                nvec4 = VK_UBO_SLOT / 16;
        if (nvec4)
                memcpy(fr->ubo_map + off, par, (size_t)nvec4 * 16);
        return off;
}

/* ========================================================================== *
 * Dispatch: one compute kernel invocation with its descriptor set
 * ========================================================================== */

typedef struct {
        int kernel;
        const VImg *A;  bool A_lin;   /* binding 1: combined sampler slot  */
        const VImg *B;  bool B_lin;   /* binding 2                          */
        const VImg *C;  bool C_lin;   /* binding 3                          */
        const VImg *D;  bool D_lin;   /* binding 4                          */
        const VImg *F16a;             /* binding 8: rgba16f read            */
        VImg *F16w;                   /* binding 9: rgba16f write           */
        const VImg *F16b;             /* binding 10: rgba16f read 2         */
        VImg *out8;                   /* binding 7: rgba8 write             */
        const f32 *par;               /* vec4 params                        */
        u32 npar;                     /* vec4 count                         */
        u32 gw, gh;                   /* dispatch extent (0 = out8/F16w)    */
        u32 tile;                     /* px per workgroup axis (0 = 8)      */
} VDisp;

static bool vk_run(RenderVK *v, int win, const VDisp *d)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        if (!fr->recording)
                return false;
        if (v->dispatches >= VK_MAX_DISPATCHES) {
                if (!v->dispatch_warned) {
                        v->dispatch_warned = true;
                        LOGW("vulkan: dispatch budget exhausted this frame "
                             "(>=%u kernels) — layer dropped",
                             (unsigned)VK_MAX_DISPATCHES);
                }
                return false;
        }

        /* 1. uniform block */
        u32 ubo_off = vk_ubo_alloc(v, win, d->par ? d->par : NULL, d->npar);
        if (ubo_off == 0xFFFFFFFFu)
                return false;

        /* 2. descriptor set */
        VkDescriptorSetAllocateInfo_ dsai;
        memset(&dsai, 0, sizeof dsai);
        dsai.sType_ = ST_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = fr->dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &v->dsl;
        VkDescriptorSet_ set = 0;
        if (v->vkAllocateDescriptorSets(v->dev, &dsai, &set) != VK_SUCCESS_)
                return false;

        VkDescriptorBufferInfo_ bi;
        memset(&bi, 0, sizeof bi);
        bi.buffer = fr->ubo;
        bi.offset = 0;
        bi.range = VK_UBO_SLOT;

        VkDescriptorImageInfo_ ii[7];
        VkWriteDescriptorSet_ wr[8];
        u32 nwr = 0, nii = 0;
        memset(ii, 0, sizeof ii);
        memset(wr, 0, sizeof wr);

        {
                VkWriteDescriptorSet_ *w = &wr[nwr++];
                w->sType_ = ST_WRITE_DESCRIPTOR_SET;
                w->dstSet = set;
                w->dstBinding = 0;
                w->descriptorCount = 1;
                w->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC_;
                w->pBufferInfo = &bi;
        }
        const VImg *slots[4] = { d->A, d->B, d->C, d->D };
        bool lins[4] = { d->A_lin, d->B_lin, d->C_lin, d->D_lin };
        for (u32 s = 0; s < 4; s++) {
                if (!slots[s])
                        continue;
                VkDescriptorImageInfo_ *info = &ii[nii++];
                info->sampler = lins[s] ? v->samp_linear : v->samp_nearest;
                info->imageView = slots[s]->view;
                info->imageLayout = VK_IMAGE_LAYOUT_GENERAL_;
                VkWriteDescriptorSet_ *w = &wr[nwr++];
                w->sType_ = ST_WRITE_DESCRIPTOR_SET;
                w->dstSet = set;
                w->dstBinding = 1 + s;
                w->descriptorCount = 1;
                w->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER_;
                w->pImageInfo = info;
        }
        const VImg *stow = d->F16w;
        /* bindings: 7 = out8, 8 = F16a read, 9 = F16w write, 10 = F16b */
        {
                const VImg *imgs[4] = { d->out8, d->F16a, stow, d->F16b };
                u32 binds[4] = { 7, 8, 9, 10 };
                for (u32 s = 0; s < 4; s++) {
                        if (!imgs[s])
                                continue;
                        VkDescriptorImageInfo_ *info = &ii[nii++];
                        info->imageView = imgs[s]->view;
                        info->imageLayout = VK_IMAGE_LAYOUT_GENERAL_;
                        VkWriteDescriptorSet_ *w = &wr[nwr++];
                        w->sType_ = ST_WRITE_DESCRIPTOR_SET;
                        w->dstSet = set;
                        w->dstBinding = binds[s];
                        w->descriptorCount = 1;
                        w->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE_;
                        w->pImageInfo = info;
                }
        }
        v->vkUpdateDescriptorSets(v->dev, nwr, wr, 0, NULL);

        /* 3. barriers: every image this dispatch touches (covers RAW/WAR/WAW
         *    against earlier dispatches and uploads) */
        const VImg *touched[8];
        u32 nt = 0;
        if (d->A) touched[nt++] = d->A;
        if (d->B) touched[nt++] = d->B;
        if (d->C) touched[nt++] = d->C;
        if (d->D) touched[nt++] = d->D;
        if (d->out8) touched[nt++] = d->out8;
        if (d->F16a) touched[nt++] = d->F16a;
        if (d->F16w) touched[nt++] = d->F16w;
        if (d->F16b) touched[nt++] = d->F16b;
        for (u32 i = 0; i < nt; i++)
                vk_img_barrier(v, fr->cmd, touched[i]->img,
                               VK_ACCESS_SHADER_READ_BIT_ |
                               VK_ACCESS_SHADER_WRITE_BIT_ |
                               VK_ACCESS_TRANSFER_WRITE_BIT_ |
                               VK_ACCESS_TRANSFER_READ_BIT_,
                               VK_ACCESS_SHADER_READ_BIT_ |
                               VK_ACCESS_SHADER_WRITE_BIT_,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT_);

        /* 4. bind + dispatch */
        v->vkCmdBindPipeline(fr->cmd, VK_PIPELINE_BIND_POINT_COMPUTE_,
                             v->pipe[d->kernel]);
        u32 dyn = ubo_off;
        v->vkCmdBindDescriptorSets(fr->cmd, VK_PIPELINE_BIND_POINT_COMPUTE_,
                                   v->pl, 0, 1, &set, 1, &dyn);
        u32 gw = d->gw, gh = d->gh;
        if (!gw || !gh) {
                const VImg *ref = d->out8 ? d->out8 : d->F16w;
                if (!ref)
                        return false;
                gw = ref->w;
                gh = ref->h;
        }
        u32 tile = d->tile ? d->tile : 8;
        v->vkCmdDispatch(fr->cmd, (gw + tile - 1) / tile, (gh + tile - 1) / tile, 1);
        v->dispatches++;
        return true;
}

/* full-region upload: arena -> image (barrier/copy/barrier recorded) */
static bool vk_upload_image(RenderVK *v, int win, VImg *im, const void *px,
                            u32 w, u32 h, u32 bpp)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        if (!im->img || !px || !w || !h)
                return false;
        u64 bytes = (u64)w * h * bpp;
        u64 off = 0;
        u8 *dst = vk_arena_alloc(v, win, bytes, &off);
        if (!dst)
                return false;
        f64 t0 = rev_monotonic();
        memcpy(dst, px, (size_t)bytes);
        v->perf_upload_ms += (rev_monotonic() - t0) * 1000.0;

        vk_img_barrier(v, fr->cmd, im->img,
                       VK_ACCESS_SHADER_READ_BIT_ | VK_ACCESS_SHADER_WRITE_BIT_ |
                       VK_ACCESS_TRANSFER_WRITE_BIT_ | VK_ACCESS_TRANSFER_READ_BIT_,
                       VK_ACCESS_TRANSFER_WRITE_BIT_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT_);
        VkBufferImageCopy_ cp;
        memset(&cp, 0, sizeof cp);
        cp.bufferOffset = off;
        cp.bufferRowLength = 0;
        cp.bufferImageHeight = 0;
        cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        cp.imageSubresource.layerCount = 1;
        cp.imageExtent.width = w;
        cp.imageExtent.height = h;
        cp.imageExtent.depth = 1;
        v->vkCmdCopyBufferToImage(fr->cmd, fr->arena, im->img,
                                  VK_IMAGE_LAYOUT_GENERAL_, 1, &cp);
        vk_img_barrier(v, fr->cmd, im->img, VK_ACCESS_TRANSFER_WRITE_BIT_,
                       VK_ACCESS_SHADER_READ_BIT_,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT_);
        return true;
}

/* ========================================================================== *
 * Layer sources: content-addressed upload cache, gradient cache, particles
 * ========================================================================== */

#define VK_RGBA8_USAGE (VK_IMAGE_USAGE_SAMPLED_BIT_ | VK_IMAGE_USAGE_STORAGE_BIT_ | \
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT_ | VK_IMAGE_USAGE_TRANSFER_DST_BIT_)
#define VK_F16_USAGE   (VK_IMAGE_USAGE_SAMPLED_BIT_ | VK_IMAGE_USAGE_STORAGE_BIT_ | \
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT_)
#define VK_R32F_USAGE  (VK_IMAGE_USAGE_SAMPLED_BIT_ | VK_IMAGE_USAGE_TRANSFER_DST_BIT_)
#define VK_R8_USAGE    (VK_IMAGE_USAGE_SAMPLED_BIT_ | VK_IMAGE_USAGE_TRANSFER_DST_BIT_)

static const VImg *upload_layer_tex(RenderVK *v, int win, const RLayerCmd *cmd)
{
        v->cache_clock++;
        VTexSlot *cache = v->texcache;

        if (cmd->content_id) {
                for (int i = 0; i < VK_TEX_CACHE; i++) {
                        if (cache[i].id == cmd->content_id && cache[i].im.img &&
                            cache[i].im.w == cmd->w && cache[i].im.h == cmd->h) {
                                cache[i].stamp = v->cache_clock;
                                return &cache[i].im;
                        }
                }
        }

        /* miss: evict the least-recently-used slot */
        VTexSlot *slot = &cache[0];
        for (int i = 1; i < VK_TEX_CACHE; i++)
                if (cache[i].stamp < slot->stamp)
                        slot = &cache[i];
        if (slot->im.img && (slot->im.w != cmd->w || slot->im.h != cmd->h))
                vkimg_retire(v, &slot->im);
        if (!slot->im.img) {
                if (!vkimg_new(v, win, &slot->im, cmd->w, cmd->h,
                               VK_FORMAT_R8G8B8A8_UNORM_, VK_RGBA8_USAGE, false))
                        return NULL;
        }
        if (!vk_upload_image(v, win, &slot->im, cmd->px, cmd->w, cmd->h, 4))
                return NULL;
        slot->id = cmd->content_id;    /* 0 = never matches a cached lookup */
        slot->stamp = v->cache_clock;
        return &slot->im;
}

/* Renders (or reuses) the gradient image; keyed by content serial — the
 * same capping math as gl_grad_layer()/build_lut(). */
static const VImg *vk_grad_layer(RenderVK *v, int win, const RLayerCmd *cmd)
{
        const RGradDesc *gd = cmd->grad;

        VGradSlot *slot = NULL;
        for (int i = 0; i < VK_GRAD_CACHE; i++) {
                if (cmd->content_id && v->gcache[i].id == cmd->content_id &&
                    v->gcache[i].im.img) {
                        slot = &v->gcache[i];
                        slot->stamp = ++v->g_clock;
                        return &slot->im;
                }
        }
        int pick = 0;
        for (int i = 1; i < VK_GRAD_CACHE; i++)
                if (v->gcache[i].stamp < v->gcache[pick].stamp)
                        pick = i;
        slot = &v->gcache[pick];
        if (slot->im.img && (slot->im.w != cmd->w || slot->im.h != cmd->h))
                vkimg_retire(v, &slot->im);
        if (!slot->im.img) {
                if (!vkimg_new(v, win, &slot->im, cmd->w, cmd->h,
                               VK_FORMAT_R8G8B8A8_UNORM_, VK_RGBA8_USAGE, false))
                        return NULL;
        }
        slot->id = cmd->content_id;
        slot->stamp = ++v->g_clock;

        /* evaluate: capped stops like build_lut() (GL parity) */
        RColor cst[17];
        f64 coff[17];
        int n = gd->nstops;
        for (int i = 0; i < n; i++) {
                cst[i] = gd->stops[i];
                coff[i] = gd->offs[i];
        }
        if (n > 0 && coff[0] > 0) {
                for (int i = n; i > 0; i--) {
                        cst[i] = cst[i - 1];
                        coff[i] = coff[i - 1];
                }
                coff[0] = 0.0;
                n++;
        }
        if (n > 0 && coff[n - 1] < 1) {
                cst[n] = cst[n - 1];
                coff[n] = 1.0;
                n++;
        }
        if (n < 2) {
                cst[0] = cst[1] = gd->stops[0];
                coff[0] = 0.0;
                coff[1] = 1.0;
                n = 2;
        }

        f32 par[26 * 4];
        memset(par, 0, sizeof par);
        par[0] = (f32)slot->im.w;
        par[1] = (f32)slot->im.h;
        par[2] = (f32)gd->shape;
        par[3] = (f32)n;
        par[4] = (f32)gd->p0x;
        par[5] = (f32)gd->p0y;
        par[8] = (f32)gd->p1x;
        par[9] = (f32)gd->p1y;
        par[12] = (f32)gd->cx;
        par[13] = (f32)gd->cy;
        par[15] = (f32)gd->radius;
        for (int i = 0; i < 17; i++) {
                int j = i < n ? i : n - 1;
                par[16 + i * 4 + 0] = (f32)cst[j].r / 255.0f;
                par[16 + i * 4 + 1] = (f32)cst[j].g / 255.0f;
                par[16 + i * 4 + 2] = (f32)cst[j].b / 255.0f;
                par[16 + i * 4 + 3] = (f32)cst[j].a / 255.0f;
                par[84 + i] = (f32)(i < n ? coff[i] : coff[n - 1]);
        }
        VDisp d;
        memset(&d, 0, sizeof d);
        d.kernel = K_GRADIENT;
        d.out8 = &slot->im;
        d.par = par;
        d.npar = 26;
        if (!vk_run(v, win, &d))
                return NULL;
        return &slot->im;
}

/* Uploads the packed particle bands and splats them into a frame-size
 * layer image; returns it for the standard composite pass. */
static const VImg *vk_particles_layer(RenderVK *v, int win, const RLayerCmd *cmd)
{
        const RSimParticles *pd = cmd->psim;
        u32 rows = (pd->slots + 1023) / 1024;
        u32 tex_h = rows * 2;

        if (v->ptex.img && v->ptex.h != tex_h)
                vkimg_retire(v, &v->ptex);
        if (!v->ptex.img &&
            !vkimg_new(v, win, &v->ptex, 1024, tex_h,
                       VK_FORMAT_R32G32B32A32_SFLOAT_, VK_R32F_USAGE, false))
                return NULL;
        if (pd->n) {
                if (!vk_upload_image(v, win, &v->ptex, pd->band0, 1024, tex_h, 16))
                        return NULL;
        }

        if (v->ptarget.img && (v->ptarget.w != cmd->w || v->ptarget.h != cmd->h))
                vkimg_retire(v, &v->ptarget);
        if (!v->ptarget.img &&
            !vkimg_new(v, win, &v->ptarget, cmd->w, cmd->h,
                       VK_FORMAT_R8G8B8A8_UNORM_, VK_RGBA8_USAGE, false))
                return NULL;

        if (pd->n) {
                f32 par[4];
                par[0] = (f32)cmd->w;
                par[1] = (f32)cmd->h;
                par[2] = (f32)pd->n;
                par[3] = (f32)rows;
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_PARTICLES;
                d.A = &v->ptex;
                d.A_lin = false;
                d.out8 = &v->ptarget;
                d.par = par;
                d.npar = 1;
                d.tile = 64;    /* one 64x64 tile per 8x8-thread workgroup */
                if (!vk_run(v, win, &d))
                        return NULL;
        }
        return &v->ptarget;
}

/* ========================================================================== *
 * GPU fluid solver (mirrors gl_fluid_step / fluid.c exactly)
 * ========================================================================== */

static void vk_sim_free(RenderVK *v, VKSim *s)
{
        vkimg_retire(v, &s->vel[0]);
        vkimg_retire(v, &s->vel[1]);
        vkimg_retire(v, &s->dye[0]);
        vkimg_retire(v, &s->dye[1]);
        vkimg_retire(v, &s->press[0]);
        vkimg_retire(v, &s->press[1]);
        vkimg_retire(v, &s->curl);
        vkimg_retire(v, &s->out8);
        vkimg_retire(v, &s->field[0]);
        vkimg_retire(v, &s->field[1]);
        s->key = NULL;
        s->ok = false;
}

static VKSim *vk_sim_slot(RenderVK *v, int win, const void *key, bool is_ripple,
                          u32 gw, u32 gh, u32 dw, u32 dh)
{
        VKSim *tab = v->sims[win];
        for (int i = 0; i < VK_MAX_SIMS; i++) {
                if (tab[i].key == key && tab[i].ok && tab[i].is_ripple == is_ripple) {
                        tab[i].last_seen = v->frame_no;
                        return &tab[i];
                }
        }
        /* pick a free or expired slot */
        VKSim *best = NULL;
        for (int i = 0; i < VK_MAX_SIMS; i++) {
                if (!tab[i].key) {
                        best = &tab[i];
                        break;
                }
                if (v->frame_no - tab[i].last_seen > 300 &&
                    (!best || tab[i].last_seen < best->last_seen))
                        best = &tab[i];
        }
        if (!best) {
                static bool warned;
                if (!warned) {
                        warned = true;
                        LOGW("vulkan: too many sim layers, dropping sims");
                }
                return NULL;
        }
        vk_sim_free(v, best);
        best->key = key;
        best->is_ripple = is_ripple;
        best->last_seen = v->frame_no;
        best->ok = false;

        if (is_ripple) {
                best->rgw = gw;
                best->rgh = gh;
                best->cur = 0;
                if (gw < 8 || gh < 8)
                        return NULL;
                if (!vkimg_new(v, win, &best->field[0], gw, gh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->field[1], gw, gh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true))
                        return NULL;
        } else {
                best->gw = gw;
                best->gh = gh;
                best->dw = dw;
                best->dh = dh;
                best->vc = best->dc = best->pc = 0;
                best->amb_t = 0;
                best->wisp_acc = 0;
                if (gw < 8 || gh < 8)
                        return NULL;
                if (!vkimg_new(v, win, &best->vel[0], gw, gh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->vel[1], gw, gh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->press[0], gw, gh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->press[1], gw, gh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->curl, gw, gh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->dye[0], dw, dh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->dye[1], dw, dh,
                               VK_FORMAT_R16G16B16A16_SFLOAT_, VK_F16_USAGE, true) ||
                    !vkimg_new(v, win, &best->out8, dw, dh,
                               VK_FORMAT_R8G8B8A8_UNORM_, VK_RGBA8_USAGE, true))
                        return NULL;
        }
        best->ok = true;
        return best;
}

static void vk_fluid_project(RenderVK *v, int win, VKSim *f)
{
        f32 par[4];
        memset(par, 0, sizeof par);
        par[0] = (f32)f->gw;
        par[1] = (f32)f->gh;

        /* divergence pass: vel[vc] -> press[pc] (writes (0, div)) */
        VDisp d;
        memset(&d, 0, sizeof d);
        d.kernel = K_F_DIV;
        d.F16a = &f->vel[f->vc];
        d.F16w = &f->press[f->pc];
        d.par = par;
        d.npar = 1;
        if (!vk_run(v, win, &d))
                return;

        /* Jacobi x32 (the CPU's Gauss-Seidel is sequential; Jacobi is the
         * parallel equivalent — same iteration count as the GL tier) */
        d.kernel = K_F_JACOBI;
        for (int i = 0; i < 32; i++) {
                d.F16a = &f->press[f->pc];
                d.F16w = &f->press[f->pc ^ 1];
                if (!vk_run(v, win, &d))
                        return;
                f->pc ^= 1;
        }

        /* gradient subtraction: vel[vc] + press[pc] -> vel[vc^1] */
        d.kernel = K_F_GRADSUB;
        d.F16a = &f->vel[f->vc];
        d.F16b = &f->press[f->pc];
        d.F16w = &f->vel[f->vc ^ 1];
        if (vk_run(v, win, &d))
                f->vc ^= 1;
}

/* Advance one fluid step; returns the tonemapped straight-alpha RGBA8 dye
 * image (dw x dh) ready for the composite pass. */
static const VImg *vk_fluid_step(RenderVK *v, int win, VKSim *f, const RSimFluid *sd)
{
        f64 dt = sd->dt;
        if (dt <= 0)
                dt = 1.0 / 60.0;
        if (dt > 0.1)
                dt = 0.1;
        const double force = sd->force;
        const double radius = sd->radius < 0.002 ? 0.002 :
                              sd->radius > 0.2 ? 0.2 : sd->radius;
        const double dissip = sd->dissip < 0.0 ? 0.0 :
                              sd->dissip > 1.0 ? 1.0 : sd->dissip;
        const double ambient = sd->ambient < 0.0 ? 0.0 :
                               sd->ambient > 1.0 ? 1.0 : sd->ambient;
        const double rad_g = radius * f->gw * 0.5 + 1.5;   /* grid texels */
        const double rad_d = rad_g * 2.0;                  /* dye texels  */

        /* C-side event bookkeeping (mirrors fluid.c / gl_fluid_step) */
        f64 amb_t = f->amb_t;
        if (ambient > 0)
                amb_t += dt;

        bool wisp_fire = false;
        f64 wisp_x = 0, wisp_y = 0, wisp_vx = 0, wisp_vy = 0;
        RColor wisp_c = sd->dye_a;
        if (ambient > 0) {
                f->wisp_acc += dt;
                if (f->wisp_acc > 0.9) {
                        f->wisp_acc = 0;
                        wisp_fire = true;
                        wisp_x = 0.15 + 0.7 * fabs(sin(amb_t * 1.7));
                        wisp_y = 0.15 + 0.7 * fabs(cos(amb_t * 1.1));
                        wisp_vx = sin(amb_t * 2.0) * 0.6;
                        wisp_vy = cos(amb_t * 1.6) * 0.6;
                        wisp_c = (((int)(amb_t * 3) & 1) != 0) ? sd->dye_a : sd->dye_b;
                }
        }

        bool ptr_fire = sd->pressed;
        f64 ptr_x = sd->px, ptr_y = sd->py;
        f64 ptr_vx = 0, ptr_vy = 0;
        RColor ptr_c = sd->dye_a;
        if (ptr_fire) {
                ptr_vx = sd->pdx * force;
                ptr_vy = sd->pdy * force;
                double speed = hypot(sd->pdx, sd->pdy);
                if (sd->pdx >= 0)
                        ptr_c = sd->dye_a;
                else
                        ptr_c = sd->dye_b;
                if (speed < 1e-4)
                        ptr_c = ((ptr_x * 7.0 + ptr_y * 13.0) > 10.0) ? sd->dye_a : sd->dye_b;
        }

        /* pass 1: velocity forces (splat + ambient) */
        {
                f32 par[5 * 4];
                memset(par, 0, sizeof par);
                par[0] = (f32)f->gw;
                par[1] = (f32)f->gh;
                par[2] = (f32)dt;
                par[3] = (f32)amb_t;
                par[4] = (f32)ptr_x;
                par[5] = (f32)ptr_y;
                par[7] = (f32)(ptr_fire ? rad_g : 1.0);
                par[8] = (f32)(ptr_fire ? ptr_vx : 0.0);
                par[9] = (f32)(ptr_fire ? ptr_vy : 0.0);
                par[11] = (f32)ambient;
                par[12] = (f32)wisp_x;
                par[13] = (f32)wisp_y;
                par[15] = (f32)(wisp_fire ? rad_g : 1.0);
                par[16] = (f32)(wisp_fire ? wisp_vx : 0.0);
                par[17] = (f32)(wisp_fire ? wisp_vy : 0.0);
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_F_VELF;
                d.F16a = &f->vel[f->vc];
                d.F16w = &f->vel[f->vc ^ 1];
                d.par = par;
                d.npar = 5;
                if (!vk_run(v, win, &d))
                        return NULL;
                f->vc ^= 1;
        }

        /* pass 2: dye forces (splat colors) */
        if (ptr_fire || wisp_fire) {
                f32 par[5 * 4];
                memset(par, 0, sizeof par);
                par[0] = (f32)f->dw;
                par[1] = (f32)f->dh;
                par[4] = (f32)ptr_x;
                par[5] = (f32)ptr_y;
                par[7] = (f32)(ptr_fire ? rad_d : 1.0);
                par[8] = (f32)wisp_x;
                par[9] = (f32)wisp_y;
                par[11] = (f32)(wisp_fire ? rad_d : 1.0);
                par[12] = (f32)ptr_c.r / 255.0f;
                par[13] = (f32)ptr_c.g / 255.0f;
                par[14] = (f32)ptr_c.b / 255.0f;
                par[16] = (f32)wisp_c.r / 255.0f;
                par[17] = (f32)wisp_c.g / 255.0f;
                par[18] = (f32)wisp_c.b / 255.0f;
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_F_DYEF;
                d.F16a = &f->dye[f->dc];
                d.F16w = &f->dye[f->dc ^ 1];
                d.par = par;
                d.npar = 5;
                if (!vk_run(v, win, &d))
                        return NULL;
                f->dc ^= 1;
        }

        /* vorticity confinement */
        {
                f32 par[4];
                memset(par, 0, sizeof par);
                par[0] = (f32)f->gw;
                par[1] = (f32)f->gh;
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_F_CURL;
                d.F16a = &f->vel[f->vc];
                d.F16w = &f->curl;
                d.par = par;
                d.npar = 1;
                if (!vk_run(v, win, &d))
                        return NULL;

                par[2] = (f32)(6.0 * dt);
                memset(&d, 0, sizeof d);
                d.kernel = K_F_VORT;
                d.F16a = &f->vel[f->vc];
                d.F16b = &f->curl;
                d.F16w = &f->vel[f->vc ^ 1];
                d.par = par;
                d.npar = 1;
                if (!vk_run(v, win, &d))
                        return NULL;
                f->vc ^= 1;
        }

        vk_fluid_project(v, win, f);

        /* advect velocity (+ damping folded in) */
        {
                f32 par[4];
                memset(par, 0, sizeof par);
                par[0] = (f32)f->gw;
                par[1] = (f32)f->gh;
                par[2] = (f32)dt;
                par[3] = (f32)pow(0.999, dt * 60.0);
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_F_ADV_VEL;
                d.A = &f->vel[f->vc];
                d.A_lin = true;
                d.F16w = &f->vel[f->vc ^ 1];
                d.par = par;
                d.npar = 1;
                if (!vk_run(v, win, &d))
                        return NULL;
                f->vc ^= 1;
        }

        vk_fluid_project(v, win, f);

        /* advect dye (+ dissipation folded in) */
        {
                f32 par[4];
                memset(par, 0, sizeof par);
                par[0] = (f32)f->gw;
                par[1] = (f32)f->gh;
                par[2] = (f32)dt;
                double fade = 1.0 - dissip * 0.35;
                if (fade < 0)
                        fade = 0;
                par[3] = (f32)pow(fade, dt * 60.0);
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_F_ADV_DYE;
                d.A = &f->dye[f->dc];
                d.A_lin = true;
                d.B = &f->vel[f->vc];
                d.B_lin = true;
                d.F16w = &f->dye[f->dc ^ 1];
                d.par = par;
                d.npar = 1;
                if (!vk_run(v, win, &d))
                        return NULL;
                f->dc ^= 1;
        }

        f->amb_t = amb_t;

        /* tonemap: dye (float) -> layer image (straight RGBA8) */
        {
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_F_TONEMAP;
                d.F16a = &f->dye[f->dc];
                d.out8 = &f->out8;
                if (!vk_run(v, win, &d))
                        return NULL;
        }
        return &f->out8;
}

/* ========================================================================== *
 * GPU ripple solver (mirrors gl_ripple_step / ripple.c exactly)
 * ========================================================================== */

static const VImg *vk_ripple_step(RenderVK *v, int win, VKSim *rp, const RSimRipple *sd)
{
        f64 dt = sd->dt;
        if (dt <= 0)
                dt = 1.0 / 60.0;
        if (dt > 0.1)
                dt = 0.1;
        const double strength = sd->strength < 0.1 ? 0.1 :
                                sd->strength > 8.0 ? 8.0 : sd->strength;
        const double damping = sd->damping < 0.001 ? 0.001 :
                                sd->damping > 0.9 ? 0.9 : sd->damping;
        const double radius = sd->radius < 0.002 ? 0.002 :
                              sd->radius > 0.1 ? 0.1 : sd->radius;
        int steps = (int)(dt * 60.0 < 0.5 ? 0.5 :
                          dt * 60.0 > 2.0 ? 2.0 : dt * 60.0);

        if (sd->pressed || sd->nclicks > 0) {
                f32 par[7 * 4];
                memset(par, 0, sizeof par);
                par[0] = (f32)rp->rgw;
                par[1] = (f32)rp->rgh;
                par[4] = (f32)sd->px;
                par[5] = (f32)sd->py;
                par[6] = sd->pressed ? 1.0f : 0.0f;
                par[8] = (f32)(radius * rp->rgw);
                par[9] = (f32)(-strength);
                par[10] = (f32)sd->nclicks;
                const double crad = (radius * 2.5 < 0.004 ? 0.004 :
                                     radius * 2.5 > 0.12 ? 0.12 : radius * 2.5) * rp->rgw;
                const double camp = -strength * 1.5;
                for (u32 c = 0; c < 4; c++) {
                        if (c < sd->nclicks && c < 4) {
                                par[12 + c * 4 + 0] = (f32)sd->click_x[c];
                                par[12 + c * 4 + 1] = (f32)sd->click_y[c];
                                par[12 + c * 4 + 2] = (f32)crad;
                                par[12 + c * 4 + 3] = (f32)camp;
                        }
                }
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_RIPPLE_SPLAT;
                d.F16a = &rp->field[rp->cur];
                d.F16w = &rp->field[rp->cur ^ 1];
                d.par = par;
                d.npar = 7;
                if (!vk_run(v, win, &d))
                        return NULL;
                rp->cur ^= 1;
        }

        for (int s = 0; s < steps; s++) {
                f32 par[4];
                memset(par, 0, sizeof par);
                par[0] = (f32)rp->rgw;
                par[1] = (f32)rp->rgh;
                par[2] = (f32)(damping * 0.05);
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_RIPPLE_STEP;
                d.F16a = &rp->field[rp->cur];
                d.F16w = &rp->field[rp->cur ^ 1];
                d.par = par;
                d.npar = 1;
                if (!vk_run(v, win, &d))
                        return NULL;
                rp->cur ^= 1;
        }
        return &rp->field[rp->cur];
}

/* ========================================================================== *
 * Blur chain + composite (mirrors gl_layer's pass structure)
 * ========================================================================== */

static void gauss9(double sigma, f32 w[9])
{
        if (sigma < 0.4)
                sigma = 0.4;
        double sum = 0;
        for (int i = 0; i < 9; i++) {
                double x = (double)(i - 4);
                w[i] = (f32)exp(-(x * x) / (2.0 * sigma * sigma));
                sum += w[i];
        }
        for (int i = 0; i < 9; i++)
                w[i] = (f32)(w[i] / sum);
}

static bool ensure_scratch(RenderVK *v, int win, VImg *im, u32 w, u32 h)
{
        if (im->img && im->w == w && im->h == h)
                return true;
        if (im->img)
                vkimg_retire(v, im);
        return vkimg_new(v, win, im, w, h, VK_FORMAT_R8G8B8A8_UNORM_,
                         VK_RGBA8_USAGE, false);
}

/* the standard composite pass: layer over accum, placement + blend */
static bool composite_pass(RenderVK *v, int win, const VImg *src, bool src_linear,
                           const RLayerCmd *cmd, bool src_premul)
{
        VkWin_ *w = &v->win[win];
        VFrame *fr = &v->frame[v->cur_slot][win];
        u32 fw = w->accum[0].w, fh = w->accum[0].h;
        if (!fw || !fh)
                return false;
        u32 pw = cmd->place_w ? cmd->place_w : cmd->w;
        u32 ph = cmd->place_h ? cmd->place_h : cmd->h;
        if (!pw || !ph)
                return false;

        f32 par[4 * 4];
        memset(par, 0, sizeof par);
        par[0] = (f32)fw;
        par[1] = (f32)fh;
        par[2] = (f32)cmd->w;
        par[3] = (f32)cmd->h;
        par[4] = (f32)((double)fw / (double)pw);
        par[5] = (f32)((double)fh / (double)ph);
        par[6] = (f32)(-(double)cmd->dx / (double)pw);
        par[7] = (f32)(-(double)cmd->dy / (double)ph);
        par[8] = (f32)cmd->blend;
        par[9] = (f32)(cmd->opacity < 0 ? 0 : cmd->opacity > 1 ? 1 : cmd->opacity);
        par[10] = src_premul ? 1.0f : 0.0f;
        par[11] = fr->first_pending ? 1.0f : 0.0f;

        VDisp d;
        memset(&d, 0, sizeof d);
        d.kernel = K_COMPOSITE;
        d.A = src;
        d.A_lin = src_linear;
        d.C = &w->accum[w->accum_cur];          /* dst read (texelFetch)   */
        d.out8 = &w->accum[w->accum_cur ^ 1];
        d.par = par;
        d.npar = 4;
        if (!vk_run(v, win, &d))
                return false;
        w->accum_cur ^= 1;
        fr->first_pending = false;
        fr->layers++;
        return true;
}

/* video: upload planes + YUV->RGB blend composite in one dispatch */
static bool video_pass(RenderVK *v, int win, const RLayerCmd *cmd)
{
        const RSimVideo *vd = cmd->vsim;
        VkWin_ *w = &v->win[win];
        VFrame *fr = &v->frame[v->cur_slot][win];
        u32 cw = vd->w / 2, ch = vd->h / 2;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;

        if (v->v_y.img && (v->v_y.w != vd->w || v->v_y.h != vd->h)) {
                vkimg_retire(v, &v->v_y);
                vkimg_retire(v, &v->v_u);
                vkimg_retire(v, &v->v_v);
        }
        if (!v->v_y.img) {
                if (!vkimg_new(v, win, &v->v_y, vd->w, vd->h, VK_FORMAT_R8_UNORM_,
                               VK_R8_USAGE, false) ||
                    !vkimg_new(v, win, &v->v_u, cw, ch, VK_FORMAT_R8_UNORM_,
                               VK_R8_USAGE, false) ||
                    !vkimg_new(v, win, &v->v_v, cw, ch, VK_FORMAT_R8_UNORM_,
                               VK_R8_USAGE, false))
                        return false;
        }

        /* plane uploads: repack rows (tight) into the arena — plane data
         * arrives with decoder line sizes */
        {
                struct { const u8 *px; u32 w, h, ls; const VImg *im; } pl[3];
                memset(pl, 0, sizeof pl);
                pl[0].px = vd->y; pl[0].w = vd->w; pl[0].h = vd->h;
                pl[0].ls = vd->ls_y > 0 ? (u32)vd->ls_y : 0; pl[0].im = &v->v_y;
                pl[1].px = vd->u; pl[1].w = cw; pl[1].h = ch;
                pl[1].ls = vd->ls_u > 0 ? (u32)vd->ls_u : 0; pl[1].im = &v->v_u;
                pl[2].px = vd->v; pl[2].w = cw; pl[2].h = ch;
                pl[2].ls = vd->ls_v > 0 ? (u32)vd->ls_v : 0; pl[2].im = &v->v_v;
                for (int p = 0; p < 3; p++) {
                        if (!pl[p].px || pl[p].ls <= 0)
                                continue;
                        u64 bytes = (u64)pl[p].w * pl[p].h;
                        u64 off = 0;
                        u8 *dst = vk_arena_alloc(v, win, bytes, &off);
                        if (!dst)
                                return false;
                        f64 t0 = rev_monotonic();
                        if ((i32)pl[p].ls == (i32)pl[p].w) {
                                memcpy(dst, pl[p].px, (size_t)bytes);
                        } else {
                                for (u32 y = 0; y < pl[p].h; y++)
                                        memcpy(dst + (size_t)y * pl[p].w,
                                               pl[p].px + (size_t)y * pl[p].ls,
                                               pl[p].w);
                        }
                        v->perf_upload_ms += (rev_monotonic() - t0) * 1000.0;

                        vk_img_barrier(v, fr->cmd, pl[p].im->img,
                                       VK_ACCESS_SHADER_READ_BIT_ |
                                       VK_ACCESS_SHADER_WRITE_BIT_ |
                                       VK_ACCESS_TRANSFER_WRITE_BIT_,
                                       VK_ACCESS_TRANSFER_WRITE_BIT_,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT_);
                        VkBufferImageCopy_ cp;
                        memset(&cp, 0, sizeof cp);
                        cp.bufferOffset = off;
                        cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
                        cp.imageSubresource.layerCount = 1;
                        cp.imageExtent.width = pl[p].w;
                        cp.imageExtent.height = pl[p].h;
                        cp.imageExtent.depth = 1;
                        v->vkCmdCopyBufferToImage(fr->cmd, fr->arena, pl[p].im->img,
                                                  VK_IMAGE_LAYOUT_GENERAL_, 1, &cp);
                        vk_img_barrier(v, fr->cmd, pl[p].im->img,
                                       VK_ACCESS_TRANSFER_WRITE_BIT_,
                                       VK_ACCESS_SHADER_READ_BIT_,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT_);
                }
        }

        u32 fw = w->accum[0].w, fh = w->accum[0].h;
        u32 pw = cmd->place_w ? cmd->place_w : cmd->w;
        u32 ph = cmd->place_h ? cmd->place_h : cmd->h;
        if (!fw || !fh || !pw || !ph)
                return false;

        f32 par[3 * 4];
        memset(par, 0, sizeof par);
        par[0] = (f32)fw;
        par[1] = (f32)fh;
        par[4] = (f32)((double)fw / (double)pw);
        par[5] = (f32)((double)fh / (double)ph);
        par[6] = (f32)(-(double)cmd->dx / (double)pw);
        par[7] = (f32)(-(double)cmd->dy / (double)ph);
        par[8] = (f32)cmd->blend;
        par[9] = (f32)(cmd->opacity < 0 ? 0 : cmd->opacity > 1 ? 1 : cmd->opacity);
        par[10] = fr->first_pending ? 1.0f : 0.0f;

        VDisp d;
        memset(&d, 0, sizeof d);
        d.kernel = K_VIDEO;
        d.A = &v->v_y;
        d.A_lin = true;
        d.B = &v->v_u;
        d.B_lin = true;
        d.C = &v->v_v;
        d.C_lin = true;
        d.D = &w->accum[w->accum_cur];
        d.D_lin = false;
        d.out8 = &w->accum[w->accum_cur ^ 1];
        d.par = par;
        d.npar = 3;
        if (!vk_run(v, win, &d))
                return false;
        w->accum_cur ^= 1;
        fr->first_pending = false;
        fr->layers++;
        return true;
}

/* ========================================================================== *
 * Frame recording: layer()
 * ========================================================================== */

static bool vk_layer(RRenderer *rr, const RLayerCmd *cmd)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        int win = v->cur_win;
        VkWin_ *w = &v->win[win];
        VFrame *fr = &v->frame[v->cur_slot][win];
        if (!cmd || !cmd->w || !cmd->h || !fr->recording)
                return false;

        /* 0. GPU ripple layers: height-field step + refraction composite */
        if (cmd->rsim) {
                if (!v->fmt16_ok)
                        return false;    /* scene should not have offloaded */
                VKSim *rp = vk_sim_slot(v, win, cmd->rsim, true, cmd->rsim->gw,
                                        cmd->rsim->gh, 0, 0);
                if (!rp)
                        return false;
                const VImg *field = vk_ripple_step(v, win, rp, cmd->rsim);
                if (!field)
                        return false;
                u32 fw = w->accum[0].w, fh = w->accum[0].h;
                double strength = cmd->rsim->strength < 0.1 ? 0.1 :
                                  cmd->rsim->strength > 8.0 ? 8.0 :
                                  cmd->rsim->strength;
                f32 par[2 * 4];
                memset(par, 0, sizeof par);
                par[0] = (f32)fw;
                par[1] = (f32)fh;
                par[2] = (f32)rp->rgw;
                par[3] = (f32)rp->rgh;
                par[4] = (f32)(strength * 26.0 / (double)fw);
                par[5] = (f32)(strength * 26.0 / (double)fh);
                par[6] = (f32)(cmd->opacity < 0 ? 0 :
                               cmd->opacity > 1 ? 1 : cmd->opacity);
                par[7] = fr->first_pending ? 1.0f : 0.0f;
                VDisp d;
                memset(&d, 0, sizeof d);
                d.kernel = K_RIPPLE_COMPOSITE;
                d.A = &w->accum[w->accum_cur];       /* nearest: base read  */
                d.A_lin = false;
                d.B = &w->accum[w->accum_cur];       /* linear: refraction  */
                d.B_lin = true;
                d.F16a = field;
                d.out8 = &w->accum[w->accum_cur ^ 1];
                d.par = par;
                d.npar = 2;
                if (!vk_run(v, win, &d))
                        return false;
                w->accum_cur ^= 1;
                fr->first_pending = false;
                fr->layers++;
                return true;
        }

        /* 1. obtain the layer's source image */
        const VImg *src;
        bool src_premul = false;
        bool src_linear = cmd->linear;
        if (cmd->vsim) {
                return video_pass(v, win, cmd);
        }
        if (cmd->grad) {
                src = vk_grad_layer(v, win, cmd);
                if (!src)
                        return false;
        } else if (cmd->psim) {
                src = vk_particles_layer(v, win, cmd);
                if (!src)
                        return false;
        } else if (cmd->sim) {
                if (!v->fmt16_ok)
                        return false;    /* scene should not have offloaded */
                VKSim *f = vk_sim_slot(v, win, cmd->sim, false,
                                       cmd->sim->dye_w / 2, cmd->sim->dye_h / 2,
                                       cmd->sim->dye_w, cmd->sim->dye_h);
                if (!f)
                        return false;
                src = vk_fluid_step(v, win, f, cmd->sim);
                if (!src)
                        return false;
        } else {
                if (!cmd->px)
                        return false;
                src = upload_layer_tex(v, win, cmd);
                if (!src)
                        return false;
        }

        /* 2. blur (premultiply -> downsample + separable 9-tap + upscale) */
        const VImg *layer_img = src;
        if (cmd->blur >= 0.5) {
                if (!ensure_scratch(v, win, &v->tex_a, cmd->w, cmd->h) ||
                    !ensure_scratch(v, win, &v->tex_d, cmd->w, cmd->h))
                        return false;
                /* premultiply into tex_a */
                {
                        f32 par[2 * 4];
                        memset(par, 0, sizeof par);
                        par[0] = (f32)cmd->w;
                        par[1] = (f32)cmd->h;
                        par[4] = 1.0f;    /* premul on */
                        VDisp d;
                        memset(&d, 0, sizeof d);
                        d.kernel = K_COPYPREMUL;
                        d.A = src;
                        d.A_lin = false;
                        d.out8 = &v->tex_a;
                        d.par = par;
                        d.npar = 2;
                        if (!vk_run(v, win, &d))
                                return false;
                }

                double sigma = cmd->blur;
                int k = 0;
                while (sigma / (double)(1 << k) > 4.0 && k < 6)
                        k++;
                double sloc = sigma / (double)(1 << k);
                u32 dw = cmd->w >> k, dh = cmd->h >> k;
                if (dw < 1) dw = 1;
                if (dh < 1) dh = 1;
                int spacing = (int)(sloc / 2.0 + 0.5);
                if (spacing < 1) spacing = 1;
                f32 w9[9];
                gauss9(sloc / (double)spacing, w9);

                if (!ensure_scratch(v, win, &v->tex_b, dw, dh) ||
                    !ensure_scratch(v, win, &v->tex_c, dw, dh))
                        return false;

                /* H: tex_a -> tex_b (linear-sampling downscale blur) */
                {
                        f32 par[5 * 4];
                        memset(par, 0, sizeof par);
                        par[0] = (f32)dw;
                        par[1] = (f32)dh;
                        par[4] = (f32)((double)spacing / (double)cmd->w);
                        par[5] = 0.0f;
                        for (int i = 0; i < 9; i++) {
                                if (i < 4)      par[8 + i] = w9[i];
                                else if (i < 8) par[12 + (i - 4)] = w9[i];
                                else            par[16] = w9[i];
                        }
                        VDisp d;
                        memset(&d, 0, sizeof d);
                        d.kernel = K_BLUR;
                        d.A = &v->tex_a;
                        d.A_lin = true;
                        d.out8 = &v->tex_b;
                        d.par = par;
                        d.npar = 5;
                        if (!vk_run(v, win, &d))
                                return false;
                }
                /* V: tex_b -> tex_c */
                {
                        f32 par[5 * 4];
                        memset(par, 0, sizeof par);
                        par[0] = (f32)dw;
                        par[1] = (f32)dh;
                        par[4] = 0.0f;
                        par[5] = (f32)((double)spacing / (double)cmd->h);
                        for (int i = 0; i < 9; i++) {
                                if (i < 4)      par[8 + i] = w9[i];
                                else if (i < 8) par[12 + (i - 4)] = w9[i];
                                else            par[16] = w9[i];
                        }
                        VDisp d;
                        memset(&d, 0, sizeof d);
                        d.kernel = K_BLUR;
                        d.A = &v->tex_b;
                        d.A_lin = true;
                        d.out8 = &v->tex_c;
                        d.par = par;
                        d.npar = 5;
                        if (!vk_run(v, win, &d))
                                return false;
                }
                /* upscale: tex_c -> tex_d (raster size, bilinear) */
                {
                        f32 par[2 * 4];
                        memset(par, 0, sizeof par);
                        par[0] = (f32)cmd->w;
                        par[1] = (f32)cmd->h;
                        par[4] = 0.0f;    /* plain copy */
                        VDisp d;
                        memset(&d, 0, sizeof d);
                        d.kernel = K_COPYPREMUL;
                        d.A = &v->tex_c;
                        d.A_lin = true;
                        d.out8 = &v->tex_d;
                        d.par = par;
                        d.npar = 2;
                        if (!vk_run(v, win, &d))
                                return false;
                }
                layer_img = &v->tex_d;
                src_premul = true;
        }

        /* 3. composite: layer over accum, flip */
        return composite_pass(v, win, layer_img, src_linear, cmd, src_premul);
}

/* ========================================================================== *
 * Frame lifecycle: begin / end / perf / resize
 * ========================================================================== */

static u32 ts_query_base(RenderVK *v, int win)
{
        return (u32)(v->cur_slot * REV_PLATFORM_MAX_OUTPUTS + win) * 2;
}

static bool vk_begin(RRenderer *rr, int win, u32 w, u32 h)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return false;
        v->cur_win = win;
        v->cur_slot = (int)(v->frame_no % (u64)VK_SLOTS);
        VFrame *fr = &v->frame[v->cur_slot][win];

        if (!vk_frame_create(v, win))
                return false;

        /* slot reuse: retire the frame that used this (slot, win) 3
         * engine-frames ago (fence), then reset everything it owned */
        if (fr->recording) {
                /* a previous frame abandoned this slot mid-recording (a
                 * failed present path returned early) — end the buffer so
                 * the pool reset below never sees a recording command
                 * buffer, and drop the timer pair whose TOP write will
                 * never execute (its BOTTOM is in the discarded buffer) */
                v->vkEndCommandBuffer(fr->cmd);
                fr->recording = false;
                fr->ts_valid = false;
        }
        if (fr->submitted) {
                u32 wr = v->vkWaitForFences(v->dev, 1, &fr->fence, 1, 1000000000ull);
                if (wr == (u32)VK_ERROR_DEVICE_LOST_) {
                        if (!v->device_lost) {
                                v->device_lost = true;
                                LOGE("vulkan: DEVICE LOST — the driver/GPU reset "
                                     "or died; the Vulkan tier can no longer present");
                        }
                        return false;
                }
                if (wr != VK_SUCCESS_)
                        return false;
                v->vkResetFences(v->dev, 1, &fr->fence);
        }
        fr->submitted = false;
        fr->recording = false;

        vk_grave_sweep(v);

        v->vkResetCommandPool(v->dev, fr->cmdpool, 1); /* RELEASE_RESOURCES: actually free */
        v->vkResetDescriptorPool(v->dev, fr->dpool, 0);
        fr->ubo_used = 0;
        fr->arena_used = 0;
        fr->layers = 0;
        fr->first_pending = true;
        v->dispatches = 0;

        VkCommandBufferBeginInfo_ bi;
        memset(&bi, 0, sizeof bi);
        bi.sType_ = ST_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT_;
        if (v->vkBeginCommandBuffer(fr->cmd, &bi) != VK_SUCCESS_)
                return false;
        fr->recording = true;

        /* GPU frame timing: poll THIS (slot,win)'s pair (the fence above
         * guarantees the writes retired), then re-arm. Same asynchronous,
         * several-frames-behind semantics as the GL tier's query ring. */
        if (v->tq_ok) {
                u32 qb = ts_query_base(v, win);
                if (fr->ts_valid) {
                        u64 res[2] = { 0, 0 };
                        u32 gr = v->vkGetQueryPoolResults(v->dev, v->tq, qb, 2,
                                                          sizeof res, res,
                                                          sizeof(u64),
                                                          VK_QUERY_RESULT_64_BIT_);
                        if (gr == VK_SUCCESS_ && res[1] >= res[0]) {
                                v->perf_gpu_ms_sum +=
                                        (f64)(res[1] - res[0]) * v->ts_period / 1e6;
                                v->perf_gpu_samples++;
                        }
                        fr->ts_valid = false;
                }
                v->vkCmdResetQueryPool(fr->cmd, v->tq, qb, 2);
                v->vkCmdWriteTimestamp(fr->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT_,
                                       v->tq, qb);
                fr->ts_valid = true;    /* armed; BOTTOM lands at end() */
        }

        /* accumulation targets (GENERAL layout; the first composite pass
         * initializes every texel — u_first — so no clear is needed) */
        VkWin_ *wk = &v->win[win];
        if (wk->accum[0].img && (wk->accum[0].w != w || wk->accum[0].h != h)) {
                /* size change: retire both, recreate below */
                vkimg_retire(v, &wk->accum[0]);
                vkimg_retire(v, &wk->accum[1]);
        }
        if (!wk->accum[0].img) {
                if (!vkimg_new(v, win, &wk->accum[0], w, h,
                               VK_FORMAT_R8G8B8A8_UNORM_, VK_RGBA8_USAGE, false) ||
                    !vkimg_new(v, win, &wk->accum[1], w, h,
                               VK_FORMAT_R8G8B8A8_UNORM_, VK_RGBA8_USAGE, false))
                        return false;
        }
        wk->accum_cur = 0;
        return true;
}

/* clear the current accum (empty frames present black, not stale data) */
static void record_clear_accum(RenderVK *v, int win)
{
        VFrame *fr = &v->frame[v->cur_slot][win];
        VkWin_ *w = &v->win[win];
        VImg *im = &w->accum[w->accum_cur];
        if (!im->img)
                return;
        vk_img_barrier(v, fr->cmd, im->img,
                       VK_ACCESS_SHADER_READ_BIT_ | VK_ACCESS_SHADER_WRITE_BIT_ |
                       VK_ACCESS_TRANSFER_WRITE_BIT_,
                       VK_ACCESS_TRANSFER_WRITE_BIT_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT_);
        VkClearColorValue_ cc;
        memset(&cc, 0, sizeof cc);
        struct { u32 aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; } sr;
        memset(&sr, 0, sizeof sr);
        sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        sr.levelCount = 1;
        sr.layerCount = 1;
        v->vkCmdClearColorImage(fr->cmd, im->img, VK_IMAGE_LAYOUT_GENERAL_, &cc, 1, &sr);
        vk_img_barrier(v, fr->cmd, im->img, VK_ACCESS_TRANSFER_WRITE_BIT_,
                       VK_ACCESS_TRANSFER_READ_BIT_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT_);
}

static bool vk_ensure_readback(RenderVK *v, int win, u32 w, u32 h)
{
        VkWin_ *w_ = &v->win[win];
        u64 need = (u64)w * h * 4;
        if (w_->rb_map && w_->rb_size >= need)
                return true;
        if (w_->readback) {
                /* the previous frame's copy retired with its fence (begin
                 * waits it before reuse); safe to replace now */
                v->vkUnmapMemory(v->dev, w_->readback_mem);
                v->vkDestroyBuffer(v->dev, w_->readback, NULL);
                v->vkFreeMemory(v->dev, w_->readback_mem, NULL);
                w_->readback = 0;
                w_->readback_mem = 0;
                w_->rb_map = NULL;
        }
        bool coh = true;
        if (!vkbuf_new(v, &w_->readback, &w_->readback_mem, &w_->rb_map, need,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT_, true, &coh))
                return false;
        w_->rb_size = need;
        w_->rb_coherent = coh;
        {
                /* log the memory class once per (re)allocation — an uncached
                 * heap is the field-report-#4 failure signature, and this
                 * line makes it visible without vulkaninfo */
                struct { u64 size, alignment; u32 memoryTypeBits; } reqs;
                memset(&reqs, 0, sizeof reqs);
                v->vkGetBufferMemoryRequirements(v->dev, w_->readback, &reqs);
                i32 idx = vk_pick_download_memtype(v, reqs.memoryTypeBits, NULL);
                if (idx >= 0) {
                        VkPhysicalDeviceMemoryProperties_ mp;
                        v->vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
                        u32 props = mp.memoryTypes[idx].propertyFlags;
                        LOGI("vulkan: frame download staging: memory type %d "
                             "(%s%s, %s)",
                             idx,
                             props & VK_MEMORY_PROPERTY_HOST_CACHED_BIT_
                                     ? "host-cached" : "UNCACHED",
                             props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_
                                     ? "+device-local" : "",
                             props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_
                                     ? "coherent" : "non-coherent: invalidated per frame");
                }
        }
        return true;
}

/* ---- WSI present: blit the finished accum into the swapchain, FIFO ------
 *
 * FIELD REPORT #5 (REV_VK_PRESENT=wsi on a GTX 1050 Ti: 1.0 fps, frozen
 * wallpaper, p50 frame exactly 1000.0 ms): the old flow could block a
 * full second inside vkAcquireNextImageKHR while nothing drained the
 * Wayland socket, and on the timeout path it returned WITHOUT
 * re-submitting the sync fence it had already reset at entry — so every
 * following frame waited that dead fence for ANOTHER second and no
 * buffer ever presented again. The stalled slot rotation also starved
 * the GPU timer queries (gpu=0.0 "unavailable" although init reported
 * them usable).
 *
 * The flow is now wedge-free by construction:
 *   - acquire runs in 8 ms slices with the platform event queue pumped
 *     between attempts (Wayland WSI buffer releases and input flow while
 *     we wait); a frame gives up after ~250 ms instead of 1 s;
 *   - the sync fence is reset only AFTER an image is acquired — a failed
 *     acquire leaves it in its old signaled state, so no dead-fence
 *     poison can ever take hold;
 *   - the slot rotation (sync_slot + frame_no) advances on EVERY exit,
 *     so frame slots, sync slots and timer queries keep cycling even
 *     while frames are being dropped. */
static bool vk_wsi_present_frame(RenderVK *v, int win, VFrame *fr)
{
        VkWin_ *w = &v->win[win];
        VImg *acc = &w->accum[w->accum_cur];
        VkSync_ *s = &w->sync[w->sync_slot];
        bool ok = false;

        /* wait the sync-slot's fence: signaled by the submit of the last
         * frame that USED this slot (fences start SIGNALED). It is only
         * reset below when a frame will actually be submitted, so this
         * wait cannot wedge; the 250 ms bound is a driver-hang guard. */
        u32 wr = v->vkWaitForFences(v->dev, 1, &s->fence, 1, 250000000ull);
        if (wr == (u32)VK_ERROR_DEVICE_LOST_) {
                if (!v->device_lost) {
                        v->device_lost = true;
                        LOGE("vulkan: DEVICE LOST — the Vulkan tier can "
                             "no longer present");
                }
                goto out;
        }
        if (wr != VK_SUCCESS_)
                goto out;

        /* acquire, sliced: 8 ms per attempt, ~250 ms total budget, pumping
         * the platform's event queue between attempts. FIFO images sit
         * with the compositor until a newer buffer latches, and the
         * releases arrive as display events — waiting them out with the
         * event loop stalled is deadlock food. */
        u32 img = 0;
        u32 r = VK_TIMEOUT_;
        {
                f64 deadline = rev_monotonic() + 0.250;
                for (;;) {
                        f64 rem = deadline - rev_monotonic();
                        if (rem <= 0.0)
                                break;
                        u64 tns = (u64)(rem * 1e9);
                        if (tns > 8000000ull)
                                tns = 8000000ull;
                        r = v->vkAcquireNextImageKHR(v->dev, w->swapchain,
                                                     tns, s->acq, 0, &img);
                        if (vkres_ok(r))
                                break;
                        if ((i32)r == VK_ERROR_OUT_OF_DATE_KHR_) {
                                vkwin_destroy(v, w);
                                char err[128] = { 0 };
                                if (!vkwin_create(v, win, err, sizeof err))
                                        LOGW("vulkan: swapchain recreate failed: %s", err);
                                goto out;    /* frame consumed; next one re-presents */
                        }
                        if (r != (u32)VK_TIMEOUT_)
                                goto out;    /* real acquire error — drop the frame */
                        /* slice expired: drain compositor events (releases,
                         * input) and retry while the budget lasts */
                        if (v->plat->pump)
                                v->plat->pump(v->plat);
                }
                if (!vkres_ok(r)) {
                        static f64 last_starve;
                        f64 now = rev_monotonic();
                        if (now - last_starve > 5.0) {
                                last_starve = now;
                                /* While the platform reports the surface
                                 * hidden (idle power-save), a stalled
                                 * acquire is EXPECTED — the compositor is
                                 * not consuming. Keep it out of the WARN
                                 * stream so field logs stay honest. */
                                const bool hidden = v->plat->surface_visible &&
                                                    !v->plat->surface_visible(v->plat);
                                if (hidden)
                                        LOGD("vulkan: acquire idle (compositor "
                                             "not consuming — surface hidden)");
                                else
                                        LOGW("vulkan: swapchain starved — the compositor "
                                             "is not releasing FIFO images fast enough; "
                                             "dropping frames (acquire timeout)");
                        }
                        goto out;
                }
        }

        /* an image is in hand: THIS frame will submit, so the fence may
         * be reset now. A timeout above left it in its old (signaled)
         * state — the next wait on this slot sees "already complete"
         * instead of a dead fence, so the loop can never wedge. */
        v->vkResetFences(v->dev, 1, &s->fence);

        /* accum -> swapchain image (BLIT; handles the BGRA component
         * swizzle, and the identity rect is orientation-neutral —
         * accum rows are already top-down = presentation order).
         * _SRGB swapchains re-encode gamma on blit writes, and the
         * engine's bytes are already display-encoded — for those
         * (rare: most compositors offer UNORM variants) run one
         * sRGB-decode compute pass first so the blit's encode makes
         * the roundtrip the identity. */
        {
                const VImg *blit_src = acc;
                if (w->srgb_format) {
                        if (!ensure_scratch(v, win, &v->tex_a, acc->w, acc->h))
                                goto out;
                        f32 par[4];
                        memset(par, 0, sizeof par);
                        par[0] = (f32)acc->w;
                        par[1] = (f32)acc->h;
                        VDisp d;
                        memset(&d, 0, sizeof d);
                        d.kernel = K_DECODE;
                        d.F16a = acc;    /* rgba8 read (decode.comp's slot 8) */
                        d.out8 = &v->tex_a;
                        d.par = par;
                        d.npar = 1;
                        if (!vk_run(v, win, &d))
                                goto out;
                        blit_src = &v->tex_a;
                }
                vk_img_barrier(v, fr->cmd, blit_src->img,
                               VK_ACCESS_SHADER_READ_BIT_ |
                               VK_ACCESS_SHADER_WRITE_BIT_ |
                               VK_ACCESS_TRANSFER_WRITE_BIT_,
                               VK_ACCESS_TRANSFER_READ_BIT_,
                               VK_PIPELINE_STAGE_TRANSFER_BIT_);
                {
                        VkImageMemoryBarrier_ b;
                        memset(&b, 0, sizeof b);
                        b.sType_ = ST_IMAGE_MEMORY_BARRIER;
                        b.srcAccessMask = 0;
                        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT_;
                        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED_;
                        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_;
                        b.image = w->images[img];
                        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
                        b.subresourceRange.levelCount = 1;
                        b.subresourceRange.layerCount = 1;
                        v->vkCmdPipelineBarrier(fr->cmd,
                                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT_,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT_,
                                                0, 0, NULL, 0, NULL, 1, &b);
                }
                {
                        VkImageBlit_ bl;
                        memset(&bl, 0, sizeof bl);
                        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
                        bl.srcSubresource.layerCount = 1;
                        bl.srcOffsets[0].x = 0;
                        bl.srcOffsets[0].y = 0;
                        bl.srcOffsets[0].z = 0;
                        bl.srcOffsets[1].x = (i32)blit_src->w;
                        bl.srcOffsets[1].y = (i32)blit_src->h;
                        bl.srcOffsets[1].z = 1;
                        bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
                        bl.dstSubresource.layerCount = 1;
                        bl.dstOffsets[0].x = 0;
                        bl.dstOffsets[0].y = 0;
                        bl.dstOffsets[0].z = 0;
                        bl.dstOffsets[1].x = (i32)w->width;
                        bl.dstOffsets[1].y = (i32)w->height;
                        bl.dstOffsets[1].z = 1;
                        v->vkCmdBlitImage(fr->cmd, blit_src->img,
                                          VK_IMAGE_LAYOUT_GENERAL_,
                                          w->images[img],
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_,
                                          1, &bl, VK_FILTER_NEAREST_);
                }
                {
                        VkImageMemoryBarrier_ b;
                        memset(&b, 0, sizeof b);
                        b.sType_ = ST_IMAGE_MEMORY_BARRIER;
                        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT_;
                        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT_;
                        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_;
                        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR_;
                        b.image = w->images[img];
                        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
                        b.subresourceRange.levelCount = 1;
                        b.subresourceRange.layerCount = 1;
                        v->vkCmdPipelineBarrier(fr->cmd,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT_,
                                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT_,
                                                0, 0, NULL, 0, NULL, 1, &b);
                }
                if (v->tq_ok)
                        v->vkCmdWriteTimestamp(fr->cmd,
                                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT_,
                                               v->tq, ts_query_base(v, win) + 1);
                if (v->vkEndCommandBuffer(fr->cmd) != VK_SUCCESS_)
                        goto out;
                fr->recording = false;

                VkSubmitInfo_ si;
                memset(&si, 0, sizeof si);
                si.sType_ = ST_SUBMIT_INFO;
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &s->acq;
                u32 stage = VK_PIPELINE_STAGE_TRANSFER_BIT_;
                si.pWaitDstStageMask = &stage;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &fr->cmd;
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &s->rend;
                u32 sr = v->vkQueueSubmit(v->queue, 1, &si, s->fence);
                if (sr == (u32)VK_ERROR_DEVICE_LOST_) {
                        if (!v->device_lost) {
                                v->device_lost = true;
                                LOGE("vulkan: DEVICE LOST during submit — "
                                     "presenting disabled for this session");
                        }
                        goto out;
                }
                if (sr != VK_SUCCESS_)
                        goto out;

                VkPresentInfoKHR_ pi;
                memset(&pi, 0, sizeof pi);
                pi.sType_ = ST_PRESENT_INFO_KHR;
                pi.waitSemaphoreCount = 1;
                pi.pWaitSemaphores = &s->rend;
                pi.swapchainCount = 1;
                pi.pSwapchains = &w->swapchain;
                pi.pImageIndices = &img;
                /* Latched-frame evidence for the idle machinery (Wayland):
                 * request a wl_surface.frame callback on the very surface
                 * this present is about to commit. The request rides the
                 * same client queue, so the driver's commit carries it and
                 * the compositor fires it on latch — wlshim's visibility
                 * clock then has the same evidence the wl_shm path uses.
                 * No-op elsewhere (X11 / raw client: hook is NULL). */
                if (v->plat->wsi_frame_request)
                        v->plat->wsi_frame_request(v->plat, win);
                r = v->vkQueuePresentKHR(v->queue, &pi);
                if ((i32)r == VK_ERROR_OUT_OF_DATE_KHR_) {
                        vkwin_destroy(v, w);
                        char rerr[128] = { 0 };
                        if (vkwin_create(v, win, rerr, sizeof rerr))
                                LOGI("vulkan: swapchain recreated after OUT_OF_DATE present");
                        else
                                LOGW("vulkan: swapchain recreate failed: %s", rerr);
                        ok = true;    /* frame consumed — same as the old path */
                        goto out;
                }
                if (r == (u32)VK_ERROR_DEVICE_LOST_) {
                        if (!v->device_lost) {
                                v->device_lost = true;
                                LOGE("vulkan: DEVICE LOST during present — "
                                     "presenting disabled for this session");
                        }
                        goto out;
                }
                ok = vkres_ok(r) ? true : false;
        }

out:
        /* rotation on EVERY exit — frame slots, sync slots and the GPU
         * timer query pairs all cycle on frame_no/sync_slot; stalling
         * the rotation on failure paths is what starved gpu= to 0.0 in
         * the field. Also never leave the command buffer recording. */
        if (fr->recording) {
                v->vkEndCommandBuffer(fr->cmd);    /* discard-safe end */
                fr->recording = false;
                fr->ts_valid = false;    /* the armed TOP write never executes */
        }
        w->sync_slot ^= 1;
        v->frame_no++;
        return ok;
}

static bool vk_end(RRenderer *rr, const RFb *cpu_frame)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        int win = v->cur_win;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return false;
        VFrame *fr = &v->frame[v->cur_slot][win];
        VkWin_ *w = &v->win[win];

        /* engine-composited fallback (the scene needed CPU compositing —
         * e.g. ripple layers when this renderer declined GPU sims) */
        if (cpu_frame && cpu_frame->px) {
                if (v->present_mode == VK_PRESENT_PLATFORM) {
                        /* no Vulkan work this frame: discard the recording
                         * (reset next begin) and present the CPU's frame */
                        if (fr->recording)
                                v->vkEndCommandBuffer(fr->cmd);
                        fr->recording = false;
                        fr->ts_valid = false;
                        v->frame_no++;
                        return v->plat->present_cpu(v->plat, win,
                                                    cpu_frame->px,
                                                    cpu_frame->w, cpu_frame->h,
                                                    false);
                }
                /* WSI: upload the CPU frame into accum[0] and blit-present */
                if (w->accum[0].img &&
                    (w->accum[0].w != cpu_frame->w || w->accum[0].h != cpu_frame->h)) {
                        vkimg_retire(v, &w->accum[0]);
                        vkimg_retire(v, &w->accum[1]);
                }
                if (!w->accum[0].img) {
                        if (!vkimg_new(v, win, &w->accum[0], cpu_frame->w,
                                       cpu_frame->h, VK_FORMAT_R8G8B8A8_UNORM_,
                                       VK_RGBA8_USAGE, false))
                                return false;
                }
                w->accum_cur = 0;
                if (!vk_upload_image(v, win, &w->accum[0], cpu_frame->px,
                                     cpu_frame->w, cpu_frame->h, 4))
                        return false;
        } else if (fr->layers == 0 && fr->recording) {
                /* nothing composited: present a cleared accum */
                record_clear_accum(v, win);
        }

        /* ---- platform present: copy the finished frame out for SHM ------ */
        if (v->present_mode == VK_PRESENT_PLATFORM) {
                if (!fr->recording)
                        return false;
                VImg *acc = &w->accum[w->accum_cur];
                if (!acc->img)
                        return false;
                if (!vk_ensure_readback(v, win, acc->w, acc->h))
                        return false;

                /* wl_shm ARGB8888 is PREMULTIPLIED: when the platform
                 * presenter wants premultiplied input, run the
                 * straight->premul pass as a compute kernel (the accum
                 * ping-pong twin is free scratch at this point — next
                 * frame's first composite re-initializes every texel), so
                 * the CPU-side presenter is a single memcpy instead of a
                 * 2M-iteration scalar loop over GPU memory. X11 has no
                 * premul convention: copy the straight bytes through. */
                VImg *copy_src = acc;
                bool pre_premul = false;
                if (v->plat->present_premul) {
                        VImg *pre = &w->accum[w->accum_cur ^ 1];
                        if (!pre->img)
                                return false;
                        f32 par[2 * 4];
                        memset(par, 0, sizeof par);
                        par[0] = (f32)acc->w;
                        par[1] = (f32)acc->h;
                        par[4] = 1.0f;    /* premul on */
                        /* wl_shm ARGB8888 wants memory B,G,R,A; the engine's
                         * accum is R,G,B,A — swizzle on store (field report
                         * #5: blues rendered red before the flag existed).
                         * X11 takes engine order (x11.c converts). */
                        par[5] = v->plat->present_bgra ? 1.0f : 0.0f;
                        VDisp d;
                        memset(&d, 0, sizeof d);
                        d.kernel = K_COPYPREMUL;
                        d.A = acc;
                        d.A_lin = false;    /* nearest: exact texel centers */
                        d.out8 = pre;
                        d.par = par;
                        d.npar = 2;
                        if (!vk_run(v, win, &d))
                                return false;
                        copy_src = pre;
                        pre_premul = true;
                }
                vk_img_barrier(v, fr->cmd, copy_src->img,
                               VK_ACCESS_SHADER_READ_BIT_ |
                               VK_ACCESS_SHADER_WRITE_BIT_ |
                               VK_ACCESS_TRANSFER_WRITE_BIT_,
                               VK_ACCESS_TRANSFER_READ_BIT_,
                               VK_PIPELINE_STAGE_TRANSFER_BIT_);
                VkBufferImageCopy_ cp;
                memset(&cp, 0, sizeof cp);
                cp.bufferOffset = 0;
                cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
                cp.imageSubresource.layerCount = 1;
                cp.imageExtent.width = acc->w;
                cp.imageExtent.height = acc->h;
                cp.imageExtent.depth = 1;
                v->vkCmdCopyImageToBuffer(fr->cmd, copy_src->img,
                                          VK_IMAGE_LAYOUT_GENERAL_,
                                          w->readback, 1, &cp);
                {
                        /* transfer -> host visibility (matters for the
                         * HOST_CACHED non-coherent tier; harmless otherwise) */
                        VkBufferMemoryBarrier_ bb;
                        memset(&bb, 0, sizeof bb);
                        bb.sType_ = ST_BUFFER_MEMORY_BARRIER;
                        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT_;
                        bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT_;
                        bb.buffer = w->readback;
                        bb.offset = 0;
                        bb.size = VK_WHOLE_SIZE_;
                        v->vkCmdPipelineBarrier(fr->cmd,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT_,
                                                VK_PIPELINE_STAGE_HOST_BIT_, 0,
                                                0, NULL, 1, &bb, 0, NULL);
                }
                if (v->tq_ok)
                        v->vkCmdWriteTimestamp(fr->cmd,
                                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT_,
                                               v->tq, ts_query_base(v, win) + 1);
                if (v->vkEndCommandBuffer(fr->cmd) != VK_SUCCESS_)
                        return false;
                fr->recording = false;

                VkSubmitInfo_ si;
                memset(&si, 0, sizeof si);
                si.sType_ = ST_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &fr->cmd;
                u32 sr = v->vkQueueSubmit(v->queue, 1, &si, fr->fence);
                if (sr == (u32)VK_ERROR_DEVICE_LOST_) {
                        if (!v->device_lost) {
                                v->device_lost = true;
                                LOGE("vulkan: DEVICE LOST during submit — "
                                     "presenting disabled for this session");
                        }
                        return false;
                }
                if (sr != VK_SUCCESS_)
                        return false;
                fr->submitted = true;

                /* the readback is consumed immediately: wait, then hand the
                 * pixels to the platform presenter (wl_shm / MIT-SHM) */
                u32 wr = v->vkWaitForFences(v->dev, 1, &fr->fence, 1, 1000000000ull);
                if (wr != VK_SUCCESS_) {
                        if (wr == (u32)VK_ERROR_DEVICE_LOST_ && !v->device_lost) {
                                v->device_lost = true;
                                LOGE("vulkan: DEVICE LOST — the Vulkan tier can "
                                     "no longer present");
                        }
                        return false;
                }
                if (!w->rb_coherent && v->vkInvalidateMappedMemoryRanges) {
                        VkMappedMemoryRange_ range;
                        memset(&range, 0, sizeof range);
                        range.sType_ = ST_MAPPED_MEMORY_RANGE;
                        range.memory = w->readback_mem;
                        range.offset = 0;
                        range.size = w->rb_size;
                        v->vkInvalidateMappedMemoryRanges(v->dev, 1, &range);
                }
                v->frame_no++;
                return v->plat->present_cpu(v->plat, win,
                                            (const u32 *)w->rb_map,
                                            acc->w, acc->h, pre_premul);
        }

        /* ---- WSI present: blit into the swapchain, FIFO present ---------
         * (wedge-free flow: see vk_wsi_present_frame — field report #5) */
        if (!fr->recording || !w->ready)
                return false;
        if (!w->accum[w->accum_cur].img)
                return false;
        return vk_wsi_present_frame(v, win, fr);
}

static bool vk_perf(RRenderer *rr, f64 *cpu_ms, f64 *gpu_ms)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        if (cpu_ms)
                *cpu_ms = v->perf_upload_ms;
        if (gpu_ms)
                *gpu_ms = v->perf_gpu_samples ?
                          v->perf_gpu_ms_sum / (f64)v->perf_gpu_samples : 0.0;
        v->perf_upload_ms = 0;
        v->perf_gpu_ms_sum = 0;
        v->perf_gpu_samples = 0;
        return true;
}

static void vk_resize(RRenderer *rr, int win, u32 w, u32 h)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return;
        if (v->present_mode == VK_PRESENT_PLATFORM)
                return;    /* the platform presenter sizes per frame */
        VkWin_ *win_ = &v->win[win];
        if (!win_->ready)
                return;
        if (win_->width != w || win_->height != h) {
                win_->width = w;
                win_->height = h;
                vkwin_destroy(v, win_);
                char err[128] = { 0 };
                if (!vkwin_create(v, win, err, sizeof err))
                        LOGW("vulkan: resize failed: %s", err);
        }
}

/* ========================================================================== *
 * Per-window teardown / creation (WSI present path only)
 * ========================================================================== */

static void vkwin_destroy(RenderVK *v, VkWin_ *w)
{
        /* WSI-only resources (the swapchain machinery); platform-present
         * windows have none of these and skip straight to the images. */
        if (w->swapchain && v->dev && v->vkDeviceWaitIdle) {
                v->vkDeviceWaitIdle(v->dev);
                for (int i = 0; i < 2; i++) {
                        if (w->sync[i].fence && v->vkDestroyFence)
                                v->vkDestroyFence(v->dev, w->sync[i].fence, NULL);
                        if (w->sync[i].acq && v->vkDestroySemaphore)
                                v->vkDestroySemaphore(v->dev, w->sync[i].acq, NULL);
                        if (w->sync[i].rend && v->vkDestroySemaphore)
                                v->vkDestroySemaphore(v->dev, w->sync[i].rend, NULL);
                }
                if (v->vkDestroySwapchainKHR)
                        v->vkDestroySwapchainKHR(v->dev, w->swapchain, NULL);
        }
        /* per-window GPU resources (both present paths own these) */
        if (w->accum[0].img || w->accum[1].img || w->readback) {
                if (v->dev && v->vkDeviceWaitIdle)
                        v->vkDeviceWaitIdle(v->dev);
                vkimg_retire(v, &w->accum[0]);
                vkimg_retire(v, &w->accum[1]);
                if (w->readback) {
                        v->vkUnmapMemory(v->dev, w->readback_mem);
                        v->vkDestroyBuffer(v->dev, w->readback, NULL);
                        v->vkFreeMemory(v->dev, w->readback_mem, NULL);
                        w->readback = 0;
                        w->readback_mem = 0;
                        w->rb_map = NULL;
                        w->rb_size = 0;
                }
        }
        w->swapchain = 0;
        w->ready = false;
}

static bool vkwin_create(RenderVK *v, int idx, char *err, size_t errcap)
{
        VkWin_ *w = &v->win[idx];
        u64 native = v->plat->native_handle(v->plat, idx);
        if (!native) {
                snprintf(err, errcap, "window %d has no native handle", idx);
                return false;
        }

        /* surface */
        if (!w->surface) {
                if (v->vkCreateXcbSurfaceKHR && v->xcb_conn) {
                        VkXcbSurfaceCreateInfoKHR_ ci = { 0 };
                        ci.sType_ = ST_XCB_SURFACE_CREATE_INFO_KHR;
                        ci.connection = v->xcb_conn;
                        ci.window = (u32)native;
                        u32 r = v->vkCreateXcbSurfaceKHR(v->inst, &ci, NULL, &w->surface);
                        if (r != VK_SUCCESS_) {
                                snprintf(err, errcap, "vkCreateXcbSurfaceKHR 0x%x", r);
                                return false;
                        }
                } else if (v->vkCreateWaylandSurfaceKHR) {
                        VkWaylandSurfaceCreateInfoKHR_ ci = { 0 };
                        ci.sType_ = ST_WAYLAND_SURFACE_CREATE_INFO_KHR;
                        ci.display = v->plat->native_display(v->plat);
                        ci.surface = (void *)(uintptr_t)native;
                        if (!ci.display || !ci.surface) {
                                snprintf(err, errcap, "no wayland WSI handles");
                                return false;
                        }
                        u32 r = v->vkCreateWaylandSurfaceKHR(v->inst, &ci, NULL, &w->surface);
                        if (r != VK_SUCCESS_) {
                                snprintf(err, errcap, "vkCreateWaylandSurfaceKHR 0x%x", r);
                                return false;
                        }
                } else {
                        snprintf(err, errcap, "no WSI surface extension");
                        return false;
                }
        }

        /* re-verify present support on the right queue family */
        u32 supported = 0;
        if (v->vkGetPhysicalDeviceSurfaceSupportKHR(v->phys, v->queue_family, w->surface, &supported) != VK_SUCCESS_ || !supported) {
                snprintf(err, errcap, "queue family cannot present");
                return false;
        }

        VkSurfaceCapabilitiesKHR_ caps;
        if (v->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->phys, w->surface, &caps) != VK_SUCCESS_) {
                snprintf(err, errcap, "surface caps failed");
                return false;
        }
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT_)) {
                snprintf(err, errcap, "surface does not allow TRANSFER_DST usage");
                return false;
        }

        VkExtent2D_ ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu || ext.width == 0)
                ext.width = w->width;
        if (ext.height == 0xFFFFFFFFu || ext.height == 0)
                ext.height = w->height;

        u32 nfmt = 0;
        v->vkGetPhysicalDeviceSurfaceFormatsKHR(v->phys, w->surface, &nfmt, NULL);
        if (nfmt == 0 || nfmt > 32) {
                snprintf(err, errcap, "no surface formats");
                return false;
        }
        VkSurfaceFormatKHR_ fmts[32];
        v->vkGetPhysicalDeviceSurfaceFormatsKHR(v->phys, w->surface, &nfmt, fmts);
        /* Prefer UNORM swapchain formats: the present path BLITS the
         * RGBA8 accumulation into the swapchain image, and a blit into
         * an _SRGB destination re-encodes gamma (the engine's colors are
         * already display-encoded, like the CPU tier's bytes). BGRA8
         * ranks FIRST: the blit maps components by name (the R/B byte
         * swap is free), and BGRA8 is the fourcc every Wayland compositor
         * imports natively (dmabuf ARGB8888) while RGBA8/ABGR8888 import
         * support varies (field report #5 follow-up). SRGB-only surfaces
         * fall back to the raw byte-copy present (no blit). */
        u32 format = fmts[0].format, colorspace = fmts[0].colorSpace;
        static const u32 pref[4] = { VK_FORMAT_B8G8R8A8_UNORM_, VK_FORMAT_R8G8B8A8_UNORM_,
                                     50 /*B8G8R8A8_SRGB*/, 43 /*R8G8B8A8_SRGB*/ };
        for (u32 p = 0; p < 4; p++) {
                for (u32 i = 0; i < nfmt; i++) {
                        if (fmts[i].format == pref[p] &&
                            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR_) {
                                format = fmts[i].format;
                                colorspace = fmts[i].colorSpace;
                                goto fmt_picked;
                        }
                }
        }
fmt_picked:
        if (format != VK_FORMAT_R8G8B8A8_UNORM_ && format != VK_FORMAT_B8G8R8A8_UNORM_ &&
            format != 43 /*R8G8B8A8_SRGB*/ && format != 50 /*B8G8R8A8_SRGB*/) {
                snprintf(err, errcap, "surface format %u unsupported", format);
                return false;
        }
        w->srgb_format = (format == 43 || format == 50);
        /* the present path blits RGBA8 -> swapchain; the destination format
         * must support BLIT_DST (4-component 8-bit UNORM blits are in the
         * core-guaranteed set on every real driver — probe anyway) */
        {
                VkFormatProperties_ fp;
                memset(&fp, 0, sizeof fp);
                v->vkGetPhysicalDeviceFormatProperties(v->phys, format, &fp);
                if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT_)) {
                        snprintf(err, errcap,
                                 "swapchain format %u not blit-capable "
                                 "(REV_VK_PRESENT=platform avoids the blit)",
                                 format);
                        return false;
                }
        }

        u32 minimg = caps.minImageCount;
        if (minimg < 2) minimg = 2;
        /* two spare images beyond the minimum: FIFO swapchain images sit
         * with the compositor until a NEWER buffer latches, and with only
         * the minimum the acquire races the release cycle head-on — the
         * starvation of field report #5 (acquire slicing also covers it,
         * but headroom makes the race not exist). */
        if (caps.maxImageCount == 0 || caps.maxImageCount >= minimg + 2)
                minimg += 2;
        else if (caps.maxImageCount > minimg)
                minimg = caps.maxImageCount;

        VkSwapchainCreateInfoKHR_ sci;
        memset(&sci, 0, sizeof sci);
        sci.sType_ = ST_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = w->surface;
        sci.minImageCount = minimg;
        sci.imageFormat = format;
        sci.imageColorSpace = colorspace;
        sci.imageExtent = ext;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT_;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE_;
        sci.preTransform = caps.currentTransform ? caps.currentTransform : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR_;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR_;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR_;
        sci.clipped = 1;
        u32 r = v->vkCreateSwapchainKHR(v->dev, &sci, NULL, &w->swapchain);
        if (r != VK_SUCCESS_) {
                snprintf(err, errcap, "vkCreateSwapchainKHR 0x%x", r);
                return false;
        }

        u32 nimg = 0;
        v->vkGetSwapchainImagesKHR(v->dev, w->swapchain, &nimg, NULL);
        if (nimg > 8)
                nimg = 8;
        w->nimages = nimg;
        v->vkGetSwapchainImagesKHR(v->dev, w->swapchain, &nimg, w->images);
        w->width = ext.width;
        w->height = ext.height;
        w->format = format;

        /* sync objects */
        for (int i = 0; i < 2; i++) {
                VkSemaphoreCreateInfo_ semci = { ST_SEMAPHORE_CREATE_INFO, NULL, 0 };
                /* fences start SIGNALED so the first wait-before-record passes */
                VkFenceCreateInfo_ fenci = { ST_FENCE_CREATE_INFO, NULL, 1 };
                if (v->vkCreateSemaphore(v->dev, &semci, NULL, &w->sync[i].acq) != VK_SUCCESS_ ||
                    v->vkCreateSemaphore(v->dev, &semci, NULL, &w->sync[i].rend) != VK_SUCCESS_ ||
                    v->vkCreateFence(v->dev, &fenci, NULL, &w->sync[i].fence) != VK_SUCCESS_) {
                        snprintf(err, errcap, "sync object creation failed");
                        return false;
                }
        }
        w->sync_slot = 0;
        w->ready = true;
        return true;
}

/* ========================================================================== *
 * Teardown
 * ========================================================================== */

static void vk_fini(RRenderer *rr)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        if (v->dev && v->vkDeviceWaitIdle)
                v->vkDeviceWaitIdle(v->dev);

        /* compute tier */
        for (int k = 0; k < K_COUNT; k++) {
                if (v->pipe[k] && v->vkDestroyPipeline)
                        v->vkDestroyPipeline(v->dev, v->pipe[k], NULL);
        }
        if (v->pl && v->vkDestroyPipelineLayout)
                v->vkDestroyPipelineLayout(v->dev, v->pl, NULL);
        if (v->dsl && v->vkDestroyDescriptorSetLayout)
                v->vkDestroyDescriptorSetLayout(v->dev, v->dsl, NULL);
        if (v->samp_nearest && v->vkDestroySampler)
                v->vkDestroySampler(v->dev, v->samp_nearest, NULL);
        if (v->samp_linear && v->vkDestroySampler)
                v->vkDestroySampler(v->dev, v->samp_linear, NULL);
        if (v->tq && v->vkDestroyQueryPool)
                v->vkDestroyQueryPool(v->dev, v->tq, NULL);

        /* frame slots */
        for (int s = 0; s < VK_SLOTS; s++) {
                for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                        VFrame *fr = &v->frame[s][i];
                        if (!fr->created)
                                continue;
                        if (fr->fence && v->vkDestroyFence)
                                v->vkDestroyFence(v->dev, fr->fence, NULL);
                        if (fr->arena) {
                                if (v->vkUnmapMemory)
                                        v->vkUnmapMemory(v->dev, fr->arena_mem);
                                v->vkDestroyBuffer(v->dev, fr->arena, NULL);
                                v->vkFreeMemory(v->dev, fr->arena_mem, NULL);
                        }
                        if (fr->ubo) {
                                if (v->vkUnmapMemory)
                                        v->vkUnmapMemory(v->dev, fr->ubo_mem);
                                v->vkDestroyBuffer(v->dev, fr->ubo, NULL);
                                v->vkFreeMemory(v->dev, fr->ubo_mem, NULL);
                        }
                        if (fr->dpool && v->vkDestroyDescriptorPool)
                                v->vkDestroyDescriptorPool(v->dev, fr->dpool, NULL);
                        if (fr->cmdpool && v->vkDestroyCommandPool)
                                v->vkDestroyCommandPool(v->dev, fr->cmdpool, NULL);
                        memset(fr, 0, sizeof *fr);
                }
        }

        /* scratch / particles / video / caches / sims */
        vkimg_retire(v, &v->tex_a);
        vkimg_retire(v, &v->tex_b);
        vkimg_retire(v, &v->tex_c);
        vkimg_retire(v, &v->tex_d);
        vkimg_retire(v, &v->ptex);
        vkimg_retire(v, &v->ptarget);
        vkimg_retire(v, &v->v_y);
        vkimg_retire(v, &v->v_u);
        vkimg_retire(v, &v->v_v);
        for (int i = 0; i < VK_TEX_CACHE; i++)
                vkimg_retire(v, &v->texcache[i].im);
        for (int i = 0; i < VK_GRAD_CACHE; i++)
                vkimg_retire(v, &v->gcache[i].im);
        for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                for (int j = 0; j < VK_MAX_SIMS; j++)
                        vk_sim_free(v, &v->sims[i][j]);
                vkwin_destroy(v, &v->win[i]);
        }

        /* graveyard leftovers (device idle — safe to free now) */
        for (int i = 0; i < v->grave_n; i++) {
                VGrave *g = &v->grave[i];
                if (g->view && v->vkDestroyImageView)
                        v->vkDestroyImageView(v->dev, g->view, NULL);
                if (g->img && v->vkDestroyImage)
                        v->vkDestroyImage(v->dev, g->img, NULL);
                if (g->buf && v->vkDestroyBuffer)
                        v->vkDestroyBuffer(v->dev, g->buf, NULL);
                if (g->mem && v->vkFreeMemory)
                        v->vkFreeMemory(v->dev, g->mem, NULL);
        }
        v->grave_n = 0;

        if (v->inst && v->vkDestroySurfaceKHR) {
                for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                        if (v->win[i].surface)
                                v->vkDestroySurfaceKHR(v->inst, v->win[i].surface, NULL);
                }
        }
        if (v->dev && v->vkDestroyDevice)
                v->vkDestroyDevice(v->dev, NULL);
        if (v->inst && v->vkDestroyInstance)
                v->vkDestroyInstance(v->inst, NULL);
        if (v->xcb_conn) {
                void (*xcb_disconnect)(void *) = NULL;
                *(void **)(&xcb_disconnect) = dlsym(v->libxcb, "xcb_disconnect");
                if (xcb_disconnect)
                        xcb_disconnect(v->xcb_conn);
        }
        if (v->libxcb)
                dlclose(v->libxcb);
        if (v->lib)
                dlclose(v->lib);
        free(v);
}

RRenderer *render_vk_create(void)
{
        RRenderer *rr = (RRenderer *)calloc(1, sizeof *rr);
        RenderVK *v = (RenderVK *)calloc(1, sizeof *v);
        if (!rr || !v) {
                free(rr);
                free(v);
                return NULL;
        }
        rr->name = "vulkan";
        rr->tier = 3;
        rr->wants_layers = true;    /* GPU compute compositing tier        */
        rr->ctx = v;
        rr->init = vk_init;
        rr->fini = vk_fini;
        rr->begin = vk_begin;
        rr->layer = vk_layer;
        rr->end = vk_end;
        rr->resize = vk_resize;
        rr->perf = vk_perf;
        return rr;
}

/* ========================================================================== *
 * Init: instance -> device -> compute tier -> capability flags
 * ========================================================================== */

static bool vk_init(RRenderer *rr, RPlatform *p, char *err, size_t errcap)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        v->plat = p;

        if (!p->has_gpu) {
                snprintf(err, errcap, "platform '%s' has no GPU path", p->name);
                return false;
        }
        if (!p->native_handle(p, 0)) {
                snprintf(err, errcap, "no window 0 (windows must exist before renderer init)");
                return false;
        }

        v->lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!v->lib) {
                snprintf(err, errcap, "libvulkan.so.1 not loadable: %s", dlerror());
                return false;
        }
        *(void **)&v->gipa = dlsym(v->lib, "vkGetInstanceProcAddr");
        if (!v->gipa) {
                snprintf(err, errcap, "vkGetInstanceProcAddr missing");
                return false;
        }
        *(void **)&v->vkCreateInstance = (void *)v->gipa(NULL, "vkCreateInstance");
        *(void **)&v->vkEnumerateInstanceExtensionProperties = (void *)v->gipa(NULL, "vkEnumerateInstanceExtensionProperties");
        if (!v->vkCreateInstance || !v->vkEnumerateInstanceExtensionProperties) {
                snprintf(err, errcap, "instance entry points missing");
                return false;
        }

        /* which WSI surface extension can we use? */
        bool is_wayland = strcmp(p->name, "wayland") == 0;
        const char *surface_ext = NULL;
        {
                u32 n = 0;
                v->vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
                VkExtensionProperties_ exts[64];
                if (n > 64) n = 64;
                v->vkEnumerateInstanceExtensionProperties(NULL, &n, exts);
                bool have_surface = false, have_xcb = false, have_xlib = false, have_wl = false;
                for (u32 i = 0; i < n; i++) {
                        if (strcmp(exts[i].name, "VK_KHR_surface") == 0) have_surface = true;
                        if (strcmp(exts[i].name, "VK_KHR_xcb_surface") == 0) have_xcb = true;
                        if (strcmp(exts[i].name, "VK_KHR_xlib_surface") == 0) have_xlib = true;
                        if (strcmp(exts[i].name, "VK_KHR_wayland_surface") == 0) have_wl = true;
                }
                if (!have_surface) {
                        snprintf(err, errcap, "VK_KHR_surface not available");
                        return false;
                }
                if (is_wayland) {
                        if (have_wl) surface_ext = "VK_KHR_wayland_surface";
                } else {
                        if (have_xcb) surface_ext = "VK_KHR_xcb_surface";
                        else if (have_xlib) surface_ext = "VK_KHR_xlib_surface";
                }
                if (!surface_ext) {
                        snprintf(err, errcap, "no WSI surface extension for platform '%s'", p->name);
                        return false;
                }
        }

        /* instance */
        VkApplicationInfo_ app;
        memset(&app, 0, sizeof app);
        app.sType_ = ST_APPLICATION_INFO;
        app.appName = "reverie";
        app.appVersion = 1;
        app.engineName = "Reverie Engine";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_0_;
        const char *exts[2] = { "VK_KHR_surface", surface_ext };
        /* REV_VK_VALIDATE=1: opt-in validation layer (debugging aid — the
         * layer must be installed; its diagnostics go to stderr/stdout via
         * the loader). Off by default: layers change timing and can spew
         * on the hand-vendored minimal structs. */
        const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
        VkInstanceCreateInfo_ ici;
        memset(&ici, 0, sizeof ici);
        ici.sType_ = ST_INSTANCE_CREATE_INFO;
        ici.pAppInfo = &app;
        if (getenv("REV_VK_VALIDATE") && getenv("REV_VK_VALIDATE")[0] != '0') {
                ici.enabledLayerCount = 1;
                ici.ppEnabledLayerNames = layers;
                LOGI("vulkan: validation layer requested (REV_VK_VALIDATE=1) "
                     "— createInstance failure means it is not installed");
        }
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = exts;
        u32 r = v->vkCreateInstance(&ici, NULL, &v->inst);
        if (r != VK_SUCCESS_) {
                snprintf(err, errcap, "vkCreateInstance 0x%x", r);
                return false;
        }

#define IPSYM(field, name) *(void **)(&v->field) = (void *)v->gipa((void *)v->inst, name)
        IPSYM(gdpa, "vkGetDeviceProcAddr");
        IPSYM(vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
        IPSYM(vkCreateDevice, "vkCreateDevice");
        IPSYM(vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
        IPSYM(vkGetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR");
        IPSYM(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        IPSYM(vkGetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        IPSYM(vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
        IPSYM(vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
        IPSYM(vkGetPhysicalDeviceFormatProperties, "vkGetPhysicalDeviceFormatProperties");
        IPSYM(vkCreateXcbSurfaceKHR, "vkCreateXcbSurfaceKHR");
        IPSYM(vkCreateXlibSurfaceKHR, "vkCreateXlibSurfaceKHR");
        IPSYM(vkCreateWaylandSurfaceKHR, "vkCreateWaylandSurfaceKHR");
        IPSYM(vkDestroySurfaceKHR, "vkDestroySurfaceKHR");
        IPSYM(vkDestroyInstance, "vkDestroyInstance");
#undef IPSYM

        /* X11 WSI transport: dlopen libxcb and open a connection (XIDs are
         * server-global, so windows created by our raw client are usable). */
        if (!is_wayland) {
                if (v->vkCreateXcbSurfaceKHR) {
                        v->libxcb = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
                        if (v->libxcb) {
                                void *(*xcb_connect)(const char *, int *) = NULL;
                                void (*xcb_disconnect)(void *) = NULL;
                                *(void **)(&xcb_connect) = dlsym(v->libxcb, "xcb_connect");
                                *(void **)(&xcb_disconnect) = dlsym(v->libxcb, "xcb_disconnect");
                                if (xcb_connect) {
                                        v->xcb_conn = xcb_connect(NULL, NULL);
                                        if (!v->xcb_conn) {
                                                snprintf(err, errcap, "xcb_connect failed");
                                                return false;
                                        }
                                        (void)xcb_disconnect;
                                }
                        }
                }
                if (!v->xcb_conn && !v->vkCreateXlibSurfaceKHR) {
                        snprintf(err, errcap, "no usable X11 WSI transport (libxcb missing)");
                        return false;
                }
        }

        /* physical device + queue family with present support on window 0's
         * future surface — create a temporary surface first */
        VkSurfaceKHR_ probe = 0;
        u64 native0 = p->native_handle(p, 0);
        if (v->vkCreateXcbSurfaceKHR && v->xcb_conn) {
                VkXcbSurfaceCreateInfoKHR_ ci = { 0 };
                ci.sType_ = ST_XCB_SURFACE_CREATE_INFO_KHR;
                ci.connection = v->xcb_conn;
                ci.window = (u32)native0;
                r = v->vkCreateXcbSurfaceKHR(v->inst, &ci, NULL, &probe);
        } else if (v->vkCreateWaylandSurfaceKHR) {
                VkWaylandSurfaceCreateInfoKHR_ ci = { 0 };
                ci.sType_ = ST_WAYLAND_SURFACE_CREATE_INFO_KHR;
                ci.display = p->native_display(p);
                ci.surface = (void *)(uintptr_t)native0;
                r = (ci.display && ci.surface) ? v->vkCreateWaylandSurfaceKHR(v->inst, &ci, NULL, &probe) : 0x7fffffff;
        }
        if (r != VK_SUCCESS_ || !probe) {
                snprintf(err, errcap, "probe surface creation failed 0x%x", r);
                return false;
        }

        u32 ndev = 0;
        u32 enumrc = v->vkEnumeratePhysicalDevices(v->inst, &ndev, NULL);
        if (enumrc != VK_SUCCESS_ || ndev == 0) {
                snprintf(err, errcap, "no physical devices (vkEnumeratePhysicalDevices 0x%x)",
                         enumrc);
                return false;
        }
        if (ndev > 16)
                ndev = 16;
        VkPhysicalDevice_ devs[16];
        if (v->vkEnumeratePhysicalDevices(v->inst, &ndev, devs) != VK_SUCCESS_) {
                snprintf(err, errcap, "physical device enumeration failed");
                return false;
        }

        /* Deterministic device selection: enumerate EVERY device, keep the
         * ones with a GRAPHICS+COMPUTE queue family that can present to this
         * surface with TRANSFER_DST swapchains, and pick by hardware class
         * (discrete > integrated > virtual > other > CPU). */
        bool found = false;
        int best_score = -1;
        u32 best_dev = 0, best_fam = 0, best_tsbits = 0;
        char best_name[256] = { 0 };
        u32 best_type = 0, best_api = 0, best_drv = 0;
        char logbuf[512];
        size_t lo = 0;
        lo += (size_t)snprintf(logbuf + lo, sizeof logbuf - lo,
                               "vulkan: %u device(s):", ndev);
        for (u32 d = 0; d < ndev; d++) {
                VkPhysicalDeviceProperties_ props;
                memset(&props, 0, sizeof props);
                if (v->vkGetPhysicalDeviceProperties)
                        v->vkGetPhysicalDeviceProperties(devs[d], &props);
                const char *tname =
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_ ? "discrete" :
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_ ? "integrated" :
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_ ? "virtual" :
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU_ ? "CPU (software)" :
                        "other";
                int score =
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_ ? 4 :
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_ ? 3 :
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_ ? 2 :
                        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU_ ? 0 : 1;
                lo += (size_t)snprintf(logbuf + lo, sizeof logbuf - lo,
                                       "\n  [%u] %s (%s)", d, props.deviceName, tname);

                u32 nfam = 0;
                v->vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nfam, NULL);
                if (nfam == 0 || nfam > 64)
                        continue;
                VkQueueFamilyProperties_ fams[64];
                v->vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nfam, fams);
                for (u32 f = 0; f < nfam; f++) {
                        /* the compute tier needs GRAPHICS+COMPUTE on the
                         * presenting family (universal on real devices) */
                        if ((fams[f].queueFlags &
                             (VK_QUEUE_GRAPHICS_BIT_ | VK_QUEUE_COMPUTE_BIT_)) !=
                            (VK_QUEUE_GRAPHICS_BIT_ | VK_QUEUE_COMPUTE_BIT_))
                                continue;
                        u32 sup = 0;
                        if (v->vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], f, probe, &sup) != VK_SUCCESS_ || !sup)
                                continue;
                        VkSurfaceCapabilitiesKHR_ caps;
                        memset(&caps, 0, sizeof caps);
                        if (v->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(devs[d], probe, &caps) != VK_SUCCESS_)
                                continue;
                        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT_))
                                continue;
                        /* presentable AND compute-capable: eligible */
                        if (score > best_score) {
                                best_score = score;
                                best_dev = d;
                                best_fam = f;
                                best_tsbits = fams[f].timestampValidBits;
                                snprintf(best_name, sizeof best_name, "%s", props.deviceName);
                                best_type = props.deviceType;
                                best_api = props.apiVersion;
                                best_drv = props.driverVersion;
                        }
                        break;    /* one qualifying family per device is enough */
                }
        }
        LOGI("%s", logbuf);
        if (best_score >= 0) {
                v->phys = devs[best_dev];
                v->queue_family = best_fam;
                v->ts_valid_bits = best_tsbits;
                /* honest hardware/software classification + driver report */
                v->dev_is_software = (best_type == VK_PHYSICAL_DEVICE_TYPE_CPU_);
                v->dev_type = (int)best_type;
                snprintf(v->dev_name, sizeof v->dev_name, "%s", best_name);
                v->api_version = best_api;
                v->driver_version = best_drv;
                found = true;
        }
        if (found) {
                rr->software = v->dev_is_software;
                {
                        const char *cls =
                                v->dev_is_software ? "software" :
                                v->dev_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_ ? "discrete" :
                                v->dev_type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_ ? "integrated" :
                                v->dev_type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_ ? "virtual" :
                                "other";
                        /* bounded name copy: %.*s keeps the format-truncation
                         * analysis happy and long device names simply clip */
                        int prec = (int)sizeof rr->device_desc - 48;
                        if (prec < 8)
                                prec = 8;
                        snprintf(rr->device_desc, sizeof rr->device_desc,
                                 "%.*s (%s, Vulkan %u.%u)", prec, v->dev_name, cls,
                                 v->api_version >> 22,
                                 (v->api_version >> 12) & 0x3ff);
                }
                const char *tname =
                        best_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_ ? "discrete GPU" :
                        best_type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_ ? "integrated GPU" :
                        best_type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_ ? "virtual GPU" :
                        best_type == VK_PHYSICAL_DEVICE_TYPE_CPU_ ? "CPU device (SOFTWARE)" :
                        "GPU";
                LOGI("vulkan: selected device \"%s\" (%s, Vulkan %u.%u.%u, driver %u.%u.%u)",
                     best_name, tname,
                     best_api >> 22, (best_api >> 12) & 0x3ff, best_api & 0xfff,
                     best_drv >> 22, (best_drv >> 12) & 0x3ff, best_drv & 0xfff);
                if (v->dev_is_software)
                        LOGW("vulkan: SOFTWARE device selected — rendering will NOT be "
                             "hardware accelerated (CPU-type Vulkan device, e.g. lavapipe)");
        }

        if (!found) {
                snprintf(err, errcap, "no device with a presentable GRAPHICS+COMPUTE queue");
                return false;
        }

        /* create the logical device on the SELECTED device */
        {
                VkDeviceQueueCreateInfo_ qci;
                memset(&qci, 0, sizeof qci);
                qci.sType_ = ST_DEVICE_QUEUE_CREATE_INFO;
                qci.queueFamilyIndex = v->queue_family;
                qci.queueCount = 1;
                f32 prio = 1.0f;
                qci.pQueuePriorities = &prio;
                const char *devexts[1] = { "VK_KHR_swapchain" };
                VkDeviceCreateInfo_ dci;
                memset(&dci, 0, sizeof dci);
                dci.sType_ = ST_DEVICE_CREATE_INFO;
                dci.queueCreateInfoCount = 1;
                dci.pQueueCreateInfos = &qci;
                dci.enabledExtensionCount = 1;
                dci.ppEnabledExtensionNames = devexts;
                static u32 zerofeatures[55];    /* VkPhysicalDeviceFeatures, all-off:
                                                   compute needs no features */
                dci.pEnabledFeatures = zerofeatures;
                u32 drc = v->vkCreateDevice(v->phys, &dci, NULL, &v->dev);
                if (drc != VK_SUCCESS_) {
                        snprintf(err, errcap, "vkCreateDevice 0x%x on '%s'",
                                 drc, v->dev_name);
                        return false;
                }
        }

#define DSYM(field, name) *(void **)(&v->field) = (void *)v->gdpa((void *)v->dev, name)
        DSYM(vkGetDeviceQueue, "vkGetDeviceQueue");
        DSYM(vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
        DSYM(vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
        DSYM(vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
        DSYM(vkCreateCommandPool, "vkCreateCommandPool");
        DSYM(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
        DSYM(vkResetCommandPool, "vkResetCommandPool");
        DSYM(vkCreateBuffer, "vkCreateBuffer");
        DSYM(vkCreateImage, "vkCreateImage");
        DSYM(vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
        DSYM(vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
        DSYM(vkAllocateMemory, "vkAllocateMemory");
        DSYM(vkMapMemory, "vkMapMemory");
        DSYM(vkUnmapMemory, "vkUnmapMemory");
        DSYM(vkInvalidateMappedMemoryRanges, "vkInvalidateMappedMemoryRanges");
        DSYM(vkBindBufferMemory, "vkBindBufferMemory");
        DSYM(vkBindImageMemory, "vkBindImageMemory");
        DSYM(vkCreateFence, "vkCreateFence");
        DSYM(vkCreateSemaphore, "vkCreateSemaphore");
        DSYM(vkWaitForFences, "vkWaitForFences");
        DSYM(vkResetFences, "vkResetFences");
        DSYM(vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
        DSYM(vkResetCommandBuffer, "vkResetCommandBuffer");
        DSYM(vkBeginCommandBuffer, "vkBeginCommandBuffer");
        DSYM(vkEndCommandBuffer, "vkEndCommandBuffer");
        DSYM(vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
        DSYM(vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage");
        DSYM(vkCmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
        DSYM(vkCmdBlitImage, "vkCmdBlitImage");
        DSYM(vkCmdClearColorImage, "vkCmdClearColorImage");
        DSYM(vkQueueSubmit, "vkQueueSubmit");
        DSYM(vkQueuePresentKHR, "vkQueuePresentKHR");
        DSYM(vkDestroyFence, "vkDestroyFence");
        DSYM(vkDestroySemaphore, "vkDestroySemaphore");
        DSYM(vkDestroyBuffer, "vkDestroyBuffer");
        DSYM(vkDestroyImage, "vkDestroyImage");
        DSYM(vkDestroyImageView, "vkDestroyImageView");
        DSYM(vkFreeMemory, "vkFreeMemory");
        DSYM(vkDestroyCommandPool, "vkDestroyCommandPool");
        DSYM(vkDestroyDevice, "vkDestroyDevice");
        DSYM(vkDeviceWaitIdle, "vkDeviceWaitIdle");
        DSYM(vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
        DSYM(vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
        DSYM(vkCreateDescriptorPool, "vkCreateDescriptorPool");
        DSYM(vkDestroyDescriptorPool, "vkDestroyDescriptorPool");
        DSYM(vkResetDescriptorPool, "vkResetDescriptorPool");
        DSYM(vkAllocateDescriptorSets, "vkAllocateDescriptorSets");
        DSYM(vkUpdateDescriptorSets, "vkUpdateDescriptorSets");
        DSYM(vkCreateSampler, "vkCreateSampler");
        DSYM(vkDestroySampler, "vkDestroySampler");
        DSYM(vkCreateImageView, "vkCreateImageView");
        DSYM(vkCreateShaderModule, "vkCreateShaderModule");
        DSYM(vkDestroyShaderModule, "vkDestroyShaderModule");
        DSYM(vkCreatePipelineLayout, "vkCreatePipelineLayout");
        DSYM(vkDestroyPipelineLayout, "vkDestroyPipelineLayout");
        DSYM(vkCreateComputePipelines, "vkCreateComputePipelines");
        DSYM(vkDestroyPipeline, "vkDestroyPipeline");
        DSYM(vkCmdBindPipeline, "vkCmdBindPipeline");
        DSYM(vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        DSYM(vkCmdDispatch, "vkCmdDispatch");
        DSYM(vkCreateQueryPool, "vkCreateQueryPool");
        DSYM(vkDestroyQueryPool, "vkDestroyQueryPool");
        DSYM(vkGetQueryPoolResults, "vkGetQueryPoolResults");
        DSYM(vkCmdResetQueryPool, "vkCmdResetQueryPool");
        DSYM(vkCmdWriteTimestamp, "vkCmdWriteTimestamp");
#undef DSYM

        v->vkGetDeviceQueue(v->dev, v->queue_family, 0, &v->queue);

        /* ---- format probes: what this device's images can actually do --- */
        {
                VkFormatProperties_ fp;
                memset(&fp, 0, sizeof fp);
                v->vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_R8G8B8A8_UNORM_, &fp);
                if ((fp.optimalTilingFeatures &
                     (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT_ | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT_)) !=
                    (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT_ | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT_)) {
                        snprintf(err, errcap,
                                 "device lacks RGBA8 storage images — compute "
                                 "compositing impossible on this device");
                        return false;
                }
                memset(&fp, 0, sizeof fp);
                v->vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_R16G16B16A16_SFLOAT_, &fp);
                v->fmt16_ok = (fp.optimalTilingFeatures &
                               (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT_ | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT_)) ==
                              (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT_ | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT_);
                memset(&fp, 0, sizeof fp);
                v->vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_R32G32B32A32_SFLOAT_, &fp);
                v->fmt32_ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT_) != 0;
        }

        /* ---- samplers ----------------------------------------------------- */
        {
                VkSamplerCreateInfo_ sci;
                memset(&sci, 0, sizeof sci);
                sci.sType_ = ST_SAMPLER_CREATE_INFO;
                sci.magFilter = VK_FILTER_NEAREST_;
                sci.minFilter = VK_FILTER_NEAREST_;
                sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE_;
                sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE_;
                sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE_;
                sci.maxLod = 0.0f;      /* lod 0 only; maxLod=0 keeps sampling exact */
                if (v->vkCreateSampler(v->dev, &sci, NULL, &v->samp_nearest) != VK_SUCCESS_) {
                        snprintf(err, errcap, "vkCreateSampler(nearest) failed");
                        return false;
                }
                sci.magFilter = VK_FILTER_LINEAR_;
                sci.minFilter = VK_FILTER_LINEAR_;
                if (v->vkCreateSampler(v->dev, &sci, NULL, &v->samp_linear) != VK_SUCCESS_) {
                        snprintf(err, errcap, "vkCreateSampler(linear) failed");
                        return false;
                }
        }

        /* ---- descriptor set layout (shared by every kernel) ---------------
         * binding 0: dynamic uniform buffer (per-dispatch params)
         * bindings 1..4: combined image samplers (nearest or linear sampler
         *                chosen per slot at write time — no shader branch)
         * binding 7: rgba8 storage image (destination)
         * bindings 8, 10: rgba16f storage images (sim field reads)
         * binding 9: rgba16f storage image (sim field write)              */
        {
                VkDescriptorSetLayoutBinding_ binds[10];
                memset(binds, 0, sizeof binds);
                binds[0].binding = 0;
                binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC_;
                binds[0].descriptorCount = 1;
                binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT_;
                for (u32 b = 1; b <= 4; b++) {
                        binds[b].binding = b;
                        binds[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER_;
                        binds[b].descriptorCount = 1;
                        binds[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT_;
                }
                binds[5].binding = 7;
                binds[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE_;
                binds[5].descriptorCount = 1;
                binds[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT_;
                binds[6].binding = 8;
                binds[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE_;
                binds[6].descriptorCount = 1;
                binds[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT_;
                binds[7].binding = 9;
                binds[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE_;
                binds[7].descriptorCount = 1;
                binds[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT_;
                binds[8].binding = 10;
                binds[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE_;
                binds[8].descriptorCount = 1;
                binds[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT_;
                VkDescriptorSetLayoutCreateInfo_ dli;
                memset(&dli, 0, sizeof dli);
                dli.sType_ = ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                dli.bindingCount = 9;
                dli.pBindings = binds;
                if (v->vkCreateDescriptorSetLayout(v->dev, &dli, NULL, &v->dsl) != VK_SUCCESS_) {
                        snprintf(err, errcap, "vkCreateDescriptorSetLayout failed");
                        return false;
                }
                VkPipelineLayoutCreateInfo_ pli;
                memset(&pli, 0, sizeof pli);
                pli.sType_ = ST_PIPELINE_LAYOUT_CREATE_INFO;
                pli.setLayoutCount = 1;
                pli.pSetLayouts = &v->dsl;
                if (v->vkCreatePipelineLayout(v->dev, &pli, NULL, &v->pl) != VK_SUCCESS_) {
                        snprintf(err, errcap, "vkCreatePipelineLayout failed");
                        return false;
                }
        }

        /* ---- compute pipelines from the embedded SPIR-V ------------------- */
        {
#define SPV_WORDS(a) ((u32)(sizeof(a) / sizeof((a)[0])))
                const struct { const u32 *code; u32 words; } spv[K_COUNT] = {
                        { spv_composite, SPV_WORDS(spv_composite) },
                        { spv_copypremul, SPV_WORDS(spv_copypremul) },
                        { spv_blur, SPV_WORDS(spv_blur) },
                        { spv_gradient, SPV_WORDS(spv_gradient) },
                        { spv_particles, SPV_WORDS(spv_particles) },
                        { spv_video, SPV_WORDS(spv_video) },
                        { spv_decode, SPV_WORDS(spv_decode) },
                        { spv_ripple_splat, SPV_WORDS(spv_ripple_splat) },
                        { spv_ripple_step, SPV_WORDS(spv_ripple_step) },
                        { spv_ripple_composite, SPV_WORDS(spv_ripple_composite) },
                        { spv_fluid_velf, SPV_WORDS(spv_fluid_velf) },
                        { spv_fluid_dyef, SPV_WORDS(spv_fluid_dyef) },
                        { spv_fluid_curl, SPV_WORDS(spv_fluid_curl) },
                        { spv_fluid_vort, SPV_WORDS(spv_fluid_vort) },
                        { spv_fluid_div, SPV_WORDS(spv_fluid_div) },
                        { spv_fluid_jacobi, SPV_WORDS(spv_fluid_jacobi) },
                        { spv_fluid_gradsub, SPV_WORDS(spv_fluid_gradsub) },
                        { spv_fluid_advect_vel, SPV_WORDS(spv_fluid_advect_vel) },
                        { spv_fluid_advect_dye, SPV_WORDS(spv_fluid_advect_dye) },
                        { spv_fluid_tonemap, SPV_WORDS(spv_fluid_tonemap) },
                };
                for (int k = 0; k < K_COUNT; k++) {
                        VkShaderModuleCreateInfo_ smi;
                        memset(&smi, 0, sizeof smi);
                        smi.sType_ = ST_SHADER_MODULE_CREATE_INFO;
                        smi.codeSize = (u64)spv[k].words * 4;
                        smi.pCode = spv[k].code;
                        VkShaderModule_ mod = 0;
                        if (v->vkCreateShaderModule(v->dev, &smi, NULL, &mod) != VK_SUCCESS_) {
                                snprintf(err, errcap, "vkCreateShaderModule(kernel %d) failed", k);
                                return false;
                        }
                        VkComputePipelineCreateInfo_ cpi;
                        memset(&cpi, 0, sizeof cpi);
                        cpi.sType_ = ST_COMPUTE_PIPELINE_CREATE_INFO;
                        cpi.stage.sType_ = 18;    /* VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO */
                        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT_;
                        cpi.stage.module = mod;
                        cpi.stage.pName = "main";
                        cpi.layout = v->pl;
                        u32 pr = v->vkCreateComputePipelines(v->dev, 0, 1, &cpi, NULL, &v->pipe[k]);
                        v->vkDestroyShaderModule(v->dev, mod, NULL);
                        if (pr != VK_SUCCESS_) {
                                snprintf(err, errcap, "vkCreateComputePipelines(kernel %d) 0x%x",
                                         k, pr);
                                return false;
                        }
                }
        }

        /* ---- GPU timestamp queries (honest REV_PROFILE gpu= reporting) --- */
        {
                VkPhysicalDeviceProperties_ props;
                memset(&props, 0, sizeof props);
                v->vkGetPhysicalDeviceProperties(v->phys, &props);
                f32 period = 0;
                memcpy(&period, (const u8 *)&props + VKPD_TIMESTAMP_PERIOD_OFF_, sizeof period);
                u32 cag = 0;
                memcpy(&cag, (const u8 *)&props + VKPD_TIMESTAMP_CAG_OFF_, sizeof cag);
                v->ts_period = (f64)period;
                if (v->ts_valid_bits > 0 && cag && period > 0.0f && period < 100000.0f) {
                        VkQueryPoolCreateInfo_ qpi;
                        memset(&qpi, 0, sizeof qpi);
                        qpi.sType_ = ST_QUERY_POOL_CREATE_INFO;
                        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP_;
                        qpi.queryCount = VK_SLOTS * REV_PLATFORM_MAX_OUTPUTS * 2;
                        if (v->vkCreateQueryPool(v->dev, &qpi, NULL, &v->tq) == VK_SUCCESS_) {
                                v->tq_ok = true;
                                LOGI("vulkan: GPU timer queries available (timestampValidBits=%u, "
                                     "period=%.1fns) — REV_PROFILE reports async GPU timings",
                                     v->ts_valid_bits, (double)period);
                        }
                } else {
                        LOGI("vulkan: GPU timer queries unavailable (validBits=%u, "
                             "period=%.1f) — REV_PROFILE gpu= will read 0.0",
                             v->ts_valid_bits, (double)period);
                }
        }

        /* ---- capability flags (settled here, never after) ------------------
         * REV_VK_SIMS_SW=1: run the GPU sim/composite paths on SOFTWARE
         * devices (lavapipe) for regression testing — production keeps
         * sims hardware-only, mirroring the GL tier's REV_GL_FLUID_SW. */
        const bool sims_sw = getenv("REV_VK_SIMS_SW") != NULL;
        rr->wants_layers = true;
        rr->wants_sims = v->fmt16_ok && (!v->dev_is_software || sims_sw);
        rr->wants_points = v->fmt32_ok && (!v->dev_is_software || sims_sw);
        rr->wants_grads = !v->dev_is_software || sims_sw;
        rr->wants_video = true;

        if (rr->wants_sims)
                LOGI("vulkan: fluid + ripple solvers on GPU (RGBA16F storage images)");
        else if (!v->dev_is_software)
                LOGW("vulkan: RGBA16F storage images unsupported — GPU sims "
                     "declined, ripple scenes fall back to CPU compositing");
        LOGI("vulkan tier capability: GPU COMPUTE COMPOSITING — every layer "
             "op (blend/blur/gradient/particles/video/sims) runs as a "
             "compute kernel; the CPU never touches a per-pixel loop");

        /* ---- present-path selection (0.1.1 semantics, GPU-fed) ---------- */
        {
                const char *pm = getenv("REV_VK_PRESENT");
                v->present_mode = (pm && strcmp(pm, "wsi") == 0) ? VK_PRESENT_WSI
                                                                 : VK_PRESENT_PLATFORM;
        }
        if (v->present_mode == VK_PRESENT_PLATFORM) {
                if (!p->present_cpu) {
                        snprintf(err, errcap, "platform has no CPU present path");
                        return false;
                }
                /* the probe surface has served its purpose (device selection);
                 * the platform presenter owns the windows now */
                if (probe) {
                        v->vkDestroySurfaceKHR(v->inst, probe, NULL);
                }
                LOGI("vulkan: device present-capable (queue family %u); finished "
                     "frames download through HOST_CACHED staging — the "
                     "premultiply AND the wl_shm BGRA byte swap run as one GPU "
                     "kernel and the presenter does one memcpy — and present "
                     "via the platform SHM path — set REV_VK_PRESENT=wsi for "
                     "zero-copy swapchain present",
                     v->queue_family);
                return true;
        }

        /* the probe surface becomes window 0's surface */
        v->win[0].surface = probe;
        {
                RRect g;
                if (p->win_geometry(p, 0, &g)) {
                        v->win[0].width = (u32)g.w;
                        v->win[0].height = (u32)g.h;
                } else {
                        v->win[0].width = 1280;
                        v->win[0].height = 720;
                }
        }
        if (!vkwin_create(v, 0, err, errcap))
                return false;

        /* remaining windows */
        for (int i = 1; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                if (!p->native_handle(p, i))
                        continue;
                RRect g;
                v->win[i].width = 1280;
                v->win[i].height = 720;
                if (p->win_geometry(p, i, &g)) {
                        v->win[i].width = (u32)g.w;
                        v->win[i].height = (u32)g.h;
                }
                char sub[128] = { 0 };
                if (!vkwin_create(v, i, sub, sizeof sub))
                        LOGW("vulkan: window %d skipped (%s)", i, sub);
        }

        LOGI("vulkan: swapchain present path ready (queue family %u, format %s, "
             "%u images, BGRA-first) — acquire is slice-pumped so a slow "
             "compositor can stall a frame but never wedge the loop%s",
             v->queue_family,
             v->win[0].format == VK_FORMAT_R8G8B8A8_UNORM_ ? "RGBA8" : "BGRA8",
             v->win[0].nimages,
             p->wsi_frame_request
                 ? "; every present arms a frame callback so the idle "
                   "power-save pauses rendering when nothing latches"
                 : "");
        return true;
}
