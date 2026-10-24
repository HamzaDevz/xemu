/*
 * hakuX HLE GPU — Vulkan instance/device, floor + plus capability tiers.
 */
#include "hw/xbox/hle/base.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/batch.h"
#include "hw/xbox/hle/vk/quirks.h"

HleVk g_hvk;
static HleMutex g_queue_lock;
static bool g_ready;
static bool g_lost;                     /* hle_vk_lose */
static int64_t g_lock_wait_ns;          /* hle_vk_queue_wait_ns */

const char *hle_vk_result_text(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: {
        static __thread char s[24];
        snprintf(s, sizeof(s), "VkResult %d", (int)r);
        return s;
    }
    }
}

void hle_vk_lose(VkResult r, const char *what)
{
    if (__atomic_exchange_n(&g_lost, true, __ATOMIC_ACQ_REL)) {
        return;
    }
    char why[160];
    snprintf(why, sizeof(why), "stopped responding (%s in %s)", hle_vk_result_text(r), what);
    hle_log("hakuX-hle-back", "FATAL: the GPU %s: nothing more is drawn", why);
    hle_tl_lose();
    hle_host_gpu_lost(why);
}

bool hle_vk_lost(void)
{
    return __atomic_load_n(&g_lost, __ATOMIC_ACQUIRE);
}

bool hle_vk_check(VkResult r, const char *what)
{
    if (r == VK_SUCCESS) {
        return true;
    }
    if (r == VK_ERROR_DEVICE_LOST) {
        hle_vk_lose(r, what);
        return false;
    }
    static unsigned s_said;
    if (__atomic_fetch_add(&s_said, 1, __ATOMIC_RELAXED) < 16) {
        hle_log("hakuX-hle-back", "vk: %s: %s", what, hle_vk_result_text(r));
    }
    return false;
}

void hle_vk_queue_lock(void)
{
    if (pthread_mutex_trylock(&g_queue_lock.m) == 0) {
        return;
    }
    const int64_t t0 = hle_now_ns();
    hle_mutex_lock(&g_queue_lock);
    __atomic_fetch_add(&g_lock_wait_ns, hle_now_ns() - t0, __ATOMIC_RELAXED);
}

void hle_vk_queue_unlock(void)
{
    hle_mutex_unlock(&g_queue_lock);
}

int64_t hle_vk_queue_wait_ns(void)
{
    return __atomic_exchange_n(&g_lock_wait_ns, 0, __ATOMIC_RELAXED);
}

bool hle_vk_ready(void)
{
    return hle_atomic_load_acquire(&g_ready);
}

void hle_vk_set_ready(void)
{
    hle_smp_wmb();
    hle_atomic_set(&g_ready, true);
}

static bool has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(e[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static void need(HleCaps *c, bool ok, const char *what)
{
    if (!ok) {
        size_t l = strlen(c->missing);
        c->floor_ok = false;
        snprintf(c->missing + l, sizeof(c->missing) - l, "%s%s", l ? "," : "", what);
    }
}

static void chain(VkBaseOutStructure **tail, void *s)
{
    (*tail)->pNext = s;
    *tail = s;
}

int hle_vk_memtype(uint32_t bits, VkMemoryPropertyFlags want,
                   VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < g_hvk.mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = g_hvk.mem.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) {
            return (int)i;
        }
    }
    return -1;
}

int hle_vk_init(void)
{
    HleVk *v = &g_hvk;
    HleCaps *c = &v->caps;
    if (v->dev) {
        return 0;
    }
    if (volkGetInstanceVersion() == 0) {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)hle_host_vk_loader();
        if (gipa) {
            volkInitializeCustom(gipa);
        } else if (volkInitialize() != VK_SUCCESS) {
            hle_log("hakuX-hle-back", "vk: volkInitialize failed");
            return -1;
        }
    }
    hle_mutex_init(&g_queue_lock);
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hakuX-hle",
        .apiVersion = VK_API_VERSION_1_3,
    };
    /* the window's surface (vk/swapchain.c): where the loader has it */
    uint32_t nie = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    g_autofree VkExtensionProperties *iext = g_new(VkExtensionProperties, nie ? nie : 1);
    vkEnumerateInstanceExtensionProperties(NULL, &nie, iext);
    const char *iexts[2];
    uint32_t nix = 0;
    if (has_ext(iext, nie, VK_KHR_SURFACE_EXTENSION_NAME) &&
        has_ext(iext, nie, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        iexts[nix++] = VK_KHR_SURFACE_EXTENSION_NAME;
        iexts[nix++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    }
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = nix, .ppEnabledExtensionNames = iexts,
    };
    if (vkCreateInstance(&ici, NULL, &v->inst) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "vk: vkCreateInstance failed");
        return -1;
    }
    volkLoadInstanceOnly(v->inst);
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(v->inst, &n, &v->phys);
    if (!n) {
        hle_log("hakuX-hle-back", "vk: no physical device");
        return -1;
    }
    vkGetPhysicalDeviceProperties(v->phys, &v->props);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mem);

    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qf);
    v->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && v->qfam == UINT32_MAX; i++) {
        VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((qf[i].queueFlags & want) == want) {
            v->qfam = i;
        }
    }
    for (uint32_t i = 0; i < nq; i++) {
        hle_log("hakuX-hle-back", "vk: queue family %u: flags %x, %u queues%s", i, qf[i].queueFlags,
                qf[i].queueCount, i == v->qfam ? " (used)" : "");
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(v->phys, NULL, &ne, NULL);
    g_autofree VkExtensionProperties *ext = g_new(VkExtensionProperties, ne);
    vkEnumerateDeviceExtensionProperties(v->phys, NULL, &ne, ext);
    bool e_vids = has_ext(ext, ne, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    bool e_eds2 = has_ext(ext, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    bool e_cbc = has_ext(ext, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    bool e_pv = has_ext(ext, ne, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    bool e_dce = has_ext(ext, ne, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    bool e_4444 = has_ext(ext, ne, VK_EXT_4444_FORMATS_EXTENSION_NAME);
    bool e_iu8x = has_ext(ext, ne, "VK_EXT_index_type_uint8");
    bool e_iu8k = has_ext(ext, ne, "VK_KHR_index_type_uint8");
    bool e_ahb = has_ext(ext, ne,
                         VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
                 has_ext(ext, ne, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    bool e_eds3 = has_ext(ext, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    bool e_gpl = has_ext(ext, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                 has_ext(ext, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    bool e_lr = has_ext(ext, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    bool e_pd = has_ext(ext, ne, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    {   /* debug: how guest RAM could be shared with the GPU */
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
        char mt[160];
        int o = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount && o < (int)sizeof(mt) - 12; i++) {
            o += snprintf(mt + o, sizeof(mt) - (size_t)o, " %u:%x", i, mp.memoryTypes[i].propertyFlags);
        }
        hle_log("hakuX-hle-back", "vk: memory: host_ptr=%d dma_buf=%d fd=%d ahb=%d, types%s",
                has_ext(ext, ne, "VK_EXT_external_memory_host"),
                has_ext(ext, ne, "VK_EXT_external_memory_dma_buf"),
                has_ext(ext, ne, "VK_KHR_external_memory_fd"), e_ahb, mt);
    }

    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT q_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT q_eds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT q_cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
    VkPhysicalDeviceProvokingVertexFeaturesEXT q_pv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT q_dce = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT };
    VkPhysicalDevice4444FormatsFeaturesEXT q_4444 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT };
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT q_iu8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT q_eds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT };
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT q_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR q_lr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR };
    VkPhysicalDeviceVulkan13Features q13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features q12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &q13 };
    VkPhysicalDeviceFeatures2 q = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q12 };
    VkBaseOutStructure *tail = (VkBaseOutStructure *)&q13;
    if (e_vids) chain(&tail, &q_vids);
    if (e_eds2) chain(&tail, &q_eds2);
    if (e_cbc) chain(&tail, &q_cbc);
    if (e_pv) chain(&tail, &q_pv);
    if (e_dce) chain(&tail, &q_dce);
    if (e_4444) chain(&tail, &q_4444);
    if (e_iu8x || e_iu8k) chain(&tail, &q_iu8);
    if (e_eds3) chain(&tail, &q_eds3);
    if (e_gpl) chain(&tail, &q_gpl);
    if (e_lr) chain(&tail, &q_lr);
    vkGetPhysicalDeviceFeatures2(v->phys, &q);

    memset(c, 0, sizeof(*c));
    c->floor_ok = true;
    need(c, v->props.apiVersion >= VK_API_VERSION_1_3, "vulkan1.3");
    need(c, v->qfam != UINT32_MAX, "graphics+compute queue");
    c->wide_lines = q.features.wideLines;
    need(c, q.features.largePoints, "largePoints");
    need(c, q.features.logicOp, "logicOp");
    need(c, q.features.dualSrcBlend, "dualSrcBlend");
    need(c, q.features.depthClamp, "depthClamp");
    need(c, q.features.textureCompressionBC, "textureCompressionBC");
    need(c, q12.timelineSemaphore, "timelineSemaphore");
    need(c, q12.bufferDeviceAddress, "bufferDeviceAddress");
    need(c, q12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(c, q12.descriptorBindingPartiallyBound, "partiallyBound");
    need(c, q12.descriptorBindingSampledImageUpdateAfterBind, "sampledImageUAB");
    need(c, q13.synchronization2, "synchronization2");
    need(c, q13.dynamicRendering, "dynamicRendering");
    need(c, e_vids && q_vids.vertexInputDynamicState, "vertex_input_dynamic_state");
    need(c, e_eds2 && q_eds2.extendedDynamicState2 && q_eds2.extendedDynamicState2LogicOp,
         "eds2+logicOp");
    need(c, e_cbc && q_cbc.customBorderColors && q_cbc.customBorderColorWithoutFormat,
         "custom_border_color");
    need(c, e_pv && q_pv.provokingVertexLast, "provoking_vertex");
    need(c, e_dce && q_dce.depthClipEnable, "depth_clip_enable");
    need(c, e_4444 && q_4444.formatA4R4G4B4, "4444_formats");
    need(c, (e_iu8x || e_iu8k) && q_iu8.indexTypeUint8, "index_type_uint8");
    need(c, e_ahb, "AHB external memory");
    c->gpl = e_gpl && q_gpl.graphicsPipelineLibrary;
    c->eds3 = e_eds3 && q_eds3.extendedDynamicState3ColorBlendEnable &&
              q_eds3.extendedDynamicState3ColorBlendEquation &&
              q_eds3.extendedDynamicState3ColorWriteMask &&
              q_eds3.extendedDynamicState3LogicOpEnable;
    c->descriptor_buffer = has_ext(ext, ne, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    c->maint5 = has_ext(ext, ne, "VK_KHR_maintenance5");
    c->local_read = e_lr && q_lr.dynamicRenderingLocalRead;
    c->fbfetch = c->local_read && e_pd && !getenv("HAKUX_HLE_NO_FBFETCH");
    c->exec_props = has_ext(ext, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    c->aniso = q.features.samplerAnisotropy;
    c->host_query_reset = q12.hostQueryReset;
    c->cache_control = q13.pipelineCreationCacheControl;
    c->occl_precise = q.features.occlusionQueryPrecise;
    c->swapchain = nix && has_ext(ext, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_R16G16_SSCALED, &fp);
    c->sscaled = fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (!c->floor_ok) {
        hle_log("hakuX-hle-back", "vk: floor not met on \"%s\": missing %s",
                v->props.deviceName, c->missing);
        snprintf(g_hle_unavailable, sizeof(g_hle_unavailable), "%s lacks %s", v->props.deviceName,
                 c->missing);
        vkDestroyInstance(v->inst, NULL);
        v->inst = VK_NULL_HANDLE;
        return -2;
    }

    /* Enable exactly the features used (never "everything supported"). */
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT f_vids = q_vids;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT f_eds2 = {
        .sType = q_eds2.sType, .extendedDynamicState2 = VK_TRUE,
        .extendedDynamicState2LogicOp = VK_TRUE };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT f_cbc = {
        .sType = q_cbc.sType, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE };
    VkPhysicalDeviceProvokingVertexFeaturesEXT f_pv = {
        .sType = q_pv.sType, .provokingVertexLast = VK_TRUE };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT f_dce = {
        .sType = q_dce.sType, .depthClipEnable = VK_TRUE };
    VkPhysicalDevice4444FormatsFeaturesEXT f_4444 = {
        .sType = q_4444.sType, .formatA4R4G4B4 = VK_TRUE };
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT f_iu8 = {
        .sType = q_iu8.sType, .indexTypeUint8 = VK_TRUE };
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT f_eds3 = {
        .sType = q_eds3.sType,
        .extendedDynamicState3ColorBlendEnable = VK_TRUE,
        .extendedDynamicState3ColorBlendEquation = VK_TRUE,
        .extendedDynamicState3ColorWriteMask = VK_TRUE,
        .extendedDynamicState3LogicOpEnable = VK_TRUE };
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT f_gpl = {
        .sType = q_gpl.sType, .graphicsPipelineLibrary = VK_TRUE };
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR f_lr = {
        .sType = q_lr.sType, .dynamicRenderingLocalRead = VK_TRUE };
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE,
        .pipelineCreationCacheControl = c->cache_control,
        /* SPIR-V 1.6: glslang makes a discard a demote */
        .shaderDemoteToHelperInvocation = q13.shaderDemoteToHelperInvocation };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13,
        .timelineSemaphore = VK_TRUE, .bufferDeviceAddress = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE, .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        /* the shaders index the texture and sampler arrays nonuniformEXT */
        .shaderSampledImageArrayNonUniformIndexing = q12.shaderSampledImageArrayNonUniformIndexing,
        .hostQueryReset = c->host_query_reset };
    VkPhysicalDeviceFeatures2 f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    f.features.wideLines = c->wide_lines;
    f.features.largePoints = f.features.logicOp = VK_TRUE;
    f.features.dualSrcBlend = f.features.depthClamp = VK_TRUE;
    f.features.textureCompressionBC = VK_TRUE;
    f.features.samplerAnisotropy = c->aniso;
    f.features.occlusionQueryPrecise = c->occl_precise;
    f_vids.pNext = NULL;
    f_vids.vertexInputDynamicState = VK_TRUE;
    tail = (VkBaseOutStructure *)&f13;
    chain(&tail, &f_vids);
    chain(&tail, &f_eds2);
    chain(&tail, &f_cbc);
    chain(&tail, &f_pv);
    chain(&tail, &f_dce);
    chain(&tail, &f_4444);
    chain(&tail, &f_iu8);
    if (c->eds3) chain(&tail, &f_eds3);
    if (c->gpl) chain(&tail, &f_gpl);
    if (c->fbfetch) chain(&tail, &f_lr);

    const char *exts[20];
    uint32_t nx = 0;
    exts[nx++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    exts[nx++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME;
    exts[nx++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    exts[nx++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
    exts[nx++] = VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME;
    exts[nx++] = VK_EXT_4444_FORMATS_EXTENSION_NAME;
    exts[nx++] = e_iu8k ? "VK_KHR_index_type_uint8" : "VK_EXT_index_type_uint8";
    exts[nx++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
    exts[nx++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    if (c->eds3) exts[nx++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
    if (c->gpl) {
        exts[nx++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        exts[nx++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
    }
    if (c->fbfetch) {
        exts[nx++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        exts[nx++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    }
    if (c->swapchain) {
        exts[nx++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = v->qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nx, .ppEnabledExtensionNames = exts };
    VkResult r = vkCreateDevice(v->phys, &dci, NULL, &v->dev);
    if (r != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "vk: vkCreateDevice failed (%d)", r);
        vkDestroyInstance(v->inst, NULL);
        v->inst = VK_NULL_HANDLE;
        v->dev = VK_NULL_HANDLE;
        return -1;
    }
    volkLoadDeviceTable(&v->t, v->dev);
    hle_batch_install(&v->t);           /* strip batching sees every recorded command */
    v->t.vkGetDeviceQueue(v->dev, v->qfam, 0, &v->queue);
    v->get_ahb_props = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(v->dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    hle_log("hakuX-hle-back", "vk: device \"%s\" api %u.%u.%u driver 0x%x floor=ok "
            "plus: gpl=%d eds3=%d dbuf=%d m5=%d sscaled=%d local_read=%d fbfetch=%d cache_control=%d "
            "swapchain=%d",
            v->props.deviceName, VK_API_VERSION_MAJOR(v->props.apiVersion),
            VK_API_VERSION_MINOR(v->props.apiVersion),
            VK_API_VERSION_PATCH(v->props.apiVersion), v->props.driverVersion,
            c->gpl, c->eds3, c->descriptor_buffer, c->maint5, c->sscaled,
            c->local_read, c->fbfetch, c->cache_control, c->swapchain);
    hle_quirks_init(v->phys);           /* what this driver needs worked around */
    return 0;
}

