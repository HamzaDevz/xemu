/*
 * hakuX HLE GPU — own Vulkan instance/device (spec §5.1, D7).
 * Include only under CONFIG_VULKAN.
 */
#ifndef HW_XBOX_HLE_VK_VKDEV_H
#define HW_XBOX_HLE_VK_VKDEV_H

#include <volk.h>
#include <vulkan/vulkan_android.h>

/* Command buffers in flight (back.c's ring, one recording each): a slot's
 * recording starts once its previous submission has completed, so the
 * per-slot resources (batch.c's indices, gamma.c, zread.c) hold one each. */
#define HLE_FRAMES 3

/* VK_KHR_dynamic_rendering_local_read (Vulkan 1.4) for headers older than it
 * (the NDK's): what HleCaps.fbfetch uses */
#ifndef VK_KHR_dynamic_rendering_local_read
#define VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME "VK_KHR_dynamic_rendering_local_read"
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR \
    ((VkStructureType)1000232000)
#define VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR ((VkImageLayout)1000232000)
typedef struct VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR {
    VkStructureType sType;
    void *pNext;
    VkBool32 dynamicRenderingLocalRead;
} VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR;
#endif

typedef struct HleCaps {
    bool floor_ok;
    char missing[256];          /* floor features absent (comma list) */
    /* Plus tier, opportunistic (§5.1) */
    bool gpl, eds3, descriptor_buffer, maint5, sscaled, local_read;
    bool fbfetch;               /* local read + push descriptors: combiner stages that
                                   sample the bound colour target read it as an input
                                   attachment (vk/draw.c hle_draw_fb_stages) */
    bool exec_props, aniso;
    bool host_query_reset, occl_precise;        /* visibility tests */
    bool wide_lines;            /* enabled when present: every pipeline draws 1.0-wide
                                   lines (Turnip on the Adreno 650 has none) */
    bool cache_control;         /* pipelineCreationCacheControl: a pipeline asked of the
                                   cache only (vk/draw.c, asynchronous builds) */
    bool swapchain;             /* the Android surface and VK_KHR_swapchain: the window's
                                   presenter (vk/swapchain.c) */
} HleCaps;

typedef struct HleVk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    struct VolkDeviceTable t;   /* device-level entry points of `dev` (volk.h declares
                                 * only the struct tag, no typedef) */
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_props;
    HleCaps caps;
} HleVk;

extern HleVk g_hvk;

/* 0 = ready; -1 = no Vulkan; -2 = floor not met (g_hvk.caps.missing). */
int hle_vk_init(void);
/* The back end is up (hle_back_start done): other threads may use g_hvk. */
bool hle_vk_ready(void);

/* The device can't go on (VK_ERROR_DEVICE_LOST: a driver fault; or a
 * submit that failed, whose timeline value would never signal): every wait
 * on the GPU returns (hle_tl_lose), the back end and the presenter stop,
 * and the host hears why (hle_host_gpu_lost). Once; later calls do nothing. */
void hle_vk_lose(VkResult r, const char *what);
/* hle_vk_lose was called: nothing more goes to the device. */
bool hle_vk_lost(void);
/* r == VK_SUCCESS; else false, the first few logged, and a lost device
 * reported (hle_vk_lose). */
bool hle_vk_check(VkResult r, const char *what);
/* r's name ("VK_ERROR_DEVICE_LOST"), or its number. */
const char *hle_vk_result_text(VkResult r);
void hle_vk_set_ready(void);
/* g_hvk.queue's external synchronization: the render thread, the window's
 * presenter and a capture submit to it, and vkDeviceWaitIdle needs it too. */
void hle_vk_queue_lock(void);
void hle_vk_queue_unlock(void);
/* The time its lock was waited for since the last call (statistics). */
int64_t hle_vk_queue_wait_ns(void);
int hle_vk_memtype(uint32_t bits, VkMemoryPropertyFlags want,
                   VkMemoryPropertyFlags avoid);

#endif
