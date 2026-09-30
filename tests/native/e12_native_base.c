/* SPDX-License-Identifier: MIT
 * E07: cross-BO correctness gate using the unchanged integrated E05 RADV.
 * Each round begins after GPU validation of the preceding generation.
 * The other queue never accesses the sparse allocation. No chunk pacing.
 */
#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan_core.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#define TILES 2048u
#define TILE_BYTES UINT64_C(65536)
#define COPY_BYTES 4096u
#define CHECK_BYTES (TILES * 16u)
#define ROUNDS 12u
#define PERIOD_NS UINT64_C(250000000)
#define PROBE_PERIOD_NS UINT64_C(4000000)
#define FENCE_TIMEOUT_NS UINT64_C(2000000000)
#define MAX_SAMPLES 4096u
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory_props;
static VkQueue sparse_queue, copy_queue, graphics_queue;
static uint32_t graphics_family;
static uint32_t shared_families[2];
static uint32_t sparse_family, sparse_index, copy_family, copy_index;
static VkBuffer sparse_buffer;
static VkDeviceMemory sparse_memory[2];
static unsigned cross_mode;
static VkFence sparse_fence, copy_fence, validation_fence;
static VkSemaphore application_timeline;
static uint64_t application_value;
static VkCommandPool copy_pool, validation_pool;
static VkCommandBuffer copy_cmd, validation_cmd;
struct buffer { VkBuffer handle; VkDeviceMemory memory; void *mapped; };
static struct buffer source, destination, readback, validation;
static VkSparseMemoryBind binds[TILES];
static FILE *samples_file, *rounds_file;
static uint64_t program_origin, global_round;
static uint32_t copy_serial;
static bool nonresident_strict;

static void die(const char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
    fflush(NULL);
    /* Do not enter an unbounded driver wait after a timeout/device loss. */
    _Exit(2);
}

static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "ERROR: %s: VkResult %d\n", what, r);
        fflush(NULL); _Exit(2);
    }
}

#define VK(expr) check((expr), #expr)

static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime failed");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static void sleep_until(uint64_t ns) {
    struct timespec t = { .tv_sec = (time_t)(ns / UINT64_C(1000000000)),
                          .tv_nsec = (long)(ns % UINT64_C(1000000000)) };
    int r;
    do { r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); } while (r == EINTR);
    if (r) die("clock_nanosleep failed");
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags need) {
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & need) == need)
            return i;
    die("required Vulkan memory type unavailable"); return 0;
}

static void wait_fence(VkFence fence) {
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS));
}

static uint32_t tile_offset(unsigned pattern, uint32_t i) {
    if (pattern == 0) return i;
    if (pattern == 1) return TILES - 1u - i;
    /* A bijection modulo 2^11; fixed, reproducible scattered backing. */
    return (i * 109u + 37u) & (TILES - 1u);
}

static void verify_patterns(void) {
    for (unsigned pattern = 0; pattern < 3; ++pattern) {
        bool seen[TILES] = {false};
        for (uint32_t i = 0; i < TILES; ++i) {
            uint32_t j = tile_offset(pattern, i);
            if (j >= TILES || seen[j]) die("invalid tile permutation");
            seen[j] = true;
        }
    }
}

static void init_device(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "FF7 E07 cross-BO test", .applicationVersion = 2,
        .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app };
    VK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) die("no Vulkan devices");
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) die("calloc devices");
    VK(vkEnumeratePhysicalDevices(instance, &count, devices));
    unsigned matches = 0;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceDriverProperties dp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp };
        vkGetPhysicalDeviceProperties2(devices[i], &pp);
        printf("DEVICE: %s; driver=%s; info=%s; vendor=0x%x; device=0x%x\n",
               pp.properties.deviceName, dp.driverName, dp.driverInfo,
               pp.properties.vendorID, pp.properties.deviceID);
        if (pp.properties.vendorID == 0x1002 &&
            pp.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            dp.driverID == VK_DRIVER_ID_MESA_RADV) {
            if (pp.properties.apiVersion < VK_API_VERSION_1_2) die("Vulkan 1.2 is required");
            physical = devices[i]; ++matches;
            nonresident_strict = pp.properties.sparseProperties.residencyNonResidentStrict;
        }
    }
    free(devices);
    if (matches != 1) die("expected exactly one discrete AMD RADV device; select one Vulkan device before testing");
    VkPhysicalDeviceFeatures available;
    vkGetPhysicalDeviceFeatures(physical, &available);
    if (!available.sparseBinding || !available.sparseResidencyBuffer)
        die("sparse binding/residency unavailable");
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof(*qp));
    if (!qp) die("calloc queue properties");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, qp);
    for (uint32_t i = 0; i < nq; ++i)
        printf("QUEUE_FAMILY: %u flags=0x%x count=%u\n", i, qp[i].queueFlags, qp[i].queueCount);
    /* Match the game's default two compute COPY queues when available. */
    bool chosen = false;
    for (uint32_t i = 0; i < nq; ++i) {
        VkQueueFlags f = qp[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) && (f & VK_QUEUE_SPARSE_BINDING_BIT) &&
            !(f & VK_QUEUE_GRAPHICS_BIT) && qp[i].queueCount >= 2) {
            sparse_family = copy_family = i; sparse_index = 1; copy_index = 0;
            chosen = true; break;
        }
    }
    if (!chosen) die("two compute-only sparse-capable queues unavailable; refusing a different test topology");
    bool have_graphics=false;
    for(uint32_t i=0;i<nq;++i)if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) {
        graphics_family=i;have_graphics=true;break;
    }
    if(!have_graphics || graphics_family==sparse_family)die("distinct graphics queue unavailable");
    shared_families[0]=sparse_family;shared_families[1]=graphics_family;
    free(qp);
    float priorities[2] = {0.5f, 0.5f};
    VkDeviceQueueCreateInfo qci[2] = {
        { .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=sparse_family,.queueCount=2,.pQueuePriorities=priorities },
        { .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=graphics_family,.queueCount=1,.pQueuePriorities=priorities }
    };
    VkPhysicalDeviceFeatures enabled = { .sparseBinding = VK_TRUE, .sparseResidencyBuffer = VK_TRUE };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &timeline_feature,
        .queueCreateInfoCount = 2, .pQueueCreateInfos = qci, .pEnabledFeatures = &enabled };
    VK(vkCreateDevice(physical, &dci, NULL, &device));
    VkSemaphoreTypeCreateInfo application_type = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
    VkSemaphoreCreateInfo application_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &application_type };
    VK(vkCreateSemaphore(device, &application_ci, NULL, &application_timeline));
    vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
    printf("E12_GRAPHICS_QUEUE: family=%u index=0\n",graphics_family);
    vkGetDeviceQueue(device, sparse_family, sparse_index, &sparse_queue);
    vkGetDeviceQueue(device, copy_family, copy_index, &copy_queue);
    if (sparse_queue == copy_queue) die("queue handles unexpectedly identical");
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    printf("SELECTED_QUEUES: sparse=%u:%u copy=%u:%u\n",
           sparse_family, sparse_index, copy_family, copy_index);
}

static void init_buffer(struct buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount=2,.pQueueFamilyIndices=shared_families };
    VK(vkCreateBuffer(device, &bi, NULL, &b->handle));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, b->handle, &mr);
    VkMemoryPropertyFlags need = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = memory_type(mr.memoryTypeBits, need) };
    VK(vkAllocateMemory(device, &ai, NULL, &b->memory));
    VK(vkBindBufferMemory(device, b->handle, b->memory, 0));
    if (host) VK(vkMapMemory(device, b->memory, 0, size, 0, &b->mapped));
}
static uint32_t pattern_word(unsigned bank, unsigned tile, unsigned word) {
    return UINT32_C(0x701bac00) ^ (bank * UINT32_C(0x69173ab1)) ^ (tile * UINT32_C(0x12345)) ^ word;
}
static void init_resources(void) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        .size = TILES * TILE_BYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount=2,.pQueueFamilyIndices=shared_families };
    VK(vkCreateBuffer(device, &bi, NULL, &sparse_buffer));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, sparse_buffer, &mr);
    if (!mr.alignment || TILE_BYTES % mr.alignment || mr.size != TILES * TILE_BYTES)
        die("unexpected sparse memory alignment/size");
    uint32_t mt = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = TILES * TILE_BYTES, .memoryTypeIndex = mt };
    for (unsigned bank = 0; bank < 2; ++bank) {
        VK(vkAllocateMemory(device, &ai, NULL, &sparse_memory[bank]));
        void *mapped;
        VK(vkMapMemory(device, sparse_memory[bank], 0, ai.allocationSize, 0, &mapped));
        for (unsigned tile = 0; tile < TILES; ++tile) {
            uint32_t *p = (void *)((uint8_t *)mapped + (uint64_t)tile * TILE_BYTES);
            for (unsigned word = 0; word < 4; ++word) p[word] = pattern_word(bank, tile, word);
        }
        vkUnmapMemory(device, sparse_memory[bank]);
    }
    printf("SPARSE_BUFFER: tiles=%u tile_bytes=%" PRIu64 " allocation_count=2 bytes_each=%" PRIu64 " memory_type=%u strict_nonresident=%u\n",
        TILES, TILE_BYTES, (uint64_t)ai.allocationSize, mt, nonresident_strict);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK(vkCreateFence(device, &fi, NULL, &sparse_fence));
    VK(vkCreateFence(device, &fi, NULL, &copy_fence));
    VK(vkCreateFence(device, &fi, NULL, &validation_fence));
    init_buffer(&source, COPY_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    init_buffer(&destination, COPY_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    init_buffer(&readback, COPY_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    init_buffer(&validation, CHECK_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    for (unsigned i = 0; i < COPY_BYTES / 4; ++i) ((uint32_t *)source.mapped)[i] = UINT32_C(0xc001d00d) ^ i;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = copy_family };
    VK(vkCreateCommandPool(device, &pci, NULL, &copy_pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = copy_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK(vkAllocateCommandBuffers(device, &cai, &copy_cmd));
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(copy_cmd, &begin));
    VkBufferCopy region = { .size = COPY_BYTES };
    vkCmdCopyBuffer(copy_cmd, source.handle, destination.handle, 1, &region);
    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    vkCmdCopyBuffer(copy_cmd, destination.handle, readback.handle, 1, &region);
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(copy_cmd));
    pci.queueFamilyIndex = sparse_family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(vkCreateCommandPool(device, &pci, NULL, &validation_pool));
    cai.commandPool = validation_pool;
    VK(vkAllocateCommandBuffers(device, &cai, &validation_cmd));
}
static void record_validation(bool holes) {
    VK(vkResetCommandBuffer(validation_cmd, 0));
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(validation_cmd, &begin));
    VkBufferCopy regions[TILES];
    unsigned count = 0;
    for (unsigned i = 0; i < TILES; ++i) {
        if (holes && i % 8u == 0 && !nonresident_strict) continue;
        regions[count++] = (VkBufferCopy){ .srcOffset = (uint64_t)i * TILE_BYTES,
            .dstOffset = i * 16u, .size = 16u };
    }
    vkCmdCopyBuffer(validation_cmd, sparse_buffer, validation.handle, count, regions);
    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(validation_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(validation_cmd));
}
struct sample { uint64_t start, submit, wait, total; };
static struct sample validate_mapping(unsigned bank, bool holes, bool mixed) {
    VK(vkResetFences(device, 1, &validation_fence));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo timeline = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &application_value };
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &application_timeline, .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &validation_cmd };
    uint64_t a = now_ns();
    VK(vkQueueSubmit(sparse_queue, 1, &sub, validation_fence));
    uint64_t b = now_ns();
    wait_fence(validation_fence);
    uint64_t c = now_ns();
    for (unsigned i = 0; i < TILES; ++i) {
        bool hole = holes && i % 8u == 0;
        if (hole && !nonresident_strict) continue;
        for (unsigned w = 0; w < 4; ++w) {
            uint32_t expect = hole ? 0 : pattern_word(mixed && (i & 1u) ? bank ^ 1u : bank, tile_offset(2, i), w);
            uint32_t found = ((uint32_t *)validation.mapped)[i * 4u + w];
            if (found != expect) {
                fprintf(stderr, "DATA_MISMATCH: bank=%u virtual_tile=%u word=%u hole=%u expected=%08x got=%08x\n",
                    bank, i, w, hole, expect, found);
                die("sparse mapping validation failed");
            }
        }
    }
    return (struct sample){a,b-a,c-b,c-a};
}

static struct sample do_copy(void) {
    uint32_t token = ++copy_serial;
    ((uint32_t *)source.mapped)[0] = token;
    ((uint32_t *)source.mapped)[COPY_BYTES/4-1] = ~token;
    VK(vkResetFences(device, 1, &copy_fence));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &copy_cmd };
    uint64_t a = now_ns();
    VK(vkQueueSubmit(copy_queue, 1, &sub, copy_fence));
    uint64_t b = now_ns();
    wait_fence(copy_fence);
    uint64_t c = now_ns();
    if (memcmp(source.mapped, readback.mapped, COPY_BYTES)) die("GPU copy/readback data mismatch");
    return (struct sample){ .start = a, .submit = b-a, .wait = c-b, .total = c-a };
}

struct probe_state {
    atomic_bool ready, go, stop;
    uint32_t count;
    struct sample samples[MAX_SAMPLES];
};
static void *probe_thread(void *arg) {
    struct probe_state *p = arg;
    atomic_store(&p->ready, true);
    while (!atomic_load(&p->go)) sleep_until(now_ns() + UINT64_C(100000));
    uint64_t next = now_ns();
    while (!atomic_load(&p->stop)) {
        if (p->count == MAX_SAMPLES) die("copy sample bound reached");
        p->samples[p->count++] = do_copy();
        next += PROBE_PERIOD_NS;
        uint64_t t = now_ns();
        if (next < t) next = t + PROBE_PERIOD_NS;
        sleep_until(next);
    }
    return NULL;
}
static int compare_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static double percentile(uint64_t *x, uint32_t n, double p) {
    if (!n) return 0;
    uint32_t i = (uint32_t)((n-1) * p);
    return (double)x[i] / 1e6;
}

struct round_result { uint64_t bind_api, bind_wait, gpu_submit, gpu_wait, total; };
static struct round_result bind_round(bool holes, bool mixed) {
    unsigned bank = (unsigned)(global_round++ & 1u);
    for (unsigned i = 0; i < TILES; ++i)
        binds[i] = (VkSparseMemoryBind){ .resourceOffset = (uint64_t)i*TILE_BYTES, .size = TILE_BYTES,
            .memory = holes && i % 8u == 0 ? VK_NULL_HANDLE : sparse_memory[mixed && (i & 1u) ? bank ^ 1u : bank],
            .memoryOffset = holes && i % 8u == 0 ? 0 : (uint64_t)tile_offset(2,i)*TILE_BYTES };
    VkSparseBufferMemoryBindInfo bb = {.buffer=sparse_buffer,.bindCount=TILES,.pBinds=binds};
    uint64_t value = application_value + 1;
    VkTimelineSemaphoreSubmitInfo tl = {.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&value};
    VkBindSparseInfo si = {.sType=VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,.pNext=&tl,
        .bufferBindCount=1,.pBufferBinds=&bb,.signalSemaphoreCount=1,.pSignalSemaphores=&application_timeline};
    /* Previous GPU readback fence has completed: no outstanding accesses to
     * this allocation remain. The probe queue accesses different buffers. */
    VK(vkResetFences(device,1,&sparse_fence));
    uint64_t a=now_ns();
    VK(vkQueueBindSparse(sparse_queue,1,&si,sparse_fence));
    uint64_t b=now_ns();
    wait_fence(sparse_fence);
    uint64_t c=now_ns(),reached=0;
    VK(vkGetSemaphoreCounterValue(device,application_timeline,&reached));
    if (reached!=value) die("binding timeline did not reach expected value");
    application_value=value;
    struct sample v=validate_mapping(bank,holes,mixed);
    return (struct round_result){b-a,c-b,v.submit,v.wait,now_ns()-a};
}
struct kernel_stats {
    uint64_t control_ops, direct_ops, control_ranges, direct_ranges;
    uint64_t control_jobs, direct_jobs, eliminated, fallback, errors;
};
static void kernel_counts(struct kernel_stats *s) {
    FILE *f=fopen("/sys/module/amdgpu/parameters/ff7_vm_e03_stats","r");
    if(!f)die("kernel E03 counters missing");
    int n=fscanf(f,"control_ops=%" SCNu64 " direct_ops=%" SCNu64
        " control_ranges=%" SCNu64 " direct_ranges=%" SCNu64
        " control_jobs=%" SCNu64 " direct_jobs=%" SCNu64
        " eliminated=%" SCNu64 " fallback=%" SCNu64 " errors=%" SCNu64,
        &s->control_ops,&s->direct_ops,&s->control_ranges,&s->direct_ranges,
        &s->control_jobs,&s->direct_jobs,&s->eliminated,&s->fallback,&s->errors);
    fclose(f);
    if(n!=9)die("invalid kernel E03 counters");
}
struct batch_kernel_stats { uint64_t batches,entries,ranges,jobs,unsupported,errors; };
static void batch_kernel_counts(struct batch_kernel_stats *s) {
    FILE *f=fopen("/sys/module/amdgpu/parameters/ff7_vm_e04_stats","r");
    if(!f)die("kernel E04 counters missing");
    int n=fscanf(f,"batches=%" SCNu64 " entries=%" SCNu64 " ranges=%" SCNu64
        " jobs=%" SCNu64 " unsupported=%" SCNu64 " errors=%" SCNu64,
        &s->batches,&s->entries,&s->ranges,&s->jobs,&s->unsupported,&s->errors);
    fclose(f);if(n!=6)die("invalid kernel E04 counters");
}
struct cross_stats { uint64_t accepted,other_bo,shape,source,leaf,target,missing,errors,cross_entries; };
/* Request check only. Kernel counters below prove that mapping paths ran. */
static unsigned get_cross_mode(void) {
    const char *s=getenv("RADV_EXPERIMENTAL");
    if(!s)return 0;
    while(*s) {
        s+=strspn(s,", ");
        size_t n=strcspn(s,", ");
        if(n==9 && !strncmp(s,"sparse_vm",n))return 1;
        s+=n;
    }
    return 0;
}

static void cross_counts(struct cross_stats *s) {
    FILE *f=fopen("/sys/module/amdgpu/parameters/ff7_vm_e07_stats","r");
    if(!f)die("E07 counters missing");
    int n=fscanf(f,"accepted=%" SCNu64 " other_bo=%" SCNu64 " shape=%" SCNu64
        " source=%" SCNu64 " leaf=%" SCNu64 " target=%" SCNu64 " missing=%" SCNu64
        " errors=%" SCNu64 " cross_entries=%" SCNu64,
        &s->accepted,&s->other_bo,&s->shape,&s->source,&s->leaf,&s->target,&s->missing,&s->errors,&s->cross_entries);
    fclose(f);if(n!=9)die("invalid E07 counters");
}
static void phase(const char *name,unsigned mode,bool idle) {
    struct cross_stats cb,ca; cross_counts(&cb);
    struct kernel_stats kb,ka; kernel_counts(&kb);
    struct batch_kernel_stats bb,ba;batch_kernel_counts(&bb);
    struct probe_state *p=calloc(1,sizeof(*p));
    if (!p) die("calloc probe state");
    atomic_init(&p->ready,false); atomic_init(&p->go,false); atomic_init(&p->stop,false);
    pthread_t thread;
    if (pthread_create(&thread,NULL,probe_thread,p)) die("pthread_create failed");
    while (!atomic_load(&p->ready)) sleep_until(now_ns()+UINT64_C(100000));
    printf("PHASE_BEGIN: %s mode=%u rounds=%u tiles_per_round=%u\n",name,mode,idle?0:ROUNDS,idle?0:TILES);
    uint64_t start=now_ns(),total=0,max=0,gpu_wait=0;
    unsigned late_rounds=0;
    atomic_store(&p->go,true);
    if (!idle) for (unsigned round=0;round<ROUNDS;++round) {
        sleep_until(start+round*PERIOD_NS);
        uint64_t a=now_ns(),late=a-(start+round*PERIOD_NS);
        struct round_result r=bind_round(false,false);
        total+=r.total;if(r.total>max)max=r.total;gpu_wait+=r.gpu_wait;
        if(late>UINT64_C(5000000))++late_rounds;
        fprintf(rounds_file,"%s,%u,%u,%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
            name,mode,round,a-program_origin,TILES,r.bind_api,r.bind_wait,r.gpu_submit,r.gpu_wait,r.total,late);
    }
    sleep_until(start+ROUNDS*PERIOD_NS);
    atomic_store(&p->stop,true);
    if(pthread_join(thread,NULL))die("pthread_join failed");
    uint64_t end=now_ns();
    kernel_counts(&ka);batch_kernel_counts(&ba);
    uint64_t calls=idle?0:2u*ROUNDS*TILES;
    uint64_t eliminated=ka.eliminated-kb.eliminated,fallback=ka.fallback-kb.fallback;
    uint64_t control_ops=ka.control_ops-kb.control_ops,direct_ops=ka.direct_ops-kb.direct_ops;
    uint64_t control_jobs=ka.control_jobs-kb.control_jobs,direct_jobs=ka.direct_jobs-kb.direct_jobs;
    uint64_t control_ranges=ka.control_ranges-kb.control_ranges,direct_ranges=ka.direct_ranges-kb.direct_ranges;
    printf("KERNEL_PHASE: %s control_ops=%" PRIu64 " direct_ops=%" PRIu64
        " control_ranges=%" PRIu64 " direct_ranges=%" PRIu64
        " control_jobs=%" PRIu64 " direct_jobs=%" PRIu64
        " eliminated=%" PRIu64 " fallback=%" PRIu64 " errors=%" PRIu64 "\n",
        name,control_ops,direct_ops,control_ranges,direct_ranges,control_jobs,direct_jobs,
        eliminated,fallback,ka.errors-kb.errors);
    uint64_t batches=ba.batches-bb.batches,entries=ba.entries-bb.entries;
    uint64_t ranges=ba.ranges-bb.ranges,jobs=ba.jobs-bb.jobs;
    uint64_t unsupported=ba.unsupported-bb.unsupported,errors=ba.errors-bb.errors;
    printf("BATCH_KERNEL_PHASE: %s batches=%" PRIu64 " entries=%" PRIu64 " ranges=%" PRIu64
        " jobs=%" PRIu64 " unsupported=%" PRIu64 " errors=%" PRIu64 "\n",
        name,batches,entries,ranges,jobs,unsupported,errors);
    fflush(rounds_file);
    if(ka.errors!=kb.errors || errors)die("kernel VM update error");
    cross_counts(&ca);
    uint64_t cross_entries=ca.cross_entries-cb.cross_entries;
    uint64_t other_bo=ca.other_bo-cb.other_bo;
    printf("CROSS_KERNEL_PHASE: %s mode=%u cross_entries=%" PRIu64 " rejected_other_bo=%" PRIu64 "\n",
        name,cross_mode,cross_entries,other_bo);
    if(get_cross_mode()!=cross_mode || ca.errors!=cb.errors)die("E07 mode changed or mapping error occurred");
    if(ca.shape!=cb.shape || ca.source!=cb.source || ca.leaf!=cb.leaf ||
       ca.target!=cb.target || ca.missing!=cb.missing)die("unexpected E07 eligibility rejection in exact two-BO phase");
    if(fallback)die("unexpected E03 direct fallback in timed phase");
    if(!cross_mode && (control_ops || control_ranges || control_jobs || direct_ops!=calls ||
        direct_ranges!=calls || direct_jobs!=calls || eliminated!=calls || batches || entries || ranges || jobs ||
        unsupported*16!=calls || other_bo!=unsupported || cross_entries || ca.accepted!=cb.accepted))
        die("E07 narrow control did not fall back exactly as expected for two different BOs");
    if(cross_mode && (control_ops || control_ranges || control_jobs || direct_ops || direct_ranges ||
        direct_jobs || eliminated || entries!=calls || ranges!=calls || jobs!=batches || batches*16!=calls ||
        unsupported || other_bo || cross_entries!=calls || ca.accepted-cb.accepted!=batches))
        die("E07 cross-BO path did not batch the exact two-BO workload");
    uint64_t *times=calloc(p->count?p->count:1,sizeof(*times));
    if(!times)die("calloc times");
    unsigned over5=0,over16=0;
    for(unsigned i=0;i<p->count;++i){
        struct sample s=p->samples[i];times[i]=s.total;over5+=s.total>UINT64_C(5000000);over16+=s.total>UINT64_C(16666667);
        fprintf(samples_file,"%s,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",name,i,s.start-program_origin,s.submit,s.wait,s.total);
    }
    qsort(times,p->count,sizeof(*times),compare_u64);
    printf("PHASE_RESULT: %s mode=%u elapsed_ms=%.3f tile_updates=%u mapping_calls=%" PRIu64
        " batch_jobs=%" PRIu64 " batch_entries=%" PRIu64 " control_jobs=%" PRIu64 " direct_jobs=%" PRIu64
        " copy_samples=%u copy_p50_ms=%.3f copy_p95_ms=%.3f copy_p99_ms=%.3f"
        " copy_max_ms=%.3f copy_gt5ms=%u copy_gt16_67ms=%u round_total_ms=%.3f round_max_ms=%.3f"
        " gpu_validation_wait_ms=%.3f late_rounds=%u\n",
        name,mode,(end-start)/1e6,idle?0:ROUNDS*TILES,calls,jobs,entries,control_jobs,direct_jobs,p->count,
        percentile(times,p->count,.5),percentile(times,p->count,.95),percentile(times,p->count,.99),percentile(times,p->count,1),
        over5,over16,total/1e6,max/1e6,gpu_wait/1e6,late_rounds);
    fflush(samples_file);fflush(rounds_file);free(times);free(p);
}

static void destroy_buffer(struct buffer *b) {
    if (b->mapped) vkUnmapMemory(device, b->memory);
    vkDestroyBuffer(device, b->handle, NULL); vkFreeMemory(device, b->memory, NULL);
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);
    verify_patterns();
    if(argc==2 && !strcmp(argv[1],"--self-test")) {
        for(unsigned i=0;i<TILES;++i)for(unsigned w=0;w<4;++w)
            if(pattern_word(0,i,w)==pattern_word(1,i,w))die("bank patterns overlap");
        puts("PASS: deterministic permutation and distinct bank patterns");return 0;
    }
    if(argc!=1)die("unexpected arguments");
    if(geteuid()==0)die("run the native test as your normal user");
    printf("EXECUTION_UID: real=%u effective=%u\n",(unsigned)getuid(),(unsigned)geteuid());
    program_origin=now_ns();
    samples_file=fopen("copy-samples.csv","w");rounds_file=fopen("mapping-rounds.csv","w");
    if(!samples_file||!rounds_file)die("cannot create output CSVs");
    fputs("phase,sample,start_ns,submit_ns,wait_ns,total_ns\n",samples_file);
    fputs("phase,mode,round,start_ns,tiles,bind_api_ns,bind_fence_wait_ns,gpu_submit_ns,gpu_wait_ns,total_ns,lateness_ns\n",rounds_file);
    if(!get_cross_mode())die("run with RADV_EXPERIMENTAL=sparse_vm");
    cross_mode=get_cross_mode();
    unsigned mode=cross_mode;
    printf("E07_MODE: %s (same E05 RADV, two distinct allocations; no ioctl interposer)\n",cross_mode?"cross":"narrow");
    init_device();init_resources();
    struct kernel_stats preflight_before; kernel_counts(&preflight_before);
    struct batch_kernel_stats pb,pa;batch_kernel_counts(&pb);
    record_validation(false);(void)bind_round(false,false);
    record_validation(true);(void)bind_round(true,false);
    record_validation(false);(void)bind_round(false,false);
    (void)bind_round(false,true); /* mixed old backing within later target vectors */
    (void)bind_round(false,false);
    struct kernel_stats preflight_stats; kernel_counts(&preflight_stats);
    if(preflight_stats.errors!=preflight_before.errors)die("kernel reported an error during preflight");
    batch_kernel_counts(&pa);
    printf("PREFLIGHT_BATCH: batches=%" PRIu64 " entries=%" PRIu64 " jobs=%" PRIu64
        " unsupported=%" PRIu64 " errors=%" PRIu64 "\n",
        pa.batches-pb.batches,pa.entries-pb.entries,pa.jobs-pb.jobs,
        pa.unsupported-pb.unsupported,pa.errors-pb.errors);
    if(pa.errors!=pb.errors)die("batch error during preflight");
    if(cross_mode && (pa.entries==pb.entries || pa.jobs==pb.jobs))
        die("cross-BO preflight batching never activated");
    puts("PREFLIGHT_PASS: two allocations, mixed old backing, partial unmaps, rebinds, timeline and GPU data checks");
    phase(cross_mode?"cross":"narrow",mode,false);
    /* Each queue's final operation completed through a bounded fence wait. */
    vkDestroyCommandPool(device,validation_pool,NULL);destroy_buffer(&validation);
    vkDestroyCommandPool(device,copy_pool,NULL);
    destroy_buffer(&readback);destroy_buffer(&destination);destroy_buffer(&source);
    vkDestroyFence(device,validation_fence,NULL);vkDestroyFence(device,copy_fence,NULL);vkDestroyFence(device,sparse_fence,NULL);
    vkDestroyBuffer(device,sparse_buffer,NULL);vkFreeMemory(device,sparse_memory[0],NULL);vkFreeMemory(device,sparse_memory[1],NULL);
    vkDestroySemaphore(device,application_timeline,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    fclose(samples_file);fclose(rounds_file);
    puts("FF7_DONE: E07 two-BO correctness gate passed; game smoothness remains untested.");
    return 0;
}
