/* SPDX-License-Identifier: MIT
 * E16: legal, completed-use destruction while unrelated graphics is pending.
 * Repeated ordinary memory, sparse-buffer and opaque sparse-image lifetimes.
 * All GPU waits are bounded; this gate cannot establish game smoothness.
 */
#include "e16_regression.inc"

#define LIFE_BYTES UINT64_C(65536)
#define LIFE_ROUNDS 12u
struct life_object {
    unsigned kind;
    VkBuffer buffer;
    VkImage image;
    VkDeviceMemory memory;
};
static struct buffer life_readback;
static VkCommandPool life_pool;
static VkCommandBuffer life_cmd;
static VkFence life_fence;

static void life_create(struct life_object *o,unsigned kind) {
    memset(o,0,sizeof(*o));o->kind=kind;
    VkMemoryRequirements mr;
    if(kind==2) {
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags=VK_IMAGE_CREATE_SPARSE_BINDING_BIT,.imageType=VK_IMAGE_TYPE_2D,
            .format=VK_FORMAT_R32_UINT,.extent={128,128,1},.mipLevels=1,.arrayLayers=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
        VK(vkCreateImage(device,&ci,NULL,&o->image));
        vkGetImageMemoryRequirements(device,o->image,&mr);
    } else {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .flags=kind==1?VK_BUFFER_CREATE_SPARSE_BINDING_BIT:0,.size=LIFE_BYTES,
            .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        VK(vkCreateBuffer(device,&ci,NULL,&o->buffer));
        vkGetBufferMemoryRequirements(device,o->buffer,&mr);
    }
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=mr.size,.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VK(vkAllocateMemory(device,&ai,NULL,&o->memory));
    if(kind==0) {
        VK(vkBindBufferMemory(device,o->buffer,o->memory,0));
    } else {
        VkSparseMemoryBind bind={.size=mr.size,.memory=o->memory};
        VkSparseBufferMemoryBindInfo buffer={.buffer=o->buffer,.bindCount=1,.pBinds=&bind};
        VkSparseImageOpaqueMemoryBindInfo image={.image=o->image,.bindCount=1,.pBinds=&bind};
        VkBindSparseInfo bi={.sType=VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
            .bufferBindCount=kind==1?1u:0u,.pBufferBinds=kind==1?&buffer:NULL,
            .imageOpaqueBindCount=kind==2?1u:0u,.pImageOpaqueBinds=kind==2?&image:NULL};
        VK(vkResetFences(device,1,&sparse_fence));
        VK(vkQueueBindSparse(sparse_queue,1,&bi,sparse_fence));wait_fence(sparse_fence);
    }
}

static void life_use_and_check(struct life_object *o,uint32_t pattern) {
    VK(vkResetCommandBuffer(life_cmd,0));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(life_cmd,&begin));
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    if(o->kind==2) {
        VkImageSubresourceRange range={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1};
        VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=o->image,.subresourceRange=range};
        vkCmdPipelineBarrier(life_cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,NULL,0,NULL,1,&ib);
        VkClearColorValue color={.uint32={pattern,0,0,0}};
        vkCmdClearColorImage(life_cmd,o->image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&range);
        vkCmdPipelineBarrier(life_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,1,&mb,0,NULL,0,NULL);
        VkBufferImageCopy region={.imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},
            .imageExtent={128,128,1}};
        vkCmdCopyImageToBuffer(life_cmd,o->image,VK_IMAGE_LAYOUT_GENERAL,life_readback.handle,1,&region);
    } else {
        vkCmdFillBuffer(life_cmd,o->buffer,0,LIFE_BYTES,pattern);
        vkCmdPipelineBarrier(life_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,1,&mb,0,NULL,0,NULL);
        VkBufferCopy region={.size=LIFE_BYTES};
        vkCmdCopyBuffer(life_cmd,o->buffer,life_readback.handle,1,&region);
    }
    mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(life_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&mb,0,NULL,0,NULL);
    VK(vkEndCommandBuffer(life_cmd));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&life_cmd};
    VK(vkResetFences(device,1,&life_fence));VK(vkQueueSubmit(copy_queue,1,&si,life_fence));
    wait_fence(life_fence);
    for(unsigned i=0;i<LIFE_BYTES/4;++i)
        if(((uint32_t *)life_readback.mapped)[i]!=pattern)die("E16 old/new allocation readback mismatch");
    /* The resource is no longer in GPU use; make the command buffer independent
     * before destruction. The next graphics workload touches only busy_a/b.
     */
    VK(vkResetCommandBuffer(life_cmd,0));
}

static void life_destroy(struct life_object *o) {
    if(o->image)vkDestroyImage(device,o->image,NULL);
    if(o->buffer)vkDestroyBuffer(device,o->buffer,NULL);
    vkFreeMemory(device,o->memory,NULL);memset(o,0,sizeof(*o));
}

static void life_checks(void) {
    init_buffer(&life_readback,LIFE_BYTES,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=copy_family};
    VK(vkCreateCommandPool(device,&pi,NULL,&life_pool));
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=life_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VK(vkAllocateCommandBuffers(device,&ai,&life_cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device,&fi,NULL,&life_fence));
    FILE *f=fopen("destruction-rounds.csv","w");if(!f)die("E16 CSV open failed");
    fputs("kind,round,destroy_cpu_ms,probe_copy_ms,graphics_pending_after_probe,new_data_checked\n",f);
    for(unsigned kind=0;kind<3;++kind)for(unsigned round=0;round<LIFE_ROUNDS;++round) {
        struct life_object old,replacement;
        life_create(&old,kind);life_use_and_check(&old,0x3a291b05u^(round*17u+kind));
        record_graphics(false);submit_graphics(VK_NULL_HANDLE,0);
        if(vkGetFenceStatus(device,graphics_fence)!=VK_NOT_READY)die("E16 independent graphics overlap not established");
        uint64_t start=now_ns();life_destroy(&old);uint64_t elapsed=now_ns()-start;
        struct sample probe=do_copy();
        VkResult status=vkGetFenceStatus(device,graphics_fence);
        if(status!=VK_SUCCESS && status!=VK_NOT_READY)check(status,"E16 graphics fence status");
        /* Reallocate immediately; fresh mappings/readback must remain correct
         * even when the unrelated queue has not finished. No VA identity is
         * inferred from this API-level test.
         */
        life_create(&replacement,kind);life_use_and_check(&replacement,0xb65ced91u^(round*137u+kind));
        check_busy();life_destroy(&replacement);
        fprintf(f,"%u,%u,%.6f,%.6f,%u,1\n",kind,round,elapsed/1e6,probe.total/1e6,status==VK_NOT_READY);fflush(f);
        printf("E16_LIFETIME_PASS: kind=%u round=%u destroy_cpu_ms=%.6f probe_ms=%.6f graphics_pending=%u old_and_new_data=checked\n",
               kind,round,elapsed/1e6,probe.total/1e6,status==VK_NOT_READY);
    }
    fclose(f);vkDestroyFence(device,life_fence,NULL);vkDestroyCommandPool(device,life_pool,NULL);
    destroy_buffer(&life_readback);
}

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--self-test"))return e12_regression_main(argc,argv);
    if(argc==2 && !strcmp(argv[1],"--regression"))return e12_regression_main(1,argv);
    if(argc!=1 || geteuid()==0)die("run E16 native as your normal user");
    if(!get_cross_mode())die("run with RADV_EXPERIMENTAL=sparse_vm");
    setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);
    puts("SPARSE_VM_NATIVE: API-owned cleanup after completed uses");
    init_device();init_resources();fill_tail_patterns();record_edges();initialize_graphics();
    life_checks();
    vkDestroySemaphore(device,read_binary,NULL);vkDestroySemaphore(device,read_timeline,NULL);
    vkDestroyFence(device,graphics_fence,NULL);vkDestroyCommandPool(device,graphics_pool,NULL);
    destroy_buffer(&busy_a);destroy_buffer(&busy_b);destroy_buffer(&busy_check);destroy_buffer(&old_readback);
    vkDestroyCommandPool(device,validation_pool,NULL);destroy_buffer(&validation);
    vkDestroyCommandPool(device,copy_pool,NULL);destroy_buffer(&source);destroy_buffer(&destination);destroy_buffer(&readback);
    vkDestroyFence(device,sparse_fence,NULL);vkDestroyFence(device,copy_fence,NULL);vkDestroyFence(device,validation_fence,NULL);
    vkDestroyBuffer(device,sparse_buffer,NULL);vkFreeMemory(device,sparse_memory[0],NULL);vkFreeMemory(device,sparse_memory[1],NULL);
    vkDestroySemaphore(device,application_timeline,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("FF7_DONE: E16 destruction, completed-use and fresh-allocation data gate passed; game smoothness remains untested.");
    return 0;
}
