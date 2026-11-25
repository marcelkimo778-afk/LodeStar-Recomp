/* Implementation of the VMA subset in vk_mem_alloc.h: one VkDeviceMemory per
 * allocation, chosen like VMA does for the usage and flags given - except small
 * GPU-only images, which share 64 MiB blocks (SwBlock below, 2026-11-16). */
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include "vk_mem_alloc.h"

struct VmaAllocator_T {
    VkPhysicalDevice phys;
    VkDevice device;
    VkPhysicalDeviceMemoryProperties mp;
    VmaDeviceMemoryCallbacks cb;
    int has_cb;
    int bda;                                   /* VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT for buffers */
    SRWLOCK lock;
    VkDeviceSize heap_bytes[VK_MAX_MEMORY_HEAPS];
    uint32_t heap_count[VK_MAX_MEMORY_HEAPS];
};

struct SwBlock;
struct VmaAllocation_T {
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint32_t type;
    void *mapped;
    int map_count;
    int persistent;
    struct SwBlock *block;                     /* [arceus] a piece of a shared block (images), else NULL */
    VkDeviceSize offset;                       /* its offset in the block's memory */
};

/* [arceus] Image sub-allocation (2026-11-16).  One vkAllocateMemory per image made streaming in an area cost ~120 ms
 * on NVIDIA (hundreds of new textures in one submission, at the start of a route).  Small GPU-only optimal-tiling images
 * now share 64 MiB blocks per memory type: first fit, aligned to the image's alignment and bufferImageGranularity,
 * neighbours merged on free; an empty block is freed when another empty one of the type exists.  Mapped memory,
 * buffers, large or dedicated-only images keep one allocation each.  SW_VMA_BLOCKS=0 turns it off. */
typedef struct { VkDeviceSize off, size; } SwRange;
typedef struct SwBlock {
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint32_t type;
    SwRange *free_;                            /* sorted by offset, merged */
    int nfree, capfree, live;
} SwBlock;
#define SW_BLOCK_SIZE (64ull << 20)
#define SW_BLOCK_MAX_PIECE (16ull << 20)
static SwBlock **g_blocks;
static int g_nblocks, g_capblocks;
static int g_blocks_on = -1;
static VkDeviceSize g_granularity = 1;

struct VmaPool_T { uint32_t type; };

VkResult vmaCreateAllocator(const VmaAllocatorCreateInfo *ci, VmaAllocator *out)
{
    VmaAllocator a = (VmaAllocator)calloc(1, sizeof *a);
    if (!a) return VK_ERROR_OUT_OF_HOST_MEMORY;
    a->phys = ci->physicalDevice;
    a->device = ci->device;
    vkGetPhysicalDeviceMemoryProperties(a->phys, &a->mp);
    if (ci->pDeviceMemoryCallbacks) { a->cb = *ci->pDeviceMemoryCallbacks; a->has_cb = 1; }
    a->bda = (ci->flags & VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT) != 0;
    InitializeSRWLock(&a->lock);
    *out = a;
    return VK_SUCCESS;
}

void vmaDestroyAllocator(VmaAllocator a) { free(a); }

/* VMA's rules, simplified: required flags must be present; among the rest,
 * the type with the most preferred and the fewest unwanted flags wins. */
static int pick_type(VmaAllocator a, uint32_t bits, const VmaAllocationCreateInfo *ci, uint32_t *out)
{
    VkMemoryPropertyFlags req = ci->requiredFlags, pref = ci->preferredFlags, avoid = 0;
    int host = (ci->flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)) != 0;
    if (ci->memoryTypeBits) bits &= ci->memoryTypeBits;
    switch (ci->usage) {
    case VMA_MEMORY_USAGE_GPU_ONLY: pref |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; break;
    case VMA_MEMORY_USAGE_CPU_ONLY: req |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; break;
    case VMA_MEMORY_USAGE_CPU_TO_GPU: req |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT; pref |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; break;
    case VMA_MEMORY_USAGE_GPU_TO_CPU: req |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT; pref |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT; break;
    case VMA_MEMORY_USAGE_AUTO:
    case VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE:
    case VMA_MEMORY_USAGE_AUTO_PREFER_HOST: {
        int prefer_host = ci->usage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        if (host) {
            req |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            if (ci->flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) pref |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            else if (!prefer_host) pref |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;   /* ReBAR when there is one */
            else avoid |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        } else if (prefer_host) {
            avoid |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        } else {
            pref |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            avoid |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        }
        break;
    }
    default: break;
    }
    int best = -1, best_cost = 1 << 30;
    for (uint32_t i = 0; i < a->mp.memoryTypeCount; i++) {
        if (!(bits >> i & 1)) continue;
        VkMemoryPropertyFlags f = a->mp.memoryTypes[i].propertyFlags;
        if ((f & req) != req) continue;
        int cost = __popcnt(pref & ~f) * 4 + __popcnt(avoid & f);
        if (cost < best_cost) { best_cost = cost; best = (int)i; }
    }
    if (best < 0) return 0;
    *out = (uint32_t)best;
    return 1;
}

static VkResult alloc_mem(VmaAllocator a, VkDeviceSize size, uint32_t type, int buffer_bda, int map,
                          VmaAllocation *out)
{
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = buffer_bda ? &fi : NULL,
                               .allocationSize = size, .memoryTypeIndex = type};
    VmaAllocation al = (VmaAllocation)calloc(1, sizeof *al);
    if (!al) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r = vkAllocateMemory(a->device, &ai, NULL, &al->mem);
    if (r != VK_SUCCESS) { free(al); return r; }
    al->size = size;
    al->type = type;
    if (map) {
        r = vkMapMemory(a->device, al->mem, 0, VK_WHOLE_SIZE, 0, &al->mapped);
        if (r != VK_SUCCESS) { vkFreeMemory(a->device, al->mem, NULL); free(al); return r; }
        al->persistent = 1;
    }
    uint32_t heap = a->mp.memoryTypes[type].heapIndex;
    AcquireSRWLockExclusive(&a->lock);
    a->heap_bytes[heap] += size;
    a->heap_count[heap]++;
    ReleaseSRWLockExclusive(&a->lock);
    if (a->has_cb && a->cb.pfnAllocate) a->cb.pfnAllocate(a, type, al->mem, size, a->cb.pUserData);
    *out = al;
    return VK_SUCCESS;
}

static void info_of(VmaAllocation al, VmaAllocationInfo *info)
{
    if (!info) return;
    memset(info, 0, sizeof *info);
    info->memoryType = al->type;
    info->deviceMemory = al->mem;
    info->offset = al->offset;
    info->size = al->size;
    info->pMappedData = al->mapped;
}

/* a piece of a shared block: 1 with al->mem / block / offset set, 0 = take a dedicated allocation instead.
 * Caller does not hold a->lock. */
static int block_alloc(VmaAllocator a, VkDeviceSize size, VkDeviceSize align, uint32_t type, VmaAllocation al)
{
    if (align < g_granularity) align = g_granularity;
    AcquireSRWLockExclusive(&a->lock);
    for (int pass = 0; pass < 2; pass++) {
        for (int b = 0; b < g_nblocks; b++) {
            SwBlock *k = g_blocks[b];
            if (k->type != type) continue;
            for (int i = 0; i < k->nfree; i++) {
                SwRange r = k->free_[i];
                VkDeviceSize start = (r.off + align - 1) / align * align;
                if (start + size > r.off + r.size) continue;
                VkDeviceSize before = start - r.off, after = r.off + r.size - (start + size);
                if (before && after) {          /* split: the range keeps the part before, a new one after */
                    if (k->nfree == k->capfree) {
                        int cap = k->capfree ? k->capfree * 2 : 16;
                        SwRange *nr = (SwRange *)realloc(k->free_, (size_t)cap * sizeof *nr);
                        if (!nr) { ReleaseSRWLockExclusive(&a->lock); return 0; }
                        k->free_ = nr, k->capfree = cap;
                    }
                    memmove(&k->free_[i + 2], &k->free_[i + 1], (size_t)(k->nfree - i - 1) * sizeof *k->free_);
                    k->free_[i].size = before;
                    k->free_[i + 1] = (SwRange){start + size, after};
                    k->nfree++;
                } else if (before) k->free_[i].size = before;
                else if (after) k->free_[i] = (SwRange){start + size, after};
                else { memmove(&k->free_[i], &k->free_[i + 1], (size_t)(k->nfree - i - 1) * sizeof *k->free_); k->nfree--; }
                k->live++;
                al->mem = k->mem, al->block = k, al->offset = start, al->size = size, al->type = type;
                ReleaseSRWLockExclusive(&a->lock);
                return 1;
            }
        }
        if (pass) break;
        /* no room: a new block (under the lock, so two threads do not both add one) */
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = SW_BLOCK_SIZE,
                                   .memoryTypeIndex = type};
        SwBlock *k = (SwBlock *)calloc(1, sizeof *k);
        SwRange *fr = (SwRange *)malloc(16 * sizeof *fr);
        if (g_nblocks == g_capblocks) {
            int cap = g_capblocks ? g_capblocks * 2 : 32;
            SwBlock **nb = (SwBlock **)realloc(g_blocks, (size_t)cap * sizeof *nb);
            if (nb) g_blocks = nb, g_capblocks = cap;
        }
        if (!k || !fr || g_nblocks == g_capblocks || vkAllocateMemory(a->device, &ai, NULL, &k->mem) != VK_SUCCESS) {
            free(k), free(fr);
            ReleaseSRWLockExclusive(&a->lock);
            return 0;
        }
        k->size = SW_BLOCK_SIZE, k->type = type, k->free_ = fr, k->capfree = 16, k->nfree = 1;
        fr[0] = (SwRange){0, SW_BLOCK_SIZE};
        g_blocks[g_nblocks++] = k;
        uint32_t heap = a->mp.memoryTypes[type].heapIndex;
        a->heap_bytes[heap] += SW_BLOCK_SIZE;
        a->heap_count[heap]++;
        if (a->has_cb && a->cb.pfnAllocate) a->cb.pfnAllocate(a, type, k->mem, SW_BLOCK_SIZE, a->cb.pUserData);
    }
    ReleaseSRWLockExclusive(&a->lock);
    return 0;
}

static void block_free(VmaAllocator a, VmaAllocation al)
{
    SwBlock *k = al->block;
    AcquireSRWLockExclusive(&a->lock);
    int i = 0;
    while (i < k->nfree && k->free_[i].off < al->offset) i++;
    int merged = 0;
    if (i > 0 && k->free_[i - 1].off + k->free_[i - 1].size == al->offset) {   /* joins the range before */
        k->free_[i - 1].size += al->size;
        merged = 1;
        if (i < k->nfree && k->free_[i - 1].off + k->free_[i - 1].size == k->free_[i].off) {   /* and the one after */
            k->free_[i - 1].size += k->free_[i].size;
            memmove(&k->free_[i], &k->free_[i + 1], (size_t)(k->nfree - i - 1) * sizeof *k->free_);
            k->nfree--;
        }
    } else if (i < k->nfree && al->offset + al->size == k->free_[i].off) {   /* joins the range after */
        k->free_[i].off = al->offset, k->free_[i].size += al->size;
        merged = 1;
    }
    if (!merged) {
        if (k->nfree == k->capfree) {
            int cap = k->capfree * 2;
            SwRange *nr = (SwRange *)realloc(k->free_, (size_t)cap * sizeof *nr);
            if (!nr) { ReleaseSRWLockExclusive(&a->lock); return; }   /* the piece stays lost: no corruption */
            k->free_ = nr, k->capfree = cap;
        }
        memmove(&k->free_[i + 1], &k->free_[i], (size_t)(k->nfree - i) * sizeof *k->free_);
        k->free_[i] = (SwRange){al->offset, al->size};
        k->nfree++;
    }
    k->live--;
    SwBlock *dead = NULL;
    if (!k->live) {   /* empty: free it when another empty block of the type is kept */
        for (int b = 0; b < g_nblocks; b++)
            if (g_blocks[b] != k && g_blocks[b]->type == k->type && !g_blocks[b]->live) { dead = k; break; }
        if (dead) {
            for (int b = 0; b < g_nblocks; b++)
                if (g_blocks[b] == dead) { g_blocks[b] = g_blocks[--g_nblocks]; break; }
            uint32_t heap = a->mp.memoryTypes[dead->type].heapIndex;
            a->heap_bytes[heap] -= dead->size;
            a->heap_count[heap]--;
        }
    }
    ReleaseSRWLockExclusive(&a->lock);
    if (dead) {
        if (a->has_cb && a->cb.pfnFree) a->cb.pfnFree(a, dead->type, dead->mem, dead->size, a->cb.pUserData);
        vkFreeMemory(a->device, dead->mem, NULL);
        free(dead->free_);
        free(dead);
    }
}

static int wants_map(const VmaAllocationCreateInfo *ci) { return (ci->flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0; }

VkResult vmaAllocateMemory(VmaAllocator a, const VkMemoryRequirements *req, const VmaAllocationCreateInfo *ci,
                           VmaAllocation *out, VmaAllocationInfo *info)
{
    uint32_t type;
    if (ci->pool) type = ci->pool->type;
    else if (!pick_type(a, req->memoryTypeBits, ci, &type)) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r = alloc_mem(a, req->size, type, a->bda, wants_map(ci), out);
    if (r == VK_SUCCESS) info_of(*out, info);
    return r;
}

void vmaFreeMemory(VmaAllocator a, VmaAllocation al)
{
    if (!al) return;
    if (al->block) { block_free(a, al); free(al); return; }   /* [arceus] a piece of a shared block */
    if (a->has_cb && a->cb.pfnFree) a->cb.pfnFree(a, al->type, al->mem, al->size, a->cb.pUserData);
    uint32_t heap = a->mp.memoryTypes[al->type].heapIndex;
    AcquireSRWLockExclusive(&a->lock);
    a->heap_bytes[heap] -= al->size;
    a->heap_count[heap]--;
    ReleaseSRWLockExclusive(&a->lock);
    if (al->mapped) vkUnmapMemory(a->device, al->mem);
    vkFreeMemory(a->device, al->mem, NULL);
    free(al);
}

VkResult vmaCreateBuffer(VmaAllocator a, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ci,
                         VkBuffer *buf, VmaAllocation *out, VmaAllocationInfo *info)
{
    VkResult r = vkCreateBuffer(a->device, bi, NULL, buf);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(a->device, *buf, &req);
    int bda = a->bda && (bi->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    uint32_t type;
    if (ci->pool) type = ci->pool->type;
    else if (!pick_type(a, req.memoryTypeBits, ci, &type)) { vkDestroyBuffer(a->device, *buf, NULL); return VK_ERROR_FEATURE_NOT_PRESENT; }
    r = alloc_mem(a, req.size, type, bda, wants_map(ci), out);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(a->device, *buf, (*out)->mem, 0);
    if (r != VK_SUCCESS) {
        if (*out) { vmaFreeMemory(a, *out); *out = NULL; }
        vkDestroyBuffer(a->device, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return r;
    }
    info_of(*out, info);
    return VK_SUCCESS;
}

void vmaDestroyBuffer(VmaAllocator a, VkBuffer buf, VmaAllocation al)
{
    if (buf) vkDestroyBuffer(a->device, buf, NULL);
    vmaFreeMemory(a, al);
}

VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ii, const VmaAllocationCreateInfo *ci,
                        VkImage *img, VmaAllocation *out, VmaAllocationInfo *info)
{
    VkResult r = vkCreateImage(a->device, ii, NULL, img);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(a->device, *img, &req);
    uint32_t type;
    if (ci->pool) type = ci->pool->type;
    else if (!pick_type(a, req.memoryTypeBits, ci, &type)) { vkDestroyImage(a->device, *img, NULL); return VK_ERROR_FEATURE_NOT_PRESENT; }
    /* [arceus] a piece of a shared block when allowed (see SwBlock above) */
    if (g_blocks_on < 0) {
        const char *e = getenv("SW_VMA_BLOCKS");
        g_blocks_on = !(e && *e == '0');
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(a->phys, &pp);
        g_granularity = pp.limits.bufferImageGranularity ? pp.limits.bufferImageGranularity : 1;
    }
    *out = NULL;
    if (g_blocks_on && !wants_map(ci) && !ci->pool && ii->tiling == VK_IMAGE_TILING_OPTIMAL &&
        req.size <= SW_BLOCK_MAX_PIECE && !(a->mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 r2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
        VkImageMemoryRequirementsInfo2 ri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = *img};
        vkGetImageMemoryRequirements2(a->device, &ri, &r2);
        if (!dr.requiresDedicatedAllocation) {
            VmaAllocation al = (VmaAllocation)calloc(1, sizeof *al);
            if (al && block_alloc(a, req.size, req.alignment, type, al)) {
                r = vkBindImageMemory(a->device, *img, al->mem, al->offset);
                if (r == VK_SUCCESS) { *out = al; info_of(al, info); return VK_SUCCESS; }
                block_free(a, al);
            }
            free(al);
        }
    }
    r = alloc_mem(a, req.size, type, 0, wants_map(ci), out);
    if (r == VK_SUCCESS) r = vkBindImageMemory(a->device, *img, (*out)->mem, 0);
    if (r != VK_SUCCESS) {
        if (*out) { vmaFreeMemory(a, *out); *out = NULL; }
        vkDestroyImage(a->device, *img, NULL);
        *img = VK_NULL_HANDLE;
        return r;
    }
    info_of(*out, info);
    return VK_SUCCESS;
}

void vmaDestroyImage(VmaAllocator a, VkImage img, VmaAllocation al)
{
    if (img) vkDestroyImage(a->device, img, NULL);
    vmaFreeMemory(a, al);
}

VkResult vmaBindBufferMemory(VmaAllocator a, VmaAllocation al, VkBuffer buf)
{ return vkBindBufferMemory(a->device, buf, al->mem, 0); }
VkResult vmaBindImageMemory(VmaAllocator a, VmaAllocation al, VkImage img)
{ return vkBindImageMemory(a->device, img, al->mem, al->offset); }
VkResult vmaBindImageMemory2(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkImage img, const void *next)
{ (void)next; return vkBindImageMemory(a->device, img, al->mem, al->offset + off); }
VkResult vmaBindBufferMemory2(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkBuffer buf, const void *next)
{ (void)next; return vkBindBufferMemory(a->device, buf, al->mem, al->offset + off); }

void vmaGetAllocationInfo(VmaAllocator a, VmaAllocation al, VmaAllocationInfo *info) { (void)a; info_of(al, info); }

VkResult vmaMapMemory(VmaAllocator a, VmaAllocation al, void **pp)
{
    if (al->block) return VK_ERROR_MEMORY_MAP_FAILED;   /* [arceus] block pieces are GPU-only images */
    if (!al->mapped) {
        VkResult r = vkMapMemory(a->device, al->mem, 0, VK_WHOLE_SIZE, 0, &al->mapped);
        if (r != VK_SUCCESS) return r;
    }
    al->map_count++;
    *pp = al->mapped;
    return VK_SUCCESS;
}

void vmaUnmapMemory(VmaAllocator a, VmaAllocation al)
{
    if (al->map_count > 0 && --al->map_count == 0 && !al->persistent) {
        vkUnmapMemory(a->device, al->mem);
        al->mapped = NULL;
    }
}

static VkResult flush_or_inv(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkDeviceSize size, int flush)
{
    if (a->mp.memoryTypes[al->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return VK_SUCCESS;
    VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = al->mem, .offset = 0,
                             .size = VK_WHOLE_SIZE};
    (void)off; (void)size;   /* whole allocation: offsets would need nonCoherentAtomSize rounding */
    return flush ? vkFlushMappedMemoryRanges(a->device, 1, &r) : vkInvalidateMappedMemoryRanges(a->device, 1, &r);
}
VkResult vmaFlushAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkDeviceSize size)
{ return flush_or_inv(a, al, off, size, 1); }
VkResult vmaInvalidateAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkDeviceSize size)
{ return flush_or_inv(a, al, off, size, 0); }

void vmaGetHeapBudgets(VmaAllocator a, VmaBudget *b)
{
    AcquireSRWLockShared(&a->lock);
    for (uint32_t h = 0; h < a->mp.memoryHeapCount; h++) {
        memset(&b[h], 0, sizeof b[h]);
        b[h].statistics.blockCount = a->heap_count[h];
        b[h].statistics.allocationCount = a->heap_count[h];
        b[h].statistics.blockBytes = a->heap_bytes[h];
        b[h].statistics.allocationBytes = a->heap_bytes[h];
        b[h].usage = a->heap_bytes[h];
        b[h].budget = a->mp.memoryHeaps[h].size * 8 / 10;
    }
    ReleaseSRWLockShared(&a->lock);
}

VkResult vmaFindMemoryTypeIndexForBufferInfo(VmaAllocator a, const VkBufferCreateInfo *bi,
                                             const VmaAllocationCreateInfo *ci, uint32_t *out)
{
    VkBuffer b;
    VkResult r = vkCreateBuffer(a->device, bi, NULL, &b);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(a->device, b, &req);
    vkDestroyBuffer(a->device, b, NULL);
    return pick_type(a, req.memoryTypeBits, ci, out) ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult vmaCreatePool(VmaAllocator a, const VmaPoolCreateInfo *ci, VmaPool *out)
{
    (void)a;
    VmaPool p = (VmaPool)calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->type = ci->memoryTypeIndex;
    *out = p;
    return VK_SUCCESS;
}

void vmaDestroyPool(VmaAllocator a, VmaPool p) { (void)a; free(p); }
