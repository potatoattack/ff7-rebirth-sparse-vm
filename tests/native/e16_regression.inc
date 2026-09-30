/* SPDX-License-Identifier: MIT
 * E12: sparse replacement while graphics work is in flight. Required waits
 * must protect the old mapping; unrelated graphics work may overlap.
 * This is a correctness/overlap gate, not a game frame-time benchmark.
 */
#include "e09_native_base.c"
#define BUSY_BYTES (UINT64_C(64)*1024*1024)
static struct buffer busy_a,busy_b,busy_check,old_readback;
static VkCommandPool graphics_pool;
static VkCommandBuffer graphics_cmd;
static VkFence graphics_fence;
static VkSemaphore read_binary,read_timeline;
static uint64_t read_value;
static unsigned busy_copies;

static void record_graphics(bool read_old) {
    VK(vkResetCommandBuffer(graphics_cmd,0));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(graphics_cmd,&begin));
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdFillBuffer(graphics_cmd,busy_a.handle,0,BUSY_BYTES,0x917bea23u);
    vkCmdPipelineBarrier(graphics_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,1,&barrier,0,NULL,0,NULL);
    VkBufferCopy all={.size=BUSY_BYTES};
    for(unsigned i=0;i<busy_copies;++i) {
        vkCmdCopyBuffer(graphics_cmd,i&1u?busy_b.handle:busy_a.handle,
                         i&1u?busy_a.handle:busy_b.handle,1,&all);
        vkCmdPipelineBarrier(graphics_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,1,&barrier,0,NULL,0,NULL);
    }
    VkBufferCopy ends[2]={{.size=32},{.srcOffset=BUSY_BYTES-32,.dstOffset=32,.size=32}};
    vkCmdCopyBuffer(graphics_cmd,busy_copies&1u?busy_b.handle:busy_a.handle,busy_check.handle,2,ends);
    if(read_old) {
        VkBufferCopy edges[TILES*2];
        for(unsigned i=0;i<TILES;++i) {
            edges[2*i]=(VkBufferCopy){.srcOffset=i*TILE_BYTES,.dstOffset=i*16u,.size=8};
            edges[2*i+1]=(VkBufferCopy){.srcOffset=(i+1)*TILE_BYTES-8,.dstOffset=i*16u+8,.size=8};
        }
        vkCmdCopyBuffer(graphics_cmd,sparse_buffer,old_readback.handle,TILES*2,edges);
    }
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(graphics_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&barrier,0,NULL,0,NULL);
    VK(vkEndCommandBuffer(graphics_cmd));
}

static void submit_graphics(VkSemaphore signal,uint64_t value) {
    VK(vkResetFences(device,1,&graphics_fence));
    VkTimelineSemaphoreSubmitInfo tl={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&value};
    VkSubmitInfo info={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext=signal==read_timeline?&tl:NULL,.commandBufferCount=1,.pCommandBuffers=&graphics_cmd,
        .signalSemaphoreCount=signal?1u:0u,.pSignalSemaphores=signal?&signal:NULL};
    VK(vkQueueSubmit(graphics_queue,1,&info,graphics_fence));
}
static void check_busy(void) {
    wait_fence(graphics_fence);
    for(unsigned i=0;i<16;++i)if(((uint32_t *)busy_check.mapped)[i]!=0x917bea23u)
        die("graphics workload readback mismatch");
}
static void initialize_graphics(void) {
    init_buffer(&busy_a,BUSY_BYTES,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
    init_buffer(&busy_b,BUSY_BYTES,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
    init_buffer(&busy_check,64,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    init_buffer(&old_readback,CHECK_BYTES,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=graphics_family};
    VK(vkCreateCommandPool(device,&pi,NULL,&graphics_pool));
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=graphics_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VK(vkAllocateCommandBuffers(device,&ai,&graphics_cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device,&fi,NULL,&graphics_fence));
    VkSemaphoreCreateInfo si={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK(vkCreateSemaphore(device,&si,NULL,&read_binary));
    VkSemaphoreTypeCreateInfo type={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE};
    si.pNext=&type;VK(vkCreateSemaphore(device,&si,NULL,&read_timeline));
    uint64_t elapsed=0;
    for(busy_copies=1;busy_copies<=128;busy_copies*=2) {
        record_graphics(false);uint64_t start=now_ns();submit_graphics(VK_NULL_HANDLE,0);check_busy();
        elapsed=now_ns()-start;
        if(elapsed>=8000000)break;
    }
    if(busy_copies>128 || elapsed>500000000)die("cannot establish bounded graphics overlap workload");
    printf("E12_GRAPHICS_CALIBRATION: copies=%u wall_ms=%.6f bytes_each=%" PRIu64 "\n",
        busy_copies,elapsed/1e6,BUSY_BYTES);
}
static void make_exact(unsigned bank) {
    for(unsigned i=0;i<TILES;++i) {
        expected_bank[i]=bank;expected_tile[i]=tile_offset(2,i);
        binds[i]=(VkSparseMemoryBind){.resourceOffset=i*TILE_BYTES,.size=TILE_BYTES,
            .memory=sparse_memory[bank],.memoryOffset=expected_tile[i]*TILE_BYTES};
    }
}
static uint64_t bind_begin(unsigned count,VkSemaphore wait,uint64_t wait_value) {
    VkSparseBufferMemoryBindInfo bb={.buffer=sparse_buffer,.bindCount=count,.pBinds=binds};
    uint64_t signal_value=application_value+1;
    VkTimelineSemaphoreSubmitInfo tl={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount=wait?1u:0u,.pWaitSemaphoreValues=wait?&wait_value:NULL,
        .signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&signal_value};
    VkBindSparseInfo bi={.sType=VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,.pNext=&tl,
        .waitSemaphoreCount=wait?1u:0u,.pWaitSemaphores=wait?&wait:NULL,
        .bufferBindCount=1,.pBufferBinds=&bb,.signalSemaphoreCount=1,.pSignalSemaphores=&application_timeline};
    VK(vkResetFences(device,1,&sparse_fence));
    uint64_t t=now_ns();VK(vkQueueBindSparse(sparse_queue,1,&bi,sparse_fence));
    application_value=signal_value;return now_ns()-t;
}
static void bind_finish(void) {
    wait_fence(sparse_fence);uint64_t value;
    VK(vkGetSemaphoreCounterValue(device,application_timeline,&value));
    if(value!=application_value)die("bind signal timeline mismatch");
    (void)check_edges();
}
static void ordered_checks(void) {
    unsigned old_bank[TILES],old_tile[TILES];
    for(unsigned test=0;test<12;++test) {
        unsigned kind=test%3,bank=test&1u;
        reset_layout(bank);
        memcpy(old_bank,expected_bank,sizeof old_bank);memcpy(old_tile,expected_tile,sizeof old_tile);
        record_graphics(true);
        VkSemaphore sem=kind==0?read_binary:read_timeline;
        uint64_t value=kind==0?0:++read_value;
        unsigned n=replacements(bank^1u,test&1u);
        /* Test 2 submits the wait before the timeline's signal submission.
         * The Vulkan runtime must defer it, rather than block this host thread
         * or expose the new backing to the old reader.
         */
        uint64_t api;
        if(kind==2) {
            api=bind_begin(n,sem,value);
            submit_graphics(sem,value);
        } else {
            submit_graphics(sem,value);
            VkResult status=vkGetFenceStatus(device,graphics_fence);
            if(status!=VK_NOT_READY)die("required old-reader overlap was not established");
            api=bind_begin(n,sem,value);
        }
        bind_finish();check_busy();
        for(unsigned i=0;i<TILES;++i)for(unsigned w=0;w<4;++w) {
            uint32_t expect=pattern_word(old_bank[i],old_tile[i],w);
            if(((uint32_t *)old_readback.mapped)[i*4+w]!=expect)
                die("old sparse reader saw replacement before its required semaphore wait");
        }
        printf("E12_ORDER_PASS: test=%u kind=%s bind_api_ms=%.6f old_and_new_edges=checked\n",
            test,kind==0?"binary":kind==1?"timeline":"future_timeline",api/1e6);
    }
}
static void overlap_checks(void) {
    FILE *f=fopen("overlap-rounds.csv","w");if(!f)die("open overlap CSV");
    fputs("round,bank,bind_api_ms,mapping_and_readback_ms,graphics_incomplete_after_mapping,batch_entries,batch_jobs,scalar_ops\n",f);
    make_exact(0);(void)bind_begin(TILES,VK_NULL_HANDLE,0);bind_finish();
    for(unsigned round=0;round<16;++round) {
        unsigned bank=(round+1)&1u;record_graphics(false);
        struct kernel_stats kb,ka;struct batch_kernel_stats bb,ba;
        kernel_counts(&kb);batch_kernel_counts(&bb);
        submit_graphics(VK_NULL_HANDLE,0);
        if(vkGetFenceStatus(device,graphics_fence)!=VK_NOT_READY)die("independent graphics overlap was not established");
        make_exact(bank);uint64_t start=now_ns();
        uint64_t api=bind_begin(TILES,VK_NULL_HANDLE,0);bind_finish();uint64_t elapsed=now_ns()-start;
        VkResult status=vkGetFenceStatus(device,graphics_fence);
        if(status!=VK_SUCCESS && status!=VK_NOT_READY)check(status,"graphics fence status");
        check_busy();kernel_counts(&ka);batch_kernel_counts(&ba);
        uint64_t entries=ba.entries-bb.entries,jobs=ba.jobs-bb.jobs;
        if(entries!=TILES*2u || jobs*16!=entries || ka.direct_ops!=kb.direct_ops ||
           ka.errors!=kb.errors || ba.errors!=bb.errors || ba.unsupported!=bb.unsupported)
            die("overlap phase lost exact E07 batching or recorded a mapping error");
        fprintf(f,"%u,%u,%.6f,%.6f,%u,%" PRIu64 ",%" PRIu64 ",0\n",round,bank,api/1e6,elapsed/1e6,
            status==VK_NOT_READY,entries,jobs);fflush(f);
        printf("E12_OVERLAP: round=%u mapping_readback_ms=%.6f graphics_still_pending=%u entries=%" PRIu64 " jobs=%" PRIu64 "\n",
            round,elapsed/1e6,status==VK_NOT_READY,entries,jobs);
    }
    fclose(f);
}
int e12_regression_main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--self-test"))return e09_gate_main(argc,argv);
    if(argc==2 && !strcmp(argv[1],"--exact"))return e09_gate_main(argc,argv);
    if(argc==2 && !strcmp(argv[1],"--mixed"))return e09_gate_main(1,argv);
    if(argc!=1 || geteuid()==0)die("run the E12 native gate as your normal user");
    setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);
    if(!get_cross_mode())die("run with RADV_EXPERIMENTAL=sparse_vm");
    puts("SPARSE_VM_NATIVE: required waits and independent graphics fences");
    init_device();init_resources();fill_tail_patterns();record_edges();initialize_graphics();
    ordered_checks();overlap_checks();
    vkDestroySemaphore(device,read_binary,NULL);vkDestroySemaphore(device,read_timeline,NULL);
    vkDestroyFence(device,graphics_fence,NULL);vkDestroyCommandPool(device,graphics_pool,NULL);
    destroy_buffer(&busy_a);destroy_buffer(&busy_b);destroy_buffer(&busy_check);destroy_buffer(&old_readback);
    vkDestroyCommandPool(device,validation_pool,NULL);destroy_buffer(&validation);
    vkDestroyCommandPool(device,copy_pool,NULL);destroy_buffer(&source);destroy_buffer(&destination);destroy_buffer(&readback);
    vkDestroyFence(device,sparse_fence,NULL);vkDestroyFence(device,copy_fence,NULL);vkDestroyFence(device,validation_fence,NULL);
    vkDestroyBuffer(device,sparse_buffer,NULL);vkFreeMemory(device,sparse_memory[0],NULL);vkFreeMemory(device,sparse_memory[1],NULL);
    vkDestroySemaphore(device,application_timeline,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("FF7_DONE: E12 required-wait, old/new data and independent-graphics gate passed; game smoothness remains untested.");
    return 0;
}
