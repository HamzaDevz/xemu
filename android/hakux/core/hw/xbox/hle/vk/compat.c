/*
 * x1box: the HLE GPU on Vulkan 1.1 drivers (compat.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/vk/compat.h"

#include <stdlib.h>
#include <string.h>

static struct VolkDeviceTable R;
static struct VolkDeviceTable g_wrapped; /* R with the layer's entry points */        /* the driver's entry points */
static VkDevice g_dev;
static bool g_on;
static HleMutex g_mx;                   /* every table below */

bool hle_compat_on(void)
{
    return g_on;
}

const struct VolkDeviceTable *hle_compat_table(void)
{
    return &g_wrapped;
}

static uint64_t fnv(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}
#define FNV0 0xcbf29ce484222325ull

/* ---- images and views: what a render pass and its framebuffer need ---------- */

typedef struct { uint32_t w, h; } Ext2;
typedef struct {
    VkFormat fmt;
    uint32_t w, h;                      /* 0: not known (a swapchain image's) */
    VkFramebuffer *fbs;                 /* framebuffers using the view */
    uint32_t nfb, cap;
} ViewInfo;

static GHashTable *g_images;            /* VkImage -> Ext2 */
static GHashTable *g_views;             /* VkImageView -> ViewInfo */

static VkResult VKAPI_CALL c_CreateImage(VkDevice d, const VkImageCreateInfo *ci,
                                         const VkAllocationCallbacks *a, VkImage *img)
{
    VkResult r = R.vkCreateImage(d, ci, a, img);
    if (r == VK_SUCCESS) {
        Ext2 *e = g_new(Ext2, 1);
        e->w = ci->extent.width;
        e->h = ci->extent.height;
        hle_mutex_lock(&g_mx);
        g_hash_table_replace(g_images, (gpointer)*img, e);
        hle_mutex_unlock(&g_mx);
    }
    return r;
}

static void VKAPI_CALL c_DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks *a)
{
    hle_mutex_lock(&g_mx);
    g_free(g_hash_table_lookup(g_images, (gpointer)img));
    g_hash_table_remove(g_images, (gpointer)img);
    hle_mutex_unlock(&g_mx);
    R.vkDestroyImage(d, img, a);
}

static VkResult VKAPI_CALL c_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci,
                                             const VkAllocationCallbacks *a, VkImageView *v)
{
    VkResult r = R.vkCreateImageView(d, ci, a, v);
    if (r == VK_SUCCESS) {
        ViewInfo *vi = g_new0(ViewInfo, 1);
        vi->fmt = ci->format;
        hle_mutex_lock(&g_mx);
        const Ext2 *e = g_hash_table_lookup(g_images, (gpointer)ci->image);
        if (e) {
            const uint32_t m = ci->subresourceRange.baseMipLevel;
            vi->w = MAX(1u, e->w >> m);
            vi->h = MAX(1u, e->h >> m);
        }
        g_hash_table_replace(g_views, (gpointer)*v, vi);
        hle_mutex_unlock(&g_mx);
    }
    return r;
}

static GHashTable *g_fbs;               /* key hash -> VkFramebuffer */
static GHashTable *g_fb_keys;           /* VkFramebuffer -> key hash */

/* A framebuffer goes with any of its views (the back end destroys a view only
 * once the GPU is done with it, so with its framebuffers too); under g_mx. */
static void drop_fb(VkFramebuffer fb)
{
    if (!g_hash_table_contains(g_fb_keys, (gpointer)fb)) {
        return;                         /* gone with another of its views */
    }
    gpointer key = g_hash_table_lookup(g_fb_keys, (gpointer)fb);
    g_hash_table_remove(g_fb_keys, (gpointer)fb);
    g_hash_table_remove(g_fbs, key);
    R.vkDestroyFramebuffer(g_dev, fb, NULL);
}

static void VKAPI_CALL c_DestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{
    hle_mutex_lock(&g_mx);
    ViewInfo *vi = g_hash_table_lookup(g_views, (gpointer)v);
    if (vi) {
        for (uint32_t i = 0; i < vi->nfb; i++) {
            drop_fb(vi->fbs[i]);
        }
        g_free(vi->fbs);
        g_free(vi);
        g_hash_table_remove(g_views, (gpointer)v);
    }
    hle_mutex_unlock(&g_mx);
    R.vkDestroyImageView(d, v, a);
}

/* ---- render passes --------------------------------------------------------- */

#define MAX_COLOR 4

typedef struct RpKey {
    uint32_t ncolor;
    VkFormat cfmt[MAX_COLOR];
    uint8_t cload[MAX_COLOR], cstore[MAX_COLOR];
    VkImageLayout clayout[MAX_COLOR];
    VkFormat dfmt;                      /* UNDEFINED: none */
    uint8_t dload, dstore, sload, sstore;
    VkImageLayout dlayout;
} RpKey;

static GHashTable *g_rps;               /* key hash -> VkRenderPass */

static VkRenderPass render_pass(const RpKey *k)
{
    const uint64_t h = fnv(k, sizeof(*k), FNV0);
    VkRenderPass rp = (VkRenderPass)g_hash_table_lookup(g_rps, (gpointer)(uintptr_t)h);
    if (rp) {
        return rp;
    }
    VkAttachmentDescription at[MAX_COLOR + 1];
    VkAttachmentReference cref[MAX_COLOR], dref;
    uint32_t n = 0;
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (k->cfmt[i] == VK_FORMAT_UNDEFINED) {
            cref[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
            continue;
        }
        at[n] = (VkAttachmentDescription){
            .format = k->cfmt[i], .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = k->cload[i], .storeOp = k->cstore[i],
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = k->clayout[i], .finalLayout = k->clayout[i] };
        cref[i] = (VkAttachmentReference){ n, k->clayout[i] };
        n++;
    }
    const bool depth = k->dfmt != VK_FORMAT_UNDEFINED;
    if (depth) {
        at[n] = (VkAttachmentDescription){
            .format = k->dfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = k->dload, .storeOp = k->dstore,
            .stencilLoadOp = k->sload, .stencilStoreOp = k->sstore,
            .initialLayout = k->dlayout, .finalLayout = k->dlayout };
        dref = (VkAttachmentReference){ n, k->dlayout };
        n++;
    }
    VkSubpassDescription sp = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = k->ncolor, .pColorAttachments = cref,
        .pDepthStencilAttachment = depth ? &dref : NULL };
    VkRenderPassCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = n, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp };
    if (R.vkCreateRenderPass(g_dev, &ci, NULL, &rp) != VK_SUCCESS) {
        hle_log("hakuX-hle-compat", "vkCreateRenderPass failed (%u colour, depth %d)", k->ncolor,
                (int)k->dfmt);
        return VK_NULL_HANDLE;
    }
    g_hash_table_insert(g_rps, (gpointer)(uintptr_t)h, (gpointer)rp);
    return rp;
}

/* A pipeline's render pass: compatibility only looks at the formats. */
static VkRenderPass compat_pass(uint32_t ncolor, const VkFormat *cf, VkFormat df, VkFormat sf)
{
    RpKey k;
    memset(&k, 0, sizeof(k));
    k.ncolor = MIN(ncolor, (uint32_t)MAX_COLOR);
    for (uint32_t i = 0; i < k.ncolor; i++) {
        k.cfmt[i] = cf[i];
        k.cload[i] = VK_ATTACHMENT_LOAD_OP_LOAD;
        k.cstore[i] = VK_ATTACHMENT_STORE_OP_STORE;
        k.clayout[i] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    k.dfmt = df != VK_FORMAT_UNDEFINED ? df : sf;
    if (k.dfmt != VK_FORMAT_UNDEFINED) {
        k.dload = k.sload = VK_ATTACHMENT_LOAD_OP_LOAD;
        k.dstore = k.sstore = VK_ATTACHMENT_STORE_OP_STORE;
        k.dlayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    hle_mutex_lock(&g_mx);
    VkRenderPass rp = render_pass(&k);
    hle_mutex_unlock(&g_mx);
    return rp;
}

/* ---- shader modules: kept while a pipeline template may build variants ------- */

static GHashTable *g_mods;              /* VkShaderModule -> refs */

static VkResult VKAPI_CALL c_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci,
                                                const VkAllocationCallbacks *a, VkShaderModule *m)
{
    VkResult r = R.vkCreateShaderModule(d, ci, a, m);
    if (r == VK_SUCCESS) {
        hle_mutex_lock(&g_mx);
        g_hash_table_replace(g_mods, (gpointer)*m, GUINT_TO_POINTER(1));
        hle_mutex_unlock(&g_mx);
    }
    return r;
}

static void mod_unref_locked(VkShaderModule m)
{
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(g_mods, (gpointer)m));
    if (n > 1) {
        g_hash_table_replace(g_mods, (gpointer)m, GUINT_TO_POINTER(n - 1));
        return;
    }
    g_hash_table_remove(g_mods, (gpointer)m);
    R.vkDestroyShaderModule(g_dev, m, NULL);
}

static void VKAPI_CALL c_DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    hle_mutex_lock(&g_mx);
    mod_unref_locked(m);
    hle_mutex_unlock(&g_mx);
}

/* ---- pipeline templates and their variants ---------------------------------- */

/* The dynamic states the layer bakes in (the others are Vulkan 1.0's). */
enum {
    D_CULL = 1 << 0, D_FRONT = 1 << 1, D_TOPO = 1 << 2, D_DTEST = 1 << 3, D_DWRITE = 1 << 4,
    D_DCMP = 1 << 5, D_STEST = 1 << 6, D_SOP = 1 << 7, D_VI = 1 << 8, D_DBIAS_EN = 1 << 9,
    D_RESTART = 1 << 10,
};

#define MAX_VI 16

typedef struct DynState {
    uint8_t cull, front, topo, dtest, dwrite, dcmp, stest, dbias_en, restart, pad[3];
    uint8_t sop[2][4];                  /* front, back: fail, pass, depth fail, compare */
    uint32_t nvb, nva;
    VkVertexInputBindingDescription vb[MAX_VI];
    VkVertexInputAttributeDescription va[MAX_VI];
} DynState;

#define TPL_MAGIC 0x54504c31u

typedef struct Tpl {
    uint32_t magic;
    uint32_t dyn;                       /* D_* the pipeline asked for */
    HleMutex mx;
    VkPipeline fixed;                   /* no D_* state: the one pipeline */
    GHashTable *variants;               /* key hash -> VkPipeline */
    /* the create info, deep */
    VkPipelineCreateFlags flags;
    uint32_t nstage;
    VkPipelineShaderStageCreateInfo st[2];
    char names[2][32];
    VkSpecializationInfo spec[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkVertexInputBindingDescription vb[MAX_VI];
    VkVertexInputAttributeDescription va[MAX_VI];
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv;
    bool has_pv;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    bool has_ds;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineColorBlendAttachmentState cba[MAX_COLOR];
    VkDynamicState dyns[24];
    uint32_t ndyn;
    VkPipelineLayout layout;
    VkPipelineCache cache;
    VkRenderPass rp;
} Tpl;

static GHashTable *g_tpls;              /* Tpl * set: a VkPipeline the layer made */

static bool is_tpl(VkPipeline p)
{
    if (!p) {
        return false;
    }
    hle_mutex_lock(&g_mx);
    bool r = g_hash_table_contains(g_tpls, (gpointer)p);
    hle_mutex_unlock(&g_mx);
    return r;
}

static VkPipeline build(Tpl *t, const DynState *s)
{
    VkPipelineVertexInputStateCreateInfo vi = t->vi;
    if (t->dyn & D_VI) {
        vi.vertexBindingDescriptionCount = s->nvb;
        vi.pVertexBindingDescriptions = s->vb;
        vi.vertexAttributeDescriptionCount = s->nva;
        vi.pVertexAttributeDescriptions = s->va;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = t->ia;
    if (t->dyn & D_TOPO) ia.topology = s->topo;
    if (t->dyn & D_RESTART) ia.primitiveRestartEnable = s->restart;
    VkPipelineRasterizationStateCreateInfo rs = t->rs;
    if (t->dyn & D_CULL) rs.cullMode = s->cull;
    if (t->dyn & D_FRONT) rs.frontFace = s->front;
    if (t->dyn & D_DBIAS_EN) rs.depthBiasEnable = s->dbias_en;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = t->pv;
    if (t->has_pv) {
        pv.pNext = NULL;
        rs.pNext = &pv;
    }
    VkPipelineDepthStencilStateCreateInfo ds = t->ds;
    if (t->dyn & D_DTEST) ds.depthTestEnable = s->dtest;
    if (t->dyn & D_DWRITE) ds.depthWriteEnable = s->dwrite;
    if (t->dyn & D_DCMP) ds.depthCompareOp = s->dcmp;
    if (t->dyn & D_STEST) ds.stencilTestEnable = s->stest;
    if (t->dyn & D_SOP) {
        VkStencilOpState *o[2] = { &ds.front, &ds.back };
        for (int f = 0; f < 2; f++) {
            o[f]->failOp = s->sop[f][0];
            o[f]->passOp = s->sop[f][1];
            o[f]->depthFailOp = s->sop[f][2];
            o[f]->compareOp = s->sop[f][3];
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = t->cb;
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = t->ndyn, .pDynamicStates = t->dyns };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .flags = t->flags & ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT,
        .stageCount = t->nstage, .pStages = t->st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &t->vp, .pRasterizationState = &rs,
        .pMultisampleState = &t->ms, .pDepthStencilState = t->has_ds ? &ds : NULL,
        .pColorBlendState = &cb, .pDynamicState = &dy, .layout = t->layout,
        .renderPass = t->rp, .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = R.vkCreateGraphicsPipelines(g_dev, t->cache, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) {
        hle_log("hakuX-hle-compat", "pipeline variant failed (%d)", r);
        return VK_NULL_HANDLE;
    }
    return p;
}

static uint32_t dyn_bit(VkDynamicState d)
{
    switch (d) {
    case VK_DYNAMIC_STATE_CULL_MODE: return D_CULL;
    case VK_DYNAMIC_STATE_FRONT_FACE: return D_FRONT;
    case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: return D_TOPO;
    case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: return D_DTEST;
    case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: return D_DWRITE;
    case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: return D_DCMP;
    case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE: return D_STEST;
    case VK_DYNAMIC_STATE_STENCIL_OP: return D_SOP;
    case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: return D_VI;
    case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: return D_DBIAS_EN;
    case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: return D_RESTART;
    default: return 0;
    }
}

static const void *find_chain(const void *next, VkStructureType t)
{
    for (const VkBaseInStructure *b = next; b; b = b->pNext) {
        if (b->sType == t) {
            return b;
        }
    }
    return NULL;
}

static VkResult make_tpl(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out)
{
    const VkPipelineRenderingCreateInfo *ri =
        find_chain(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    Tpl *t = g_new0(Tpl, 1);
    t->magic = TPL_MAGIC;
    hle_mutex_init(&t->mx);
    t->flags = ci->flags;
    t->layout = ci->layout;
    t->cache = cache;
    t->nstage = MIN(ci->stageCount, 2u);
    hle_mutex_lock(&g_mx);
    for (uint32_t i = 0; i < t->nstage; i++) {
        t->st[i] = ci->pStages[i];
        t->st[i].pNext = NULL;
        snprintf(t->names[i], sizeof(t->names[i]), "%s", ci->pStages[i].pName);
        t->st[i].pName = t->names[i];
        if (ci->pStages[i].pSpecializationInfo) {
            t->spec[i] = *ci->pStages[i].pSpecializationInfo;
            t->spec[i].pMapEntries = g_memdup2(t->spec[i].pMapEntries,
                                               t->spec[i].mapEntryCount * sizeof(VkSpecializationMapEntry));
            t->spec[i].pData = g_memdup2(t->spec[i].pData, t->spec[i].dataSize);
            t->st[i].pSpecializationInfo = &t->spec[i];
        }
        guint n = GPOINTER_TO_UINT(g_hash_table_lookup(g_mods, (gpointer)t->st[i].module));
        g_hash_table_replace(g_mods, (gpointer)t->st[i].module, GUINT_TO_POINTER(n + 1));
    }
    hle_mutex_unlock(&g_mx);
    t->vi = (VkPipelineVertexInputStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (ci->pVertexInputState) {
        const VkPipelineVertexInputStateCreateInfo *v = ci->pVertexInputState;
        t->vi.vertexBindingDescriptionCount = MIN(v->vertexBindingDescriptionCount, (uint32_t)MAX_VI);
        t->vi.vertexAttributeDescriptionCount = MIN(v->vertexAttributeDescriptionCount, (uint32_t)MAX_VI);
        memcpy(t->vb, v->pVertexBindingDescriptions, t->vi.vertexBindingDescriptionCount * sizeof(t->vb[0]));
        memcpy(t->va, v->pVertexAttributeDescriptions, t->vi.vertexAttributeDescriptionCount * sizeof(t->va[0]));
        t->vi.pVertexBindingDescriptions = t->vb;
        t->vi.pVertexAttributeDescriptions = t->va;
    }
    t->ia = *ci->pInputAssemblyState;
    t->ia.pNext = NULL;
    t->vp = *ci->pViewportState;
    t->vp.pNext = NULL;
    t->rs = *ci->pRasterizationState;
    const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *pv = find_chain(
        t->rs.pNext, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT);
    if (pv) {
        t->pv = *pv;
        t->has_pv = true;
    }
    t->rs.pNext = NULL;                 /* depth clip control: not on these drivers */
    t->ms = *ci->pMultisampleState;
    t->ms.pNext = NULL;
    if (ci->pDepthStencilState) {
        t->ds = *ci->pDepthStencilState;
        t->ds.pNext = NULL;
        t->has_ds = true;
    }
    t->cb = *ci->pColorBlendState;
    t->cb.pNext = NULL;
    t->cb.logicOpEnable = VK_FALSE;     /* no logicOp feature on these drivers */
    t->cb.attachmentCount = MIN(t->cb.attachmentCount, (uint32_t)MAX_COLOR);
    memcpy(t->cba, ci->pColorBlendState->pAttachments, t->cb.attachmentCount * sizeof(t->cba[0]));
    t->cb.pAttachments = t->cba;
    if (ci->pDynamicState) {
        for (uint32_t i = 0; i < ci->pDynamicState->dynamicStateCount; i++) {
            const VkDynamicState d = ci->pDynamicState->pDynamicStates[i];
            const uint32_t b = dyn_bit(d);
            if (b) {
                t->dyn |= b;
            } else if (t->ndyn < ARRAY_SIZE(t->dyns)) {
                t->dyns[t->ndyn++] = d;
            }
        }
    }
    if (ri) {
        t->rp = compat_pass(ri->colorAttachmentCount, ri->pColorAttachmentFormats,
                            ri->depthAttachmentFormat, ri->stencilAttachmentFormat);
    } else {
        t->rp = compat_pass(0, NULL, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED);
    }
    if (!t->rp) {
        g_free(t);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!t->dyn) {
        /* nothing to bake: the pipeline now, as the caller expects */
        DynState s;
        memset(&s, 0, sizeof(s));
        t->fixed = build(t, &s);
        if (!t->fixed) {
            g_free(t);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    } else {
        t->variants = g_hash_table_new(g_int64_hash, g_int64_equal);
    }
    hle_mutex_lock(&g_mx);
    g_hash_table_add(g_tpls, t);
    hle_mutex_unlock(&g_mx);
    *out = (VkPipeline)t;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL c_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                     const VkGraphicsPipelineCreateInfo *ci,
                                                     const VkAllocationCallbacks *a, VkPipeline *p)
{
    VkResult res = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        p[i] = VK_NULL_HANDLE;
        VkResult r = make_tpl(cache, &ci[i], &p[i]);
        if (r != VK_SUCCESS) {
            res = r;
        }
    }
    return res;
}

static void free_variant(gpointer k, gpointer v, gpointer ud)
{
    R.vkDestroyPipeline(g_dev, (VkPipeline)v, NULL);
    g_free(k);
}

static void VKAPI_CALL c_DestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{
    if (!is_tpl(p)) {
        R.vkDestroyPipeline(d, p, a);
        return;
    }
    Tpl *t = (Tpl *)p;
    hle_mutex_lock(&g_mx);
    g_hash_table_remove(g_tpls, t);
    for (uint32_t i = 0; i < t->nstage; i++) {
        mod_unref_locked(t->st[i].module);
        g_free((void *)t->spec[i].pMapEntries);
        g_free((void *)t->spec[i].pData);
    }
    hle_mutex_unlock(&g_mx);
    if (t->fixed) {
        R.vkDestroyPipeline(d, t->fixed, a);
    }
    if (t->variants) {
        g_hash_table_foreach(t->variants, free_variant, NULL);
        g_hash_table_destroy(t->variants);
    }
    hle_mutex_destroy(&t->mx);
    t->magic = 0;
    g_free(t);
}

/* ---- per command buffer: the bound template and the states to bake ---------- */

typedef struct CbState {
    Tpl *tpl;
    VkPipeline bound;                   /* the real pipeline bound */
    bool dirty;
    DynState s;
} CbState;

static GHashTable *g_cbs;               /* VkCommandBuffer -> CbState */
static __thread VkCommandBuffer t_last_cb;
static __thread CbState *t_last_st;

static CbState *cbs(VkCommandBuffer cb)
{
    if (cb == t_last_cb && t_last_st) {
        return t_last_st;
    }
    hle_mutex_lock(&g_mx);
    CbState *s = g_hash_table_lookup(g_cbs, cb);
    if (!s) {
        s = g_new0(CbState, 1);
        g_hash_table_insert(g_cbs, cb, s);
    }
    hle_mutex_unlock(&g_mx);
    t_last_cb = cb;
    t_last_st = s;
    return s;
}

static VkResult VKAPI_CALL c_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    CbState *s = cbs(cb);
    s->tpl = NULL;
    s->bound = VK_NULL_HANDLE;
    s->dirty = false;
    memset(&s->s, 0, sizeof(s->s));
    s->s.cull = VK_CULL_MODE_NONE;
    s->s.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    s->s.dcmp = VK_COMPARE_OP_ALWAYS;
    for (int f = 0; f < 2; f++) {
        s->s.sop[f][3] = VK_COMPARE_OP_ALWAYS;
    }
    return R.vkBeginCommandBuffer(cb, bi);
}

static void VKAPI_CALL c_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS || !is_tpl(p)) {
        R.vkCmdBindPipeline(cb, bp, p);
        return;
    }
    CbState *s = cbs(cb);
    s->tpl = (Tpl *)p;
    s->dirty = true;
}

/* The key of the states the template bakes in. */
static uint64_t dyn_key(const Tpl *t, const DynState *s)
{
    DynState k;
    memset(&k, 0, sizeof(k));
    if (t->dyn & D_CULL) k.cull = s->cull;
    if (t->dyn & D_FRONT) k.front = s->front;
    if (t->dyn & D_TOPO) k.topo = s->topo;
    if (t->dyn & D_DTEST) k.dtest = s->dtest;
    if (t->dyn & D_DWRITE) k.dwrite = s->dwrite;
    if (t->dyn & D_DCMP) k.dcmp = s->dcmp;
    if (t->dyn & D_STEST) k.stest = s->stest;
    if (t->dyn & D_SOP) memcpy(k.sop, s->sop, sizeof(k.sop));
    if (t->dyn & D_DBIAS_EN) k.dbias_en = s->dbias_en;
    if (t->dyn & D_RESTART) k.restart = s->restart;
    uint64_t h = fnv(&k, offsetof(DynState, nvb), FNV0);
    if (t->dyn & D_VI) {
        h = fnv(&s->nvb, sizeof(s->nvb), h);
        h = fnv(&s->nva, sizeof(s->nva), h);
        h = fnv(s->vb, s->nvb * sizeof(s->vb[0]), h);
        h = fnv(s->va, s->nva * sizeof(s->va[0]), h);
    }
    return h;
}

static void flush_pipeline(VkCommandBuffer cb)
{
    CbState *s = cbs(cb);
    if (!s->dirty || !s->tpl) {
        return;
    }
    s->dirty = false;
    Tpl *t = s->tpl;
    VkPipeline p = t->fixed;
    if (!p) {
        int64_t key = (int64_t)dyn_key(t, &s->s);
        hle_mutex_lock(&t->mx);
        p = (VkPipeline)g_hash_table_lookup(t->variants, &key);
        if (!p) {
            p = build(t, &s->s);
            if (p) {
                int64_t *k = g_new(int64_t, 1);
                *k = key;
                g_hash_table_insert(t->variants, k, (gpointer)p);
            }
        }
        hle_mutex_unlock(&t->mx);
    }
    if (p && p != s->bound) {
        R.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        s->bound = p;
    }
}

#define SET(field, val)                         \
    do {                                        \
        CbState *s_ = cbs(cb);                  \
        if (s_->s.field != (val)) {             \
            s_->s.field = (val);                \
            s_->dirty = true;                   \
        }                                       \
    } while (0)

static void VKAPI_CALL c_CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags m) { SET(cull, (uint8_t)m); }
static void VKAPI_CALL c_CmdSetFrontFace(VkCommandBuffer cb, VkFrontFace f) { SET(front, (uint8_t)f); }
static void VKAPI_CALL c_CmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t) { SET(topo, (uint8_t)t); }
static void VKAPI_CALL c_CmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 e) { SET(dtest, (uint8_t)e); }
static void VKAPI_CALL c_CmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 e) { SET(dwrite, (uint8_t)e); }
static void VKAPI_CALL c_CmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp o) { SET(dcmp, (uint8_t)o); }
static void VKAPI_CALL c_CmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 e) { SET(stest, (uint8_t)e); }
static void VKAPI_CALL c_CmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 e) { SET(dbias_en, (uint8_t)e); }
static void VKAPI_CALL c_CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 e) { SET(restart, (uint8_t)e); }

static void VKAPI_CALL c_CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags faces, VkStencilOp fail,
                                         VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp)
{
    CbState *s = cbs(cb);
    for (int f = 0; f < 2; f++) {
        if (!(faces & (f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT))) {
            continue;
        }
        uint8_t v[4] = { (uint8_t)fail, (uint8_t)pass, (uint8_t)dfail, (uint8_t)cmp };
        if (memcmp(s->s.sop[f], v, 4)) {
            memcpy(s->s.sop[f], v, 4);
            s->dirty = true;
        }
    }
}

static void VKAPI_CALL c_CmdSetVertexInputEXT(VkCommandBuffer cb, uint32_t nb,
                                              const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                                              const VkVertexInputAttributeDescription2EXT *at)
{
    CbState *s = cbs(cb);
    DynState n = s->s;
    n.nvb = MIN(nb, (uint32_t)MAX_VI);
    n.nva = MIN(na, (uint32_t)MAX_VI);
    for (uint32_t i = 0; i < n.nvb; i++) {
        n.vb[i] = (VkVertexInputBindingDescription){ b[i].binding, b[i].stride, b[i].inputRate };
    }
    for (uint32_t i = 0; i < n.nva; i++) {
        n.va[i] = (VkVertexInputAttributeDescription){ at[i].location, at[i].binding, at[i].format, at[i].offset };
    }
    if (n.nvb != s->s.nvb || n.nva != s->s.nva || memcmp(n.vb, s->s.vb, n.nvb * sizeof(n.vb[0])) ||
        memcmp(n.va, s->s.va, n.nva * sizeof(n.va[0]))) {
        s->s = n;
        s->dirty = true;
    }
}

static void VKAPI_CALL c_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    flush_pipeline(cb);
    R.vkCmdDraw(cb, vc, ic, fv, fi);
}

static void VKAPI_CALL c_CmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t ic, uint32_t fi,
                                        int32_t vo, uint32_t fin)
{
    flush_pipeline(cb);
    R.vkCmdDrawIndexed(cb, n, ic, fi, vo, fin);
}

/* ---- dynamic rendering -> a render pass ------------------------------------- */

static VkFramebuffer framebuffer(VkRenderPass rp, const VkImageView *views, uint32_t n, uint32_t w,
                                 uint32_t h)
{
    uint64_t key = fnv(&rp, sizeof(rp), FNV0);
    key = fnv(views, n * sizeof(views[0]), key);
    key = fnv(&w, sizeof(w), key);
    key = fnv(&h, sizeof(h), key);
    VkFramebuffer fb = (VkFramebuffer)g_hash_table_lookup(g_fbs, (gpointer)(uintptr_t)key);
    if (fb) {
        return fb;
    }
    VkFramebufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = n, .pAttachments = views, .width = w, .height = h, .layers = 1 };
    if (R.vkCreateFramebuffer(g_dev, &ci, NULL, &fb) != VK_SUCCESS) {
        hle_log("hakuX-hle-compat", "vkCreateFramebuffer failed (%ux%u, %u views)", w, h, n);
        return VK_NULL_HANDLE;
    }
    g_hash_table_insert(g_fbs, (gpointer)(uintptr_t)key, (gpointer)fb);
    g_hash_table_insert(g_fb_keys, (gpointer)fb, (gpointer)(uintptr_t)key);
    for (uint32_t i = 0; i < n; i++) {
        ViewInfo *vi = g_hash_table_lookup(g_views, (gpointer)views[i]);
        if (vi) {
            if (vi->nfb == vi->cap) {
                vi->cap = vi->cap ? vi->cap * 2 : 4;
                vi->fbs = g_realloc(vi->fbs, vi->cap * sizeof(vi->fbs[0]));
            }
            vi->fbs[vi->nfb++] = fb;
        }
    }
    return fb;
}

static void VKAPI_CALL c_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    RpKey k;
    memset(&k, 0, sizeof(k));
    VkImageView views[MAX_COLOR + 1];
    VkClearValue clears[MAX_COLOR + 1];
    uint32_t n = 0;
    uint32_t w = ri->renderArea.offset.x + ri->renderArea.extent.width;
    uint32_t h = ri->renderArea.offset.y + ri->renderArea.extent.height;
    uint32_t fw = UINT32_MAX, fh = UINT32_MAX;
    hle_mutex_lock(&g_mx);
    k.ncolor = MIN(ri->colorAttachmentCount, (uint32_t)MAX_COLOR);
    for (uint32_t i = 0; i < k.ncolor; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        const ViewInfo *vi = a->imageView ? g_hash_table_lookup(g_views, (gpointer)a->imageView) : NULL;
        if (!a->imageView || !vi) {
            k.cfmt[i] = VK_FORMAT_UNDEFINED;
            continue;
        }
        k.cfmt[i] = vi->fmt;
        k.cload[i] = a->loadOp;
        k.cstore[i] = a->storeOp;
        k.clayout[i] = a->imageLayout;
        if (vi->w) { fw = MIN(fw, vi->w); fh = MIN(fh, vi->h); }
        clears[n] = a->clearValue;
        views[n++] = a->imageView;
    }
    const VkRenderingAttachmentInfo *da = ri->pDepthAttachment && ri->pDepthAttachment->imageView
                                              ? ri->pDepthAttachment : NULL;
    const VkRenderingAttachmentInfo *sa = ri->pStencilAttachment && ri->pStencilAttachment->imageView
                                              ? ri->pStencilAttachment : NULL;
    const VkRenderingAttachmentInfo *dsa = da ? da : sa;
    if (dsa) {
        const ViewInfo *vi = g_hash_table_lookup(g_views, (gpointer)dsa->imageView);
        if (vi) {
            k.dfmt = vi->fmt;
            k.dlayout = dsa->imageLayout;
            /* an aspect without its attachment keeps what it holds */
            k.dload = da ? da->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD;
            k.dstore = da ? da->storeOp : VK_ATTACHMENT_STORE_OP_STORE;
            k.sload = sa ? sa->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD;
            k.sstore = sa ? sa->storeOp : VK_ATTACHMENT_STORE_OP_STORE;
            if (vi->w) { fw = MIN(fw, vi->w); fh = MIN(fh, vi->h); }
            clears[n].depthStencil.depth = da ? da->clearValue.depthStencil.depth : 1.0f;
            clears[n].depthStencil.stencil = sa ? sa->clearValue.depthStencil.stencil :
                                             da ? da->clearValue.depthStencil.stencil : 0;
            views[n++] = dsa->imageView;
        }
    }
    if (fw != UINT32_MAX) {
        w = fw;                         /* the views' size: one framebuffer for every area */
        h = fh;
    }
    VkRenderPass rp = render_pass(&k);
    VkFramebuffer fb = rp ? framebuffer(rp, views, n, w, h) : VK_NULL_HANDLE;
    hle_mutex_unlock(&g_mx);
    if (!fb) {
        return;
    }
    VkRenderPassBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = ri->renderArea, .clearValueCount = n, .pClearValues = clears };
    R.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    CbState *s = cbs(cb);
    s->bound = VK_NULL_HANDLE;          /* rebind the variant inside the pass */
    s->dirty = s->tpl != NULL;
}

static void VKAPI_CALL c_CmdEndRendering(VkCommandBuffer cb)
{
    R.vkCmdEndRenderPass(cb);
}

/* ---- timeline semaphores on the host ---------------------------------------- */

#define TL_MAGIC 0x544c5331u
#define TL_PENDING 64

typedef struct TlSem {
    uint32_t magic;
    HleMutex mx;
    uint64_t completed;
    struct { uint64_t value; VkFence fence; uint32_t waiters; } p[TL_PENDING];
    uint32_t np;                        /* pending submits, in value order */
    VkFence free[TL_PENDING];
    uint32_t nfree;
} TlSem;

static GHashTable *g_tls;               /* TlSem * set */

static TlSem *tl_of(VkSemaphore s)
{
    if (!s) return NULL;
    hle_mutex_lock(&g_mx);
    TlSem *t = g_hash_table_contains(g_tls, (gpointer)s) ? (TlSem *)s : NULL;
    hle_mutex_unlock(&g_mx);
    return t;
}

/* Advances completed past the submits done; under t->mx. */
static void tl_poll(TlSem *t)
{
    while (t->np && R.vkGetFenceStatus(g_dev, t->p[0].fence) == VK_SUCCESS) {
        if (t->p[0].waiters) {
            t->completed = MAX(t->completed, t->p[0].value);
            break;                      /* its fence stays until the waiters leave */
        }
        t->completed = MAX(t->completed, t->p[0].value);
        R.vkResetFences(g_dev, 1, &t->p[0].fence);
        t->free[t->nfree++] = t->p[0].fence;
        memmove(&t->p[0], &t->p[1], (t->np - 1) * sizeof(t->p[0]));
        t->np--;
    }
}

static VkFence tl_fence(TlSem *t)
{
    if (t->nfree) {
        return t->free[--t->nfree];
    }
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f = VK_NULL_HANDLE;
    R.vkCreateFence(g_dev, &fi, NULL, &f);
    return f;
}

static VkResult VKAPI_CALL c_CreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci,
                                             const VkAllocationCallbacks *a, VkSemaphore *s)
{
    const VkSemaphoreTypeCreateInfo *ti = find_chain(ci->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    if (!ti || ti->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE) {
        VkSemaphoreCreateInfo bin = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .flags = ci->flags };
        return R.vkCreateSemaphore(d, &bin, a, s);
    }
    TlSem *t = g_new0(TlSem, 1);
    t->magic = TL_MAGIC;
    hle_mutex_init(&t->mx);
    t->completed = ti->initialValue;
    hle_mutex_lock(&g_mx);
    g_hash_table_add(g_tls, t);
    hle_mutex_unlock(&g_mx);
    *s = (VkSemaphore)t;
    return VK_SUCCESS;
}

static void VKAPI_CALL c_DestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a)
{
    TlSem *t = tl_of(s);
    if (!t) {
        R.vkDestroySemaphore(d, s, a);
        return;
    }
    hle_mutex_lock(&g_mx);
    g_hash_table_remove(g_tls, t);
    hle_mutex_unlock(&g_mx);
    for (uint32_t i = 0; i < t->np; i++) {
        R.vkDestroyFence(d, t->p[i].fence, NULL);
    }
    for (uint32_t i = 0; i < t->nfree; i++) {
        R.vkDestroyFence(d, t->free[i], NULL);
    }
    hle_mutex_destroy(&t->mx);
    g_free(t);
}

static VkResult VKAPI_CALL c_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence fence)
{
#define MAX_SUB 4
#define MAX_SEM 8
    if (n > MAX_SUB) {
        hle_log("hakuX-hle-compat", "vkQueueSubmit2 of %u submits: more than %u", n, MAX_SUB);
        n = MAX_SUB;
    }
    VkSubmitInfo2 subs[MAX_SUB];
    VkSemaphoreSubmitInfo waits[MAX_SUB][MAX_SEM], sigs[MAX_SUB][MAX_SEM];
    TlSem *sig_t[8];
    uint64_t sig_v[8];
    uint32_t nsig = 0;
    for (uint32_t i = 0; i < n; i++) {
        subs[i] = si[i];
        /* waits on a timeline: every value was signalled by an earlier submit on
         * this one queue, which the host already saw through (the back end waits
         * on the host only) */
        VkSemaphoreSubmitInfo *w = waits[i];
        uint32_t nw = 0;
        for (uint32_t j = 0; j < si[i].waitSemaphoreInfoCount && nw < MAX_SEM; j++) {
            if (!tl_of(si[i].pWaitSemaphoreInfos[j].semaphore)) {
                w[nw++] = si[i].pWaitSemaphoreInfos[j];
            }
        }
        subs[i].waitSemaphoreInfoCount = nw;
        subs[i].pWaitSemaphoreInfos = w;
        VkSemaphoreSubmitInfo *sg = sigs[i];
        uint32_t ns = 0;
        for (uint32_t j = 0; j < si[i].signalSemaphoreInfoCount && ns < MAX_SEM; j++) {
            TlSem *t = tl_of(si[i].pSignalSemaphoreInfos[j].semaphore);
            if (!t) {
                sg[ns++] = si[i].pSignalSemaphoreInfos[j];
            } else if (nsig < ARRAY_SIZE(sig_t)) {
                sig_t[nsig] = t;
                sig_v[nsig++] = si[i].pSignalSemaphoreInfos[j].value;
            }
        }
        subs[i].signalSemaphoreInfoCount = ns;
        subs[i].pSignalSemaphoreInfos = sg;
    }
    VkResult r = R.vkQueueSubmit2KHR(q, n, subs, fence);
    if (r != VK_SUCCESS || !nsig) {
        return r;
    }
    /* the values: one fence after everything above, on this queue */
    for (uint32_t i = 0; i < nsig; i++) {
        TlSem *t = sig_t[i];
        hle_mutex_lock(&t->mx);
        while (t->np == TL_PENDING) {
            hle_mutex_unlock(&t->mx);
            R.vkWaitForFences(g_dev, 1, &t->p[0].fence, VK_TRUE, UINT64_MAX);
            hle_mutex_lock(&t->mx);
            tl_poll(t);
        }
        VkFence f = tl_fence(t);
        VkSubmitInfo2 empty = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        r = R.vkQueueSubmit2KHR(q, 1, &empty, f);
        if (r == VK_SUCCESS) {
            t->p[t->np].value = sig_v[i];
            t->p[t->np].fence = f;
            t->p[t->np].waiters = 0;
            t->np++;
        }
        hle_mutex_unlock(&t->mx);
        if (r != VK_SUCCESS) {
            return r;
        }
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL c_GetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t *v)
{
    TlSem *t = tl_of(s);
    if (!t) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    hle_mutex_lock(&t->mx);
    tl_poll(t);
    *v = t->completed;
    hle_mutex_unlock(&t->mx);
    return VK_SUCCESS;
}

/* One semaphore's value, or VK_TIMEOUT by deadline (ns, monotonic). */
static VkResult tl_wait(TlSem *t, uint64_t value, int64_t deadline)
{
    for (;;) {
        hle_mutex_lock(&t->mx);
        tl_poll(t);
        if (t->completed >= value) {
            hle_mutex_unlock(&t->mx);
            return VK_SUCCESS;
        }
        int idx = -1;
        for (uint32_t i = 0; i < t->np; i++) {
            if (t->p[i].value >= value) {
                idx = (int)i;
                break;
            }
        }
        const int64_t now = hle_now_ns();
        if (now >= deadline) {
            hle_mutex_unlock(&t->mx);
            return VK_TIMEOUT;
        }
        if (idx < 0) {
            /* not submitted yet: a timeline wait may come first */
            hle_mutex_unlock(&t->mx);
            g_usleep(500);
            continue;
        }
        VkFence f = t->p[idx].fence;
        t->p[idx].waiters++;
        hle_mutex_unlock(&t->mx);
        VkResult r = R.vkWaitForFences(g_dev, 1, &f, VK_TRUE, (uint64_t)(deadline - now));
        hle_mutex_lock(&t->mx);
        for (uint32_t i = 0; i < t->np; i++) {
            if (t->p[i].fence == f) {
                t->p[i].waiters--;
                break;
            }
        }
        hle_mutex_unlock(&t->mx);
        if (r != VK_SUCCESS && r != VK_TIMEOUT) {
            return r;
        }
    }
}

static VkResult VKAPI_CALL c_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    const int64_t now = hle_now_ns();
    const int64_t deadline = timeout >= (uint64_t)INT64_MAX - (uint64_t)now ? INT64_MAX : now + (int64_t)timeout;
    for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
        TlSem *t = tl_of(wi->pSemaphores[i]);
        if (!t) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkResult r = tl_wait(t, wi->pValues[i], deadline);
        if (r != VK_SUCCESS) {
            return r;
        }
        if (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) {
            return VK_SUCCESS;
        }
    }
    return VK_SUCCESS;
}

/* ---- install ---------------------------------------------------------------- */

void hle_compat_install(struct VolkDeviceTable *t, VkDevice dev)
{
    R = *t;
    g_dev = dev;
    g_on = true;
    hle_mutex_init(&g_mx);
    g_images = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_views = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_fbs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_fb_keys = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_rps = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_mods = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_tpls = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_cbs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_tls = g_hash_table_new(g_direct_hash, g_direct_equal);

    /* promoted to 1.2/1.3: the extensions' entry points */
    t->vkCmdPipelineBarrier2 = R.vkCmdPipelineBarrier2KHR;
    t->vkCmdBlitImage2 = R.vkCmdBlitImage2KHR;
    t->vkCmdCopyImage2 = R.vkCmdCopyImage2KHR;
    t->vkResetQueryPool = R.vkResetQueryPoolEXT;
    t->vkGetBufferDeviceAddress = R.vkGetBufferDeviceAddressKHR;

    t->vkCreateImage = c_CreateImage;
    t->vkDestroyImage = c_DestroyImage;
    t->vkCreateImageView = c_CreateImageView;
    t->vkDestroyImageView = c_DestroyImageView;
    t->vkCreateShaderModule = c_CreateShaderModule;
    t->vkDestroyShaderModule = c_DestroyShaderModule;
    t->vkCreateGraphicsPipelines = c_CreateGraphicsPipelines;
    t->vkDestroyPipeline = c_DestroyPipeline;
    t->vkBeginCommandBuffer = c_BeginCommandBuffer;
    t->vkCmdBindPipeline = c_CmdBindPipeline;
    t->vkCmdSetCullMode = c_CmdSetCullMode;
    t->vkCmdSetFrontFace = c_CmdSetFrontFace;
    t->vkCmdSetPrimitiveTopology = c_CmdSetPrimitiveTopology;
    t->vkCmdSetDepthTestEnable = c_CmdSetDepthTestEnable;
    t->vkCmdSetDepthWriteEnable = c_CmdSetDepthWriteEnable;
    t->vkCmdSetDepthCompareOp = c_CmdSetDepthCompareOp;
    t->vkCmdSetStencilTestEnable = c_CmdSetStencilTestEnable;
    t->vkCmdSetStencilOp = c_CmdSetStencilOp;
    t->vkCmdSetDepthBiasEnable = c_CmdSetDepthBiasEnable;
    t->vkCmdSetPrimitiveRestartEnable = c_CmdSetPrimitiveRestartEnable;
    t->vkCmdSetVertexInputEXT = c_CmdSetVertexInputEXT;
    t->vkCmdDraw = c_CmdDraw;
    t->vkCmdDrawIndexed = c_CmdDrawIndexed;
    t->vkCmdBeginRendering = c_CmdBeginRendering;
    t->vkCmdEndRendering = c_CmdEndRendering;
    t->vkCreateSemaphore = c_CreateSemaphore;
    t->vkDestroySemaphore = c_DestroySemaphore;
    t->vkQueueSubmit2 = c_QueueSubmit2;
    t->vkWaitSemaphores = c_WaitSemaphores;
    t->vkGetSemaphoreCounterValue = c_GetSemaphoreCounterValue;
    g_wrapped = *t;
    hle_log("hakuX-hle-compat", "Vulkan 1.1 layer on: dynamic rendering, extended and vertex-input "
            "dynamic state and timeline semaphores emulated");
}
