/* SPDX-License-Identifier: MIT
 * E22: read UNCHANGED sparse regions while disjoint bindings change.
 * This tests a concurrency gap in E12/E20's post-completion data checks.
 * No game process, shader injection or kernel switch is involved.
 */
#include "e22_native_support.c"

#define E22_CASES 6u
#define E22_ROUNDS 12u
#define E22_SAMPLES 128u
#define E22_READER_MAX 128u
#define HOLE UINT32_MAX
static const char *case_name[E22_CASES]={
    "exact_64k", "split_128k", "split_large_physical",
    "resident_to_hole", "hole_to_resident", "replace_4m"};
struct layout { unsigned bank[TILES],tile[TILES]; };
struct scenario { struct layout old,new; unsigned first,count,bind_count; };
static unsigned sample_tiles[E22_SAMPLES],sample_count;
static struct buffer neighbor_output;
static VkEvent reader_started;
static struct buffer probe_output;
static VkCommandBuffer probe_cmd;
static VkFence probe_fence;

static void probe_regions(const struct scenario *c,VkBufferCopy regions[4]) {
    unsigned tiles[2]={c->first,c->first+c->count-1};
    for(unsigned i=0;i<2;++i) {
        regions[2*i]=(VkBufferCopy){.srcOffset=tiles[i]*TILE_BYTES,.dstOffset=i*16u,.size=8};
        regions[2*i+1]=(VkBufferCopy){.srcOffset=(tiles[i]+1)*TILE_BYTES-8,.dstOffset=i*16u+8,.size=8};
    }
}

/* Pure CPU oracle. Initial mappings are contiguous except exact_64k and
 * split_128k. The updated range never intersects a graphics sample.
 */
static struct scenario make_case(unsigned kind,unsigned round) {
    struct scenario c={0};
    c.first=512u+((round*37u)%512u);c.count=kind==5?64u:1u;
    for(unsigned i=0;i<TILES;++i) {
        c.old.bank[i]=kind==4?HOLE:0u;
        c.old.tile[i]=kind==0?tile_offset(2,i):
            kind==1?((i/2u)*38u%TILES)+(i%2u):i;
    }
    c.new=c.old;
    for(unsigned i=c.first;i<c.first+c.count;++i) {
        c.new.bank[i]=kind==3?HOLE:1u;c.new.tile[i]=i;
    }
    if(kind==0 || kind==1) {
        unsigned step=kind==0?1u:2u;
        for(unsigned i=0;i<TILES;i+=step)
            binds[c.bind_count++]=(VkSparseMemoryBind){.resourceOffset=i*TILE_BYTES,
                .size=step*TILE_BYTES,.memory=sparse_memory[0],.memoryOffset=c.old.tile[i]*TILE_BYTES};
    } else {
        binds[c.bind_count++]=(VkSparseMemoryBind){.resourceOffset=0,.size=TILES*TILE_BYTES,
            .memory=kind==4?VK_NULL_HANDLE:sparse_memory[0],.memoryOffset=0};
    }
    sample_count=0;
    for(unsigned i=0;i<TILES;++i) {
        bool edge=(i+8>=c.first && i<c.first+c.count+8);
        bool selected=edge || i%32u==0 || i==TILES-1;
        if(selected && !(i>=c.first && i<c.first+c.count)) {
            if(sample_count>=E22_SAMPLES)die("sample capacity");
            sample_tiles[sample_count++]=i;
        }
    }
    return c;
}
static uint32_t expected_word(const struct layout *l,unsigned i,unsigned w) {
    return l->bank[i]==HOLE?0u:pattern_word(l->bank[i],l->tile[i],w);
}
static int host_test(void) {
    for(unsigned k=0;k<E22_CASES;++k)for(unsigned r=0;r<E22_ROUNDS;++r) {
        struct scenario c=make_case(k,r);
        if(!c.bind_count || c.first+c.count>TILES || sample_count<64)die("case bounds");
        unsigned coverage[TILES]={0};
        for(unsigned b=0;b<c.bind_count;++b) {
            unsigned first=binds[b].resourceOffset/TILE_BYTES,n=binds[b].size/TILE_BYTES;
            if(first+n>TILES || binds[b].memoryOffset+binds[b].size>TILES*TILE_BYTES)die("backing bounds");
            for(unsigned i=first;i<first+n;++i)++coverage[i];
        }
        for(unsigned i=0;i<TILES;++i) {
            if(coverage[i]!=1 || c.old.tile[i]>=TILES || c.new.tile[i]>=TILES)die("layout coverage");
            bool changed=i>=c.first && i<c.first+c.count;
            for(unsigned w=0;w<4;++w)
                if((expected_word(&c.old,i,w)!=expected_word(&c.new,i,w))!=changed)die("old/new oracle");
        }
        for(unsigned i=0;i<sample_count;++i) {
            unsigned tile=sample_tiles[i];
            if(tile>=c.first && tile<c.first+c.count)die("reader overlaps changed range");
            if(i && tile<=sample_tiles[i-1])die("duplicate sample");
        }
        VkBufferCopy regions[4];probe_regions(&c,regions);
        for(unsigned i=0;i<4;++i) {
            if(regions[i].size!=8 || regions[i].dstOffset!=i*8u ||
               regions[i].srcOffset<c.first*TILE_BYTES ||
               regions[i].srcOffset+8>(c.first+c.count)*TILE_BYTES)
                die("probe must read only the changed range into 32 distinct bytes");
        }
    }
    puts("E22_HOST_PASS: 72 layouts; backing bounds, old/new patterns, disjoint neighbours and 32-byte changed-range probes checked.");
    return 0;
}
static void validate_all(const struct layout *l) {
    VK(vkResetFences(device,1,&validation_fence));
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo tl={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount=1,.pWaitSemaphoreValues=&application_value};
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&tl,
        .waitSemaphoreCount=1,.pWaitSemaphores=&application_timeline,.pWaitDstStageMask=&stage,
        .commandBufferCount=1,.pCommandBuffers=&validation_cmd};
    VK(vkQueueSubmit(sparse_queue,1,&si,validation_fence));wait_fence(validation_fence);
    for(unsigned i=0;i<TILES;++i)for(unsigned w=0;w<4;++w) {
        uint32_t want=expected_word(l,i,w),got=((uint32_t *)validation.mapped)[i*4+w];
        if(got!=want) {
            fprintf(stderr,"E22_POST_MISMATCH: tile=%u word=%u expected=%08x got=%08x\n",i,w,want,got);
            die("post-update mapping data mismatch");
        }
    }
}
static void record_probe(const struct scenario *c) {
    VK(vkResetCommandBuffer(probe_cmd,0));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(probe_cmd,&begin));
    VkBufferCopy regions[4];probe_regions(c,regions);
    vkCmdCopyBuffer(probe_cmd,sparse_buffer,probe_output.handle,4,regions);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(probe_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                        0,1,&barrier,0,NULL,0,NULL);
    VK(vkEndCommandBuffer(probe_cmd));
}
static uint64_t run_probe(const struct scenario *c,const struct layout *l) {
    memset(probe_output.mapped,0xa5,32);
    VK(vkResetFences(device,1,&probe_fence));
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo tl={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount=1,.pWaitSemaphoreValues=&application_value};
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&tl,
        .waitSemaphoreCount=1,.pWaitSemaphores=&application_timeline,.pWaitDstStageMask=&stage,
        .commandBufferCount=1,.pCommandBuffers=&probe_cmd};
    uint64_t start=now_ns();
    VK(vkQueueSubmit(sparse_queue,1,&si,probe_fence));wait_fence(probe_fence);
    unsigned tiles[2]={c->first,c->first+c->count-1};
    for(unsigned i=0;i<2;++i)for(unsigned w=0;w<4;++w) {
        uint32_t want=expected_word(l,tiles[i],w),got=((uint32_t *)probe_output.mapped)[i*4+w];
        if(got!=want) {
            fprintf(stderr,"E22_PROBE_MISMATCH: tile=%u word=%u expected=%08x got=%08x\n",tiles[i],w,want,got);
            die("changed-range GPU probe mismatch");
        }
    }
    return now_ns()-start;
}
static void record_neighbor(void) {
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
    for(unsigned slice=0;slice<=busy_copies;++slice) {
        VkBufferCopy regions[E22_SAMPLES*2];
        for(unsigned j=0;j<sample_count;++j) {
            VkDeviceSize dest=((VkDeviceSize)slice*E22_SAMPLES+j)*16;
            regions[j*2]=(VkBufferCopy){.srcOffset=sample_tiles[j]*TILE_BYTES,.dstOffset=dest,.size=8};
            regions[j*2+1]=(VkBufferCopy){.srcOffset=(sample_tiles[j]+1)*TILE_BYTES-8,.dstOffset=dest+8,.size=8};
        }
        vkCmdCopyBuffer(graphics_cmd,sparse_buffer,neighbor_output.handle,sample_count*2,regions);
        if(slice==0)
            vkCmdSetEvent(graphics_cmd,reader_started,VK_PIPELINE_STAGE_TRANSFER_BIT);
        if(slice<busy_copies)
            vkCmdCopyBuffer(graphics_cmd,slice&1u?busy_b.handle:busy_a.handle,
                            slice&1u?busy_a.handle:busy_b.handle,1,&all);
        vkCmdPipelineBarrier(graphics_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,1,&barrier,0,NULL,0,NULL);
    }
    VkBufferCopy ends[2]={{.size=32},{.srcOffset=BUSY_BYTES-32,.dstOffset=32,.size=32}};
    vkCmdCopyBuffer(graphics_cmd,busy_copies&1u?busy_b.handle:busy_a.handle,busy_check.handle,2,ends);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(graphics_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&barrier,0,NULL,0,NULL);
    VK(vkEndCommandBuffer(graphics_cmd));
}
static void start_reader(void) {
    memset(neighbor_output.mapped,0xa5,(busy_copies+1u)*E22_SAMPLES*16u);
    VK(vkResetEvent(device,reader_started));
    submit_graphics(VK_NULL_HANDLE,0);
    uint64_t deadline=now_ns()+FENCE_TIMEOUT_NS;
    for(;;) {
        VkResult progress=vkGetEventStatus(device,reader_started);
        if(progress==VK_EVENT_SET)break;
        if(progress!=VK_EVENT_RESET)check(progress,"reader start event");
        if(now_ns()>deadline)die("first GPU neighbour read did not complete before deadline");
        struct timespec pause={.tv_nsec=100000};nanosleep(&pause,NULL);
    }
    if(vkGetFenceStatus(device,graphics_fence)!=VK_NOT_READY)die("old-neighbour reader was not outstanding before update");
}
static bool reader_pending(void) {
    VkResult r=vkGetFenceStatus(device,graphics_fence);
    if(r!=VK_SUCCESS && r!=VK_NOT_READY)check(r,"neighbour fence");
    return r==VK_NOT_READY;
}
static void finish_reader(const struct layout *l) {
    check_busy();
    for(unsigned slice=0;slice<=busy_copies;++slice)for(unsigned j=0;j<sample_count;++j)for(unsigned w=0;w<4;++w) {
        unsigned tile=sample_tiles[j];
        uint32_t want=expected_word(l,tile,w);
        uint32_t got=((uint32_t *)neighbor_output.mapped)[(slice*E22_SAMPLES+j)*4+w];
        if(got!=want) {
            fprintf(stderr,"E22_NEIGHBOR_MISMATCH: slice=%u tile=%u word=%u expected=%08x got=%08x\n",
                slice,tile,w,want,got);
            die("unchanged neighbor corrupted");
        }
    }
}
static void run_case(unsigned kind,unsigned round) {
    struct scenario c=make_case(kind,round);
    printf("E22_CASE_SETUP: case=%s round=%u\n",case_name[kind],round);
    (void)send_binds(c.bind_count);
    uint64_t full_start=now_ns();validate_all(&c.old);uint64_t full_setup=now_ns()-full_start;
    record_probe(&c);
    uint64_t idle_probe=run_probe(&c,&c.old),baseline_probe=0;
    bool baseline_pending=false;unsigned baseline_attempts=0;
    /* A matched no-rebind control proves that this probe can finish early. */
    for(;;) {
        ++baseline_attempts;record_neighbor();start_reader();
        baseline_probe=run_probe(&c,&c.old);baseline_pending=reader_pending();
        finish_reader(&c.old);
        printf("E22_NO_REBIND: case=%s round=%u attempt=%u copies=%u probe_ms=%.6f reader_pending=%u\n",
            case_name[kind],round,baseline_attempts,busy_copies,baseline_probe/1e6,baseline_pending);
        if(baseline_pending || busy_copies>=E22_READER_MAX)break;
        busy_copies*=2;
        if(busy_copies>E22_READER_MAX)busy_copies=E22_READER_MAX;
    }
    struct kernel_stats kb,ka;struct batch_kernel_stats bb,ba;
    kernel_counts(&kb);batch_kernel_counts(&bb);
    printf("E22_CASE_BEGIN: case=%s round=%u changed_first=%u changed_tiles=%u stable_samples=%u slices=%u\n",
        case_name[kind],round,c.first,c.count,sample_count,busy_copies+1);
    start_reader();
    binds[0]=(VkSparseMemoryBind){.resourceOffset=c.first*TILE_BYTES,.size=c.count*TILE_BYTES,
        .memory=kind==3?VK_NULL_HANDLE:sparse_memory[1],.memoryOffset=kind==3?0:c.first*TILE_BYTES};
    uint64_t start=now_ns();struct sample bound=send_binds(1);uint64_t finish=now_ns();
    bool pending=reader_pending();
    uint64_t probe_ns=run_probe(&c,&c.new),read_end=now_ns();
    bool gpu_pending=reader_pending();
    finish_reader(&c.old);
    uint64_t reader_end=now_ns();
    full_start=now_ns();validate_all(&c.new);uint64_t full_post=now_ns()-full_start;
    kernel_counts(&ka);batch_kernel_counts(&ba);
    if(ka.errors!=kb.errors || ba.errors!=bb.errors)die("mapping error counter");
    printf("E22_CASE_PASS: case=%s round=%u bind_api_ms=%.6f bind_fence_ms=%.6f bind_total_ms=%.6f gpu_read_ms=%.6f reader_tail_ms=%.6f reader_started_before_bind=1 reader_pending_after_bind=%u gpu_new_mapping_checked=1 reader_pending_after_gpu=%u scalar_ops=%" PRIu64 " batch_entries=%" PRIu64 " scalar_jobs=%" PRIu64 " probe_bytes=32 probe_regions=4 idle_probe_ms=%.6f no_rebind_probe_ms=%.6f no_rebind_pending=%u no_rebind_attempts=%u reader_copies=%u full_setup_ms=%.6f full_post_ms=%.6f full_post_checked=1\n",
        case_name[kind],round,bound.submit/1e6,bound.wait/1e6,(finish-start)/1e6,
        probe_ns/1e6,(reader_end-read_end)/1e6,pending,gpu_pending,
        ka.direct_ops-kb.direct_ops,ba.entries-bb.entries,ka.direct_jobs-kb.direct_jobs,
        idle_probe/1e6,baseline_probe/1e6,baseline_pending,baseline_attempts,busy_copies,full_setup/1e6,full_post/1e6);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--self-test"))return host_test();
#ifdef E22_HOST_TEST
    die("host-only binary");
#else
    if(argc!=1 || geteuid()==0)die("run the neighbor test as your normal user");
    setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);
    host_test();init_device();
    if(!get_cross_mode())die("run with RADV_EXPERIMENTAL=sparse_vm");
    init_resources();fill_tail_patterns();record_edges();initialize_graphics();
    VkEventCreateInfo ei={.sType=VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VK(vkCreateEvent(device,&ei,NULL,&reader_started));
    init_buffer(&neighbor_output,(E22_READER_MAX+1u)*E22_SAMPLES*16u,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    init_buffer(&probe_output,32,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=validation_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VK(vkAllocateCommandBuffers(device,&cai,&probe_cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device,&fi,NULL,&probe_fence));
    for(unsigned k=0;k<E22_CASES;++k)for(unsigned r=0;r<E22_ROUNDS;++r)run_case(k,r);
    destroy_buffer(&neighbor_output);
    destroy_buffer(&probe_output);vkDestroyFence(device,probe_fence,NULL);
    vkDestroyEvent(device,reader_started,NULL);
    vkDestroySemaphore(device,read_binary,NULL);vkDestroySemaphore(device,read_timeline,NULL);
    vkDestroyFence(device,graphics_fence,NULL);vkDestroyCommandPool(device,graphics_pool,NULL);
    destroy_buffer(&busy_a);destroy_buffer(&busy_b);destroy_buffer(&busy_check);destroy_buffer(&old_readback);
    vkDestroyCommandPool(device,validation_pool,NULL);destroy_buffer(&validation);
    vkDestroyCommandPool(device,copy_pool,NULL);destroy_buffer(&source);destroy_buffer(&destination);destroy_buffer(&readback);
    vkDestroyFence(device,sparse_fence,NULL);vkDestroyFence(device,copy_fence,NULL);vkDestroyFence(device,validation_fence,NULL);
    vkDestroyBuffer(device,sparse_buffer,NULL);vkFreeMemory(device,sparse_memory[0],NULL);vkFreeMemory(device,sparse_memory[1],NULL);
    vkDestroySemaphore(device,application_timeline,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("FF7_DONE: E22 v3 72 neighbour cases passed with tiny GPU probes and no-rebind controls; review overlap separately.");
#endif
    return 0;
}
