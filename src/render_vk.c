/* ============================================================================
 * Reverie Engine — render_vk.c
 * Vulkan renderer: swapchain presentation of engine-composited frames.
 *
 * Everything is loaded at runtime — libvulkan via dlopen, and the X11 WSI
 * via dlopen'd libxcb (no Xlib, no libxcb linkage). Wayland WSI uses the
 * optional wlshim (libwayland-client dlopen). Structure types, enum values
 * and struct layouts below are vendored by hand from the official
 * Khronos Vulkan headers (verified against vulkan_core.h 1.4.x).
 *
 * v0.1.0 scope: the Vulkan tier owns *presentation* — the engine composites
 * on the CPU, the frame is uploaded once to a host-visible staging buffer
 * and copied into the swapchain image with image barriers (no shaders, no
 * render passes). Full GPU compositing via compute is planned for 0.2.
 * If the surface does not allow TRANSFER_DST usage, init fails and the
 * chain falls through to the OpenGL tier.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "render_vk.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "platform.h"

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
typedef u32 VkFlags32;

enum {
        VK_SUCCESS_ = 0,
        VK_TIMEOUT_ = 2,
        VK_INCOMPLETE_ = 5,
        VK_SUBOPTIMAL_KHR_ = 1000001003,
        VK_ERROR_OUT_OF_DATE_KHR_ = -1000001004,
};

enum {
        ST_APPLICATION_INFO = 0,
        ST_INSTANCE_CREATE_INFO = 1,
        ST_DEVICE_QUEUE_CREATE_INFO = 2,
        ST_DEVICE_CREATE_INFO = 3,
        ST_SUBMIT_INFO = 4,
        ST_MEMORY_ALLOCATE_INFO = 5,
        ST_FENCE_CREATE_INFO = 8,
        ST_SEMAPHORE_CREATE_INFO = 9,
        ST_BUFFER_CREATE_INFO = 12,
        ST_IMAGE_CREATE_INFO = 14,
        ST_COMMAND_BUFFER_ALLOCATE_INFO = 40,
        ST_COMMAND_BUFFER_BEGIN_INFO = 42,
        ST_IMAGE_MEMORY_BARRIER = 45,
        ST_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
        ST_PRESENT_INFO_KHR = 1000001001,
        ST_XLIB_SURFACE_CREATE_INFO_KHR = 1000004000,
        ST_XCB_SURFACE_CREATE_INFO_KHR = 1000005000,
        ST_WAYLAND_SURFACE_CREATE_INFO_KHR = 1000006000,
};

#define VK_QUEUE_GRAPHICS_BIT_ 0x1u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT_ 0x2u
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_ 0x1u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ 0x2u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_ 0x4u
#define VK_ACCESS_TRANSFER_WRITE_BIT_ 0x1000u
#define VK_ACCESS_TRANSFER_READ_BIT_ 0x800u
#define VK_ACCESS_MEMORY_READ_BIT_ 0x8000u
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT_ 0x1u
#define VK_PIPELINE_STAGE_TRANSFER_BIT_ 0x1000u
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT_ 0x2000u
#define VK_IMAGE_ASPECT_COLOR_BIT_ 0x1u
#define VK_IMAGE_LAYOUT_UNDEFINED_ 0
#define VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_ 7
#define VK_IMAGE_LAYOUT_PRESENT_SRC_KHR_ 1000001002
#define VK_FORMAT_R8G8B8A8_UNORM_ 37
#define VK_FORMAT_B8G8R8A8_UNORM_ 44
#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR_ 0
#define VK_PRESENT_MODE_FIFO_KHR_ 2
#define VK_IMAGE_TILING_OPTIMAL_ 0
#define VK_IMAGE_TYPE_2D_ 1
#define VK_SAMPLE_COUNT_1_BIT_ 0x1u
#define VK_SHARING_MODE_EXCLUSIVE_ 0
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT_ 0x1u
#define VK_WHOLE_SIZE_ (~(u64)0)
#define VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR_ 0x1u
#define VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR_ 0x1u
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY_ 0
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT_ 0x1u
#define VK_API_VERSION_1_0_ 0x400000u /* (1<<22)|(0<<12)|0 */
#define VK_MAX_MEMORY_TYPES_ 32
#define VK_MAX_MEMORY_HEAPS_ 16

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
typedef u32 (*PFN_createBuffer_)(VkDevice_, const VkBufferCreateInfo_ *, const void *, VkBuffer_ *);
typedef void (*PFN_getPhysDevMemProps_)(VkPhysicalDevice_, VkPhysicalDeviceMemoryProperties_ *);
typedef u32 (*PFN_allocMemory_)(VkDevice_, const VkMemoryAllocateInfo_ *, const void *, VkDeviceMemory_ *);
typedef u32 (*PFN_mapMemory_)(VkDevice_, VkDeviceMemory_, u64, u64, u32, void **);
typedef u32 (*PFN_bindBufferMemory_)(VkDevice_, VkBuffer_, VkDeviceMemory_, u64);
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
typedef u32 (*PFN_queueSubmit_)(VkQueue_, u32, const VkSubmitInfo_ *, VkFence_);
typedef u32 (*PFN_queuePresent_)(VkQueue_, const VkPresentInfoKHR_ *);
typedef void (*PFN_destroyFence_)(VkDevice_, VkFence_, const void *);
typedef void (*PFN_destroySemaphore_)(VkDevice_, VkSemaphore_, const void *);
typedef void (*PFN_destroyBuffer_)(VkDevice_, VkBuffer_, const void *);
typedef void (*PFN_freeMemory_)(VkDevice_, VkDeviceMemory_, const void *);
typedef void (*PFN_destroyCommandPool_)(VkDevice_, VkCommandPool_, const void *);
typedef void (*PFN_destroySurface_)(VkInstance_, VkSurfaceKHR_, const void *);
typedef void (*PFN_destroyDevice_)(VkDevice_, const void *);
typedef void (*PFN_destroyInstance_)(VkInstance_, const void *);
typedef void (*PFN_deviceWaitIdle_)(VkDevice_);

/* per-frame sync slot (2 in flight) */
typedef struct {
        VkSemaphore_ acq, rend;
        VkFence_ fence;
} VkSync_;

/* per-window Vulkan state */
typedef struct {
        VkSurfaceKHR_ surface;
        VkSwapchainKHR_ swapchain;
        VkImage_ images[8];
        u32 nimages;
        u32 width, height;
        u32 format;              /* VK_FORMAT_* */
        VkCommandPool_ pool;
        VkCommandBuffer_ cmd;
        VkBuffer_ staging;
        VkDeviceMemory_ staging_mem;
        u8 *mapped;
        u64 staging_size;
        u8 *swizzle;             /* used when format is B8G8R8A8 */
        VkSync_ sync[2];
        int slot;
        bool ready;
} VkWin_;

/* Present-path selection (0.1.1):
 *   PLATFORM: frames flow through the platform's proven presenter — MIT-SHM
 *     on X11, wl_shm on Wayland. This is the DEFAULT: it is bit-identical to
 *     the mechanism that worked everywhere in field reports, while the 0.1.0
 *     swapchain present proved fragile on some real-driver XWayland stacks
 *     (black screen with no error). See docs/BACKENDS.md.
 *   WSI: the 0.1.0 behavior — staging upload + image copy + FIFO present via
 *     the Vulkan WSI. Opt in with REV_VK_PRESENT=wsi (used by scripts/e2e.sh
 *     so the path stays regression-tested). */
#define VK_PRESENT_PLATFORM 0
#define VK_PRESENT_WSI 1

typedef struct {
        void *lib;
        void *libxcb;            /* X11 WSI transport */
        void *xcb_conn;
        VkInstance_ inst;
        VkPhysicalDevice_ phys;
        VkDevice_ dev;
        VkQueue_ queue;
        u32 queue_family;
        int present_mode;        /* VK_PRESENT_PLATFORM | VK_PRESENT_WSI */
        VkWin_ win[REV_PLATFORM_MAX_OUTPUTS];
        int cur_win;

        PFN_vkGetInstanceProcAddr_ gipa;
        PFN_vkGetDeviceProcAddr_ gdpa;
        PFN_createInstance_ vkCreateInstance;
        PFN_enumInstExt_ vkEnumerateInstanceExtensionProperties;
        PFN_enumPhysDev_ vkEnumeratePhysicalDevices;
        PFN_getSurfSupport_ vkGetPhysicalDeviceSurfaceSupportKHR;
        PFN_getSurfCaps_ vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
        PFN_getSurfFormats_ vkGetPhysicalDeviceSurfaceFormatsKHR;
        PFN_getQueueFamProps_ vkGetPhysicalDeviceQueueFamilyProperties;
        PFN_createDevice_ vkCreateDevice;
        PFN_getDeviceQueue_ vkGetDeviceQueue;
        PFN_createXcbSurface_ vkCreateXcbSurfaceKHR;
        PFN_createXlibSurface_ vkCreateXlibSurfaceKHR;
        PFN_createWaylandSurface_ vkCreateWaylandSurfaceKHR;
        PFN_createSwapchain_ vkCreateSwapchainKHR;
        PFN_getSwapchainImages_ vkGetSwapchainImagesKHR;
        PFN_destroySwapchain_ vkDestroySwapchainKHR;
        PFN_createCommandPool_ vkCreateCommandPool;
        PFN_allocCmdBufs_ vkAllocateCommandBuffers;
        PFN_createBuffer_ vkCreateBuffer;
        void (*vkGetBufferMemoryRequirements)(VkDevice_, VkBuffer_, void *);
        PFN_getPhysDevMemProps_ vkGetPhysicalDeviceMemoryProperties;
        PFN_allocMemory_ vkAllocateMemory;
        PFN_mapMemory_ vkMapMemory;
        PFN_bindBufferMemory_ vkBindBufferMemory;
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
        PFN_queueSubmit_ vkQueueSubmit;
        PFN_queuePresent_ vkQueuePresentKHR;
        PFN_destroyFence_ vkDestroyFence;
        PFN_destroySemaphore_ vkDestroySemaphore;
        PFN_destroyBuffer_ vkDestroyBuffer;
        PFN_freeMemory_ vkFreeMemory;
        PFN_destroyCommandPool_ vkDestroyCommandPool;
        PFN_destroySurface_ vkDestroySurfaceKHR;
        PFN_destroyDevice_ vkDestroyDevice;
        PFN_destroyInstance_ vkDestroyInstance;
        PFN_deviceWaitIdle_ vkDeviceWaitIdle;

        RPlatform *plat;
} RenderVK;

#define VKSYM(field, name) \
        *(void **)(&v->field) = (void *)v->gipa(v->dev ? (void *)v->dev : (void *)v->inst, name)

static u32 vkres_ok(u32 r)
{
        return r == VK_SUCCESS_ || (i32)r == VK_SUBOPTIMAL_KHR_;
}

/* ---- per-window teardown -------------------------------------------------- */

static void vkwin_destroy(RenderVK *v, VkWin_ *w)
{
        if (!w->swapchain)
                return;
        if (v->dev && v->vkDeviceWaitIdle) {
                v->vkDeviceWaitIdle(v->dev);
                for (int i = 0; i < 2; i++) {
                        if (w->sync[i].fence && v->vkDestroyFence)
                                v->vkDestroyFence(v->dev, w->sync[i].fence, NULL);
                        if (w->sync[i].acq && v->vkDestroySemaphore)
                                v->vkDestroySemaphore(v->dev, w->sync[i].acq, NULL);
                        if (w->sync[i].rend && v->vkDestroySemaphore)
                                v->vkDestroySemaphore(v->dev, w->sync[i].rend, NULL);
                }
                if (w->staging && v->vkDestroyBuffer)
                        v->vkDestroyBuffer(v->dev, w->staging, NULL);
                if (w->staging_mem && v->vkFreeMemory)
                        v->vkFreeMemory(v->dev, w->staging_mem, NULL);
                if (w->pool && v->vkDestroyCommandPool)
                        v->vkDestroyCommandPool(v->dev, w->pool, NULL);
                if (w->swapchain && v->vkDestroySwapchainKHR)
                        v->vkDestroySwapchainKHR(v->dev, w->swapchain, NULL);
        }
        free(w->swizzle);
        w->swizzle = NULL;
        w->mapped = NULL;
        w->staging = 0;
        w->staging_mem = 0;
        w->pool = 0;
        w->swapchain = 0;
        w->ready = false;
}

/* ---- per-window creation -------------------------------------------------- */

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
        u32 format = fmts[0].format, colorspace = fmts[0].colorSpace;
        for (u32 i = 0; i < nfmt; i++) {
                if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM_ && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR_) {
                        format = VK_FORMAT_R8G8B8A8_UNORM_;
                        colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR_;
                        break;
                }
        }
        /* accept UNORM and _SRGB variants: the copy-present path moves raw
         * bytes (no sampling), so SRGB views are visually identical here */
        if (format != VK_FORMAT_R8G8B8A8_UNORM_ && format != VK_FORMAT_B8G8R8A8_UNORM_ &&
            format != 43 /*R8G8B8A8_SRGB*/ && format != 50 /*B8G8R8A8_SRGB*/) {
                snprintf(err, errcap, "surface format %u unsupported", format);
                return false;
        }

        u32 minimg = caps.minImageCount;
        if (minimg < 2) minimg = 2;
        if (caps.maxImageCount && minimg > caps.maxImageCount)
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

        /* command pool + buffer */
        struct { u32 sType_; const void *pNext; u32 flags; u32 queueFamilyIndex; } poolci;
        memset(&poolci, 0, sizeof poolci);
        poolci.sType_ = 39;    /* VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO */
        poolci.queueFamilyIndex = v->queue_family;
        r = v->vkCreateCommandPool(v->dev, &poolci, NULL, &w->pool);
        if (r != VK_SUCCESS_) {
                snprintf(err, errcap, "vkCreateCommandPool 0x%x", r);
                return false;
        }

        VkCommandBufferAllocateInfo_ cai;
        memset(&cai, 0, sizeof cai);
        cai.sType_ = ST_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = w->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY_;
        cai.commandBufferCount = 1;
        r = v->vkAllocateCommandBuffers(v->dev, &cai, &w->cmd);
        if (r != VK_SUCCESS_) {
                snprintf(err, errcap, "vkAllocateCommandBuffers 0x%x", r);
                return false;
        }

        /* staging buffer: host-visible, coherent */
        VkBufferCreateInfo_ bci;
        memset(&bci, 0, sizeof bci);
        bci.sType_ = ST_BUFFER_CREATE_INFO;
        bci.size = (u64)ext.width * ext.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT_;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE_;
        r = v->vkCreateBuffer(v->dev, &bci, NULL, &w->staging);
        if (r != VK_SUCCESS_) {
                snprintf(err, errcap, "vkCreateBuffer 0x%x", r);
                return false;
        }

        struct { u64 size, alignment; u32 memoryTypeBits; } reqs;
        memset(&reqs, 0, sizeof reqs);
        v->vkGetBufferMemoryRequirements(v->dev, w->staging, &reqs);
        VkPhysicalDeviceMemoryProperties_ mprops;
        v->vkGetPhysicalDeviceMemoryProperties(v->phys, &mprops);
        i32 memtype = -1;
        for (u32 i = 0; i < mprops.memoryTypeCount; i++) {
                u32 props = mprops.memoryTypes[i].propertyFlags;
                if ((props & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT_ | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT_) &&
                    (reqs.memoryTypeBits & (1u << i))) {
                        if ((props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_) || memtype < 0)
                                memtype = (i32)i;
                        if (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT_)
                                break;
                }
        }
        if (memtype < 0) {
                snprintf(err, errcap, "no host-visible coherent memory type");
                return false;
        }
        VkMemoryAllocateInfo_ mai;
        memset(&mai, 0, sizeof mai);
        mai.sType_ = ST_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = (u32)memtype;
        r = v->vkAllocateMemory(v->dev, &mai, NULL, &w->staging_mem);
        if (r != VK_SUCCESS_) {
                snprintf(err, errcap, "vkAllocateMemory 0x%x", r);
                return false;
        }
        r = v->vkBindBufferMemory(v->dev, w->staging, w->staging_mem, 0);
        if (r != VK_SUCCESS_) {
                snprintf(err, errcap, "vkBindBufferMemory 0x%x", r);
                return false;
        }
        r = v->vkMapMemory(v->dev, w->staging_mem, 0, VK_WHOLE_SIZE_, 0, (void **)&w->mapped);
        if (r != VK_SUCCESS_ || !w->mapped) {
                snprintf(err, errcap, "vkMapMemory 0x%x", r);
                return false;
        }
        w->staging_size = reqs.size;

        if (format == VK_FORMAT_B8G8R8A8_UNORM_ || format == 50 /*B8G8R8A8_SRGB*/) {
                w->swizzle = (u8 *)malloc((size_t)ext.width * ext.height * 4);
                if (!w->swizzle)
                        return false;
        }

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
        w->slot = 0;
        w->ready = true;
        return true;
}

/* ---- init ------------------------------------------------------------------- */

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
        VkInstanceCreateInfo_ ici;
        memset(&ici, 0, sizeof ici);
        ici.sType_ = ST_INSTANCE_CREATE_INFO;
        ici.pAppInfo = &app;
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
        v->vkEnumeratePhysicalDevices(v->inst, &ndev, NULL);
        if (ndev == 0 || ndev > 16) {
                snprintf(err, errcap, "no physical devices");
                return false;
        }
        VkPhysicalDevice_ devs[16];
        v->vkEnumeratePhysicalDevices(v->inst, &ndev, devs);

        bool found = false;
        for (u32 d = 0; d < ndev && !found; d++) {
                u32 nfam = 0;
                v->vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nfam, NULL);
                if (nfam == 0 || nfam > 64)
                        continue;
                VkQueueFamilyProperties_ fams[64];
                v->vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nfam, fams);
                for (u32 f = 0; f < nfam; f++) {
                        if (!(fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT_))
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
                        /* this device can present and accept transfer writes */
                        VkDeviceQueueCreateInfo_ qci;
                        memset(&qci, 0, sizeof qci);
                        qci.sType_ = ST_DEVICE_QUEUE_CREATE_INFO;
                        qci.queueFamilyIndex = f;
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
                        static u32 zerofeatures[55];    /* VkPhysicalDeviceFeatures, all-off */
                        dci.pEnabledFeatures = zerofeatures;
                        if (v->vkCreateDevice(devs[d], &dci, NULL, &v->dev) != VK_SUCCESS_)
                                continue;
                        v->phys = devs[d];
                        v->queue_family = f;
                        found = true;
                        break;
                }
        }
        if (!found) {
                snprintf(err, errcap, "no device with presentable TRANSFER_DST swapchain");
                return false;
        }

#define DSYM(field, name) *(void **)(&v->field) = (void *)v->gdpa((void *)v->dev, name)
        DSYM(vkGetDeviceQueue, "vkGetDeviceQueue");
        DSYM(vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
        DSYM(vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
        DSYM(vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
        DSYM(vkCreateCommandPool, "vkCreateCommandPool");
        DSYM(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
        DSYM(vkCreateBuffer, "vkCreateBuffer");
        DSYM(vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
        DSYM(vkAllocateMemory, "vkAllocateMemory");
        DSYM(vkMapMemory, "vkMapMemory");
        DSYM(vkBindBufferMemory, "vkBindBufferMemory");
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
        DSYM(vkQueueSubmit, "vkQueueSubmit");
        DSYM(vkQueuePresentKHR, "vkQueuePresentKHR");
        DSYM(vkDestroyFence, "vkDestroyFence");
        DSYM(vkDestroySemaphore, "vkDestroySemaphore");
        DSYM(vkDestroyBuffer, "vkDestroyBuffer");
        DSYM(vkFreeMemory, "vkFreeMemory");
        DSYM(vkDestroyCommandPool, "vkDestroyCommandPool");
        DSYM(vkDestroyDevice, "vkDestroyDevice");
        DSYM(vkDeviceWaitIdle, "vkDeviceWaitIdle");
#undef DSYM

        v->vkGetDeviceQueue(v->dev, v->queue_family, 0, &v->queue);

        /* 0.1.1 present-path selection: platform (default) or WSI */
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
                LOGI("vulkan: GPU present capable (queue family %u); presenting via "
                     "platform SHM path — set REV_VK_PRESENT=wsi for swapchain present",
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

        LOGI("vulkan: swapchain present path ready (queue family %u, format %s)",
             v->queue_family, v->win[0].format == VK_FORMAT_R8G8B8A8_UNORM_ ? "RGBA8" : "BGRA8");
        return true;
}

/* ---- frame ------------------------------------------------------------------- */

static bool vk_begin(RRenderer *rr, int win, u32 w, u32 h)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return false;
        v->cur_win = win;
        if (v->present_mode == VK_PRESENT_PLATFORM) {
                /* the platform presenter takes explicit sizes per frame */
                return true;
        }
        if (!v->win[win].ready)
                return false;
        if (v->win[win].width != w || v->win[win].height != h) {
                v->win[win].width = w;
                v->win[win].height = h;
                vkwin_destroy(v, &v->win[win]);
                char err[128] = { 0 };
                if (!vkwin_create(v, win, err, sizeof err)) {
                        LOGW("vulkan: swapchain recreate failed: %s", err);
                        return false;
                }
        }
        v->cur_win = win;
        return true;
}

static bool vk_layer(RRenderer *rr, const RLayerCmd *cmd)
{
        (void)rr;
        (void)cmd;
        return false;    /* present-only tier: engine composites on CPU */
}

static bool vk_end(RRenderer *rr, const RFb *cpu_frame)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        if (!cpu_frame || !cpu_frame->px)
                return false;

        /* platform-present mode: hand the composited frame to the backend's
         * proven presenter (MIT-SHM / wl_shm). Same pixels, same window, the
         * mechanism the 1.0-era engine shipped and every field report
         * confirms — see the 0.1.0 XWayland black-screen postmortem. */
        if (v->present_mode == VK_PRESENT_PLATFORM)
                return v->plat->present_cpu(v->plat, v->cur_win,
                                            cpu_frame->px, cpu_frame->w, cpu_frame->h);

        VkWin_ *w = &v->win[v->cur_win];
        if (!w->ready)
                return false;
        if (cpu_frame->w != w->width || cpu_frame->h != w->height)
                return false;

        VkSync_ *s = &w->sync[w->slot];

        if (v->vkWaitForFences(v->dev, 1, &s->fence, 1, 1000000000ull) != VK_SUCCESS_)
                return false;
        v->vkResetFences(v->dev, 1, &s->fence);

        u32 img = 0;
        u32 r = v->vkAcquireNextImageKHR(v->dev, w->swapchain, 1000000000ull, s->acq, 0, &img);
        if (r != VK_SUCCESS_) {
                if ((i32)r == VK_ERROR_OUT_OF_DATE_KHR_) {
                        vkwin_destroy(v, w);
                        char err[128] = { 0 };
                        if (vkwin_create(v, v->cur_win, err, sizeof err))
                                return vk_end(rr, cpu_frame);
                }
                return false;
        }

        /* upload: memcpy (swizzle for BGRA) */
        size_t n = (size_t)w->width * w->height;
        if (w->format == VK_FORMAT_R8G8B8A8_UNORM_) {
                memcpy(w->mapped, cpu_frame->px, n * 4);
        } else {
                const u32 *src = cpu_frame->px;
                u32 *dst = (u32 *)w->mapped;
                for (size_t i = 0; i < n; i++) {
                        u32 p = src[i];
                        dst[i] = (p & 0xff00ff00u) | ((p & 0x00ff0000u) >> 16) | ((p & 0x000000ffu) << 16);
                }
        }

        /* record: barriers + copy */
        v->vkResetCommandBuffer(w->cmd, 0);
        VkCommandBufferBeginInfo_ bi;
        memset(&bi, 0, sizeof bi);
        bi.sType_ = ST_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT_;
        if (v->vkBeginCommandBuffer(w->cmd, &bi) != VK_SUCCESS_)
                return false;

        VkImageMemoryBarrier_ b1;
        memset(&b1, 0, sizeof b1);
        b1.sType_ = ST_IMAGE_MEMORY_BARRIER;
        b1.srcAccessMask = 0;
        b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT_;
        b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED_;
        b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_;
        b1.image = w->images[img];
        b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        b1.subresourceRange.levelCount = 1;
        b1.subresourceRange.layerCount = 1;
        v->vkCmdPipelineBarrier(w->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT_,
                                VK_PIPELINE_STAGE_TRANSFER_BIT_, 0, 0, NULL, 0, NULL, 1, &b1);

        VkBufferImageCopy_ copy;
        memset(&copy, 0, sizeof copy);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = w->width;
        copy.imageExtent.height = w->height;
        copy.imageExtent.depth = 1;
        v->vkCmdCopyBufferToImage(w->cmd, w->staging, w->images[img],
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_, 1, &copy);

        VkImageMemoryBarrier_ b2;
        memset(&b2, 0, sizeof b2);
        b2.sType_ = ST_IMAGE_MEMORY_BARRIER;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT_;
        b2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT_;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL_;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR_;
        b2.image = w->images[img];
        b2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT_;
        b2.subresourceRange.levelCount = 1;
        b2.subresourceRange.layerCount = 1;
        v->vkCmdPipelineBarrier(w->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT_,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT_, 0, 0, NULL, 0, NULL, 1, &b2);

        if (v->vkEndCommandBuffer(w->cmd) != VK_SUCCESS_)
                return false;

        VkSubmitInfo_ si;
        memset(&si, 0, sizeof si);
        si.sType_ = ST_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s->acq;
        u32 stage = VK_PIPELINE_STAGE_TRANSFER_BIT_;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &w->cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s->rend;
        if (v->vkQueueSubmit(v->queue, 1, &si, s->fence) != VK_SUCCESS_)
                return false;

        VkPresentInfoKHR_ pi;
        memset(&pi, 0, sizeof pi);
        pi.sType_ = ST_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s->rend;
        pi.swapchainCount = 1;
        pi.pSwapchains = &w->swapchain;
        pi.pImageIndices = &img;
        r = v->vkQueuePresentKHR(v->queue, &pi);

        w->slot ^= 1;
        return vkres_ok(r);
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

static void vk_fini(RRenderer *rr)
{
        RenderVK *v = (RenderVK *)rr->ctx;
        if (v->dev && v->vkDeviceWaitIdle) {
                v->vkDeviceWaitIdle(v->dev);
                for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++)
                        vkwin_destroy(v, &v->win[i]);
        }
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
        rr->wants_layers = false;    /* present-only tier in 0.1.x; see VK_PRESENT notes above */
        rr->ctx = v;
        rr->init = vk_init;
        rr->fini = vk_fini;
        rr->begin = vk_begin;
        rr->layer = vk_layer;
        rr->end = vk_end;
        rr->resize = vk_resize;
        return rr;
}
