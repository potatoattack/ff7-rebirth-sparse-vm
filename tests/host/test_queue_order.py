"""Compile the actual patched sparse-submission function with mock boundaries."""
from pathlib import Path
import subprocess,sys,tempfile
if len(sys.argv)!=2:raise SystemExit('usage: python test_queue_order.py PATCHED_MESA_SOURCE')
M=Path(sys.argv[1])
s=(M/'src/amd/vulkan/radv_queue.c').read_text()
start=s.index('static VkResult\nradv_queue_submit_bind_sparse_memory(')
end=s.index('\nstatic VkResult',start+10)
fn=s[start:end]
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
typedef int VkResult;
#define VK_SUCCESS 0
struct radv_device;
struct radeon_winsys { bool ff7_explicit_unmap;
 int (*sparse_bind_begin)(struct radeon_winsys *,bool,void **);
 int (*sparse_bind_end)(struct radeon_winsys *,void *,bool); };
struct radv_device { struct radeon_winsys *ws; int vk; };
struct vk_queue_submit {unsigned buffer_bind_count,image_opaque_bind_count,image_bind_count,wait_count;
 int *buffer_binds,*image_opaque_binds,*image_binds,*waits;};
static int order[16],n,wait_error,map_error,end_error;static bool expect_explicit;
static int vk_sync_wait_many(int *d,unsigned c,int *w,int flags,uint64_t timeout){
 (void)d;(void)c;(void)w;assert(flags==0 && timeout==UINT64_MAX);order[n++]=1;return wait_error;}
static int begin(struct radeon_winsys *w,bool explicit_unmap,void **c){
 (void)w;assert(explicit_unmap==expect_explicit);order[n++]=2;*c=order;return 0;}
static int end(struct radeon_winsys *w,void *c,bool abort){
 (void)w;assert(c==order && abort==(map_error!=0));order[n++]=4;return end_error;}
static int bind(struct radv_device *d,int *b,void *c){(void)d;(void)b;assert(c==order);order[n++]=3;return map_error;}
#define radv_sparse_buffer_bind_memory bind
#define radv_sparse_image_opaque_bind_memory bind
#define radv_sparse_image_bind_memory bind
'''
post=r'''
int main(void){
 struct radeon_winsys ws={.ff7_explicit_unmap=true,.sparse_bind_begin=begin,.sparse_bind_end=end};
 struct radv_device d={.ws=&ws};int record=1;struct vk_queue_submit sub={.buffer_bind_count=1,.buffer_binds=&record,.wait_count=1,.waits=&record};
 expect_explicit=true;assert(!radv_queue_submit_bind_sparse_memory(&d,&sub,true));
 assert(n==4 && order[0]==1 && order[1]==2 && order[2]==3 && order[3]==4);
 n=0;wait_error=-4;assert(radv_queue_submit_bind_sparse_memory(&d,&sub,true)==-4 && n==1);wait_error=0;
 n=0;map_error=-2;end_error=-4;assert(radv_queue_submit_bind_sparse_memory(&d,&sub,true)==-2 && n==4);map_error=end_error=0;
 for(unsigned kind=0;kind<2;++kind){n=0;expect_explicit=false;ws.ff7_explicit_unmap=kind==0;
  assert(!radv_queue_submit_bind_sparse_memory(&d,&sub,kind!=0));assert(n==3 && order[0]==2);}
 n=0;sub.buffer_bind_count=0;ws.ff7_explicit_unmap=true;
 assert(!radv_queue_submit_bind_sparse_memory(&d,&sub,true) && n==0);
 puts("PASS: actual RADV helper honors required waits before mutations, aborts on wait failure, preserves errors, leaves control/dedicated/empty paths implicit");
}
'''
with tempfile.TemporaryDirectory(prefix='e12-queue-test-') as td:
 p=Path(td)/'test.c';p.write_text(pre+fn+post);binary=Path(td)/'test'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
