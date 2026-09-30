/* SPDX-License-Identifier: MIT
 * E09 gate: original E07 exact workload plus repeated mixed-size layouts.
 * Replacing one half of two 128 KiB mappings leaves both prefix and suffix
 * data to verify alongside six exact cross-BO replacements per 16-tile block.
 */
#define main e07_exact_main
#include "e12_native_base.c"
#undef main
static unsigned expected_bank[TILES], expected_tile[TILES];
static bool splitting;

static void fill_tail_patterns(void) {
    for(unsigned b=0;b<2;++b) {
        void *ptr;VK(vkMapMemory(device,sparse_memory[b],0,TILES*TILE_BYTES,0,&ptr));
        for(unsigned i=0;i<TILES;++i) {
            uint32_t *tail=(void *)((uint8_t *)ptr+(i+1)*TILE_BYTES-8);
            tail[0]=pattern_word(b,i,2);tail[1]=pattern_word(b,i,3);
        }
        vkUnmapMemory(device,sparse_memory[b]);
    }
}
static void record_edges(void) {
    VK(vkResetCommandBuffer(validation_cmd,0));
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(validation_cmd,&bi));
    VkBufferCopy r[TILES*2];
    for(unsigned i=0;i<TILES;++i) {
        r[2*i]=(VkBufferCopy){.srcOffset=i*TILE_BYTES,.dstOffset=i*16u,.size=8};
        r[2*i+1]=(VkBufferCopy){.srcOffset=(i+1)*TILE_BYTES-8,.dstOffset=i*16u+8,.size=8};
    }
    vkCmdCopyBuffer(validation_cmd,sparse_buffer,validation.handle,TILES*2,r);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(validation_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&barrier,0,NULL,0,NULL);
    VK(vkEndCommandBuffer(validation_cmd));
}
static struct sample check_edges(void) {
    VK(vkResetFences(device,1,&validation_fence));
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo tl={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount=1,.pWaitSemaphoreValues=&application_value};
    VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&tl,
        .waitSemaphoreCount=1,.pWaitSemaphores=&application_timeline,.pWaitDstStageMask=&stage,
        .commandBufferCount=1,.pCommandBuffers=&validation_cmd};
    uint64_t a=now_ns();VK(vkQueueSubmit(sparse_queue,1,&s,validation_fence));
    uint64_t b=now_ns();wait_fence(validation_fence);uint64_t c=now_ns();
    for(unsigned i=0;i<TILES;++i)for(unsigned w=0;w<4;++w) {
        uint32_t expect=pattern_word(expected_bank[i],expected_tile[i],w);
        uint32_t got=((uint32_t *)validation.mapped)[i*4+w];
        if(got!=expect){fprintf(stderr,"MIXED_MISMATCH: tile=%u edge_word=%u expected=%08x got=%08x\n",i,w,expect,got);die("mixed mapping edge readback failed");}
    }
    return (struct sample){a,b-a,c-b,c-a};
}
static struct sample send_binds(unsigned count) {
    VkSparseBufferMemoryBindInfo bb={.buffer=sparse_buffer,.bindCount=count,.pBinds=binds};
    uint64_t value=application_value+1;
    VkTimelineSemaphoreSubmitInfo tl={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&value};
    VkBindSparseInfo bi={.sType=VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,.pNext=&tl,
        .bufferBindCount=1,.pBufferBinds=&bb,.signalSemaphoreCount=1,.pSignalSemaphores=&application_timeline};
    VK(vkResetFences(device,1,&sparse_fence));
    uint64_t a=now_ns();VK(vkQueueBindSparse(sparse_queue,1,&bi,sparse_fence));
    uint64_t b=now_ns();wait_fence(sparse_fence);uint64_t c=now_ns(),reached=0;
    VK(vkGetSemaphoreCounterValue(device,application_timeline,&reached));
    if(reached!=value)die("mixed bind timeline mismatch");
    application_value=value;
    return (struct sample){a,b-a,c-b,c-a};
}
static unsigned build_layout(unsigned bank) {
    unsigned count=0;
    for(unsigned i=0;i<TILES;) {
        unsigned local=i%16;
        bool pair=local==0 || local==2;
        unsigned length=pair?2:1;
        unsigned backing=pair?bank:(bank^1u);
        unsigned offset=pair?(((i/16)*38u+local*2u)&(TILES-2u)):tile_offset(2,i);
        binds[count++]=(VkSparseMemoryBind){.resourceOffset=i*TILE_BYTES,.size=length*TILE_BYTES,
            .memory=sparse_memory[backing],.memoryOffset=offset*TILE_BYTES};
        for(unsigned j=0;j<length;++j){expected_bank[i+j]=backing;expected_tile[i+j]=offset+j;}
        i+=length;
    }
    return count;
}
static void reset_layout(unsigned bank) {
    (void)send_binds(build_layout(bank));(void)check_edges();
}
static unsigned replacements(unsigned bank,unsigned parity) {
    unsigned count=0;
    for(unsigned g=0;g<TILES;g+=16) {
        unsigned slots[8]={parity?1u:0u,parity?2u:3u,4,5,6,7,8,9};
        for(unsigned j=0;j<8;++j) {
            unsigned i=g+slots[j],offset=tile_offset(2,i);
            binds[count++]=(VkSparseMemoryBind){.resourceOffset=i*TILE_BYTES,.size=TILE_BYTES,
                .memory=sparse_memory[bank],.memoryOffset=offset*TILE_BYTES};
            expected_bank[i]=bank;expected_tile[i]=offset;
        }
    }
    return count;
}
static int e09_gate_main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--self-test")) {
        int result=e07_exact_main(argc,argv);
        for(unsigned bank=0;bank<2;++bank)for(unsigned parity=0;parity<2;++parity) {
            unsigned n=build_layout(bank);if(n!=TILES*7/8)die("layout bind count");
            unsigned large=0;for(unsigned i=0;i<n;++i) {
                large+=binds[i].size==2*TILE_BYTES;
                if(binds[i].memoryOffset+binds[i].size>TILES*TILE_BYTES)die("layout offset bounds");
            }
            if(large!=TILES/8 || replacements(bank,parity)!=TILES/2)die("layout structure");
            for(unsigned g=0;g<TILES;g+=16) {
                unsigned a=g+(parity?0u:1u),b=g+(parity?3u:2u);
                unsigned old=((g/16)*38u)&(TILES-2u);
                if(expected_bank[a]!=bank || expected_bank[b]!=bank ||
                   expected_tile[a]!=old+(a-g) || expected_tile[b]!=old+4+(b-g-2))die("untouched prefix/suffix oracle");
            }
        }
        puts("PASS: mixed 64K/128K layout, partial prefix/suffix preservation and bounded offsets");return result;
    }
    if(argc==2 && !strcmp(argv[1],"--exact"))return e07_exact_main(1,argv);
    if(argc!=1 || geteuid()==0)die("run the E09 native gate as your normal user");
    setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);
    if(!get_cross_mode())die("run with RADV_EXPERIMENTAL=sparse_vm");
    splitting=false;
    printf("E09_MIXED: split=%u rounds=%u old_ranges=64K_and_128K kernel=unchanged_E07\n",splitting,ROUNDS);
    init_device();init_resources();fill_tail_patterns();record_edges();
    FILE *f=fopen("mixed-rounds.csv","w");if(!f)die("open mixed-rounds.csv");
    fputs("round,split,requests,accepted_entries,scalar_ops,batch_attempts,batch_jobs,scalar_jobs,bind_api_ms,bind_wait_ms,gpu_validation_wait_ms,total_ms\n",f);
    uint64_t sum_jobs=0,sum_entries=0,sum_scalar=0,total_ns=0;
    for(unsigned round=0;round<ROUNDS;++round) {
        unsigned bank=round&1u;reset_layout(bank);
        unsigned count=replacements(bank,round&1u);
        struct kernel_stats kb,ka;struct batch_kernel_stats bb,ba;
        kernel_counts(&kb);batch_kernel_counts(&bb);
        uint64_t start=now_ns();struct sample s=send_binds(count);struct sample v=check_edges();uint64_t done=now_ns();
        kernel_counts(&ka);batch_kernel_counts(&ba);
        uint64_t entries=ba.entries-bb.entries,scalar=ka.direct_ops-kb.direct_ops;
        uint64_t jobs=ba.jobs-bb.jobs+ka.direct_jobs-kb.direct_jobs;
        uint64_t requests=2u*count;
        if(ka.errors!=kb.errors || ba.errors!=bb.errors || ka.control_ops!=kb.control_ops || get_cross_mode()!=1)
            die("mixed gate kernel mode/error failure");
        if(entries+scalar!=requests)die("mixed gate original mapping counts disagree");
        if(!splitting && (entries || scalar!=requests || ba.unsupported==bb.unsupported))
            die("control did not exercise whole-vector fallback");
        if(splitting && (entries<requests/2 || !scalar || jobs>=requests))
            die("adaptive mixed gate did not preserve difficult entries while reducing jobs");
        fprintf(f,"%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%.6f,%.6f\n",
            round,splitting,requests,entries,scalar,(ba.batches-bb.batches)+(ba.unsupported-bb.unsupported),
            ba.jobs-bb.jobs,ka.direct_jobs-kb.direct_jobs,s.submit/1e6,s.wait/1e6,v.wait/1e6,(done-start)/1e6);
        fflush(f);sum_jobs+=jobs;sum_entries+=entries;sum_scalar+=scalar;total_ns+=done-start;
    }
    printf("MIXED_RESULT: split=%u entries=%" PRIu64 " scalar=%" PRIu64 " jobs=%" PRIu64 " total_ms=%.6f rounds=%u\n",splitting,sum_entries,sum_scalar,sum_jobs,total_ns/1e6,ROUNDS);
    fclose(f);
    vkDestroyCommandPool(device,validation_pool,NULL);destroy_buffer(&validation);
    vkDestroyCommandPool(device,copy_pool,NULL);destroy_buffer(&readback);destroy_buffer(&destination);destroy_buffer(&source);
    vkDestroyFence(device,validation_fence,NULL);vkDestroyFence(device,copy_fence,NULL);vkDestroyFence(device,sparse_fence,NULL);
    vkDestroyBuffer(device,sparse_buffer,NULL);vkFreeMemory(device,sparse_memory[0],NULL);vkFreeMemory(device,sparse_memory[1],NULL);
    vkDestroySemaphore(device,application_timeline,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("FF7_DONE: E09 mixed-range correctness gate passed; game smoothness remains untested.");return 0;
}
