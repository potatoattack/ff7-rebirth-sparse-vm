"""Compile the actual RADV eligibility/VA helpers against controlled boundaries."""
from pathlib import Path
import subprocess,sys,tempfile
if len(sys.argv)!=2:raise SystemExit('usage: python test_destroy_scope.py PATCHED_MESA_SOURCE')
M=Path(sys.argv[1])
s=(M/'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c').read_text()
def function(name,kind):
    start=s.index('static '+kind+'\n'+name+'(')
    end=s.index('\n}\n',start)+3
    return s[start:end]
functions='\n'.join([function('radv_amdgpu_ff7_destroy_begin','uint64_t'),
                     function('radv_amdgpu_ff7_destroy_done','void'),
                     function('radv_amdgpu_bo_va_op','int'),
                     function('radv_amdgpu_virtual_bo_clear_mapping','int')])
pre=r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include "drm-uapi/amdgpu_drm.h"
#define FF7_VM_EXPLICIT (1u<<10)
#define RADEON_FLAG_GL2_BYPASS 1u
#define RADEON_FLAG_READ_ONLY 2u
#define RADEON_FLAG_VM_UPDATE_WAIT 4u
#define GFX9 9
#define p_atomic_inc(p) (++*(p))
#define simple_mtx_lock(p) ((void)(p))
#define simple_mtx_unlock(p) ((void)(p))
#define align64(v,a) (((v)+(a)-1)&~((uint64_t)(a)-1))
struct radeon_winsys_bo { bool ff7_api_owned,is_local,is_virtual;uint64_t size,va; };
struct radv_amdgpu_winsys_bo {struct radeon_winsys_bo base;bool emulate_sparse_residency;};
struct radv_amdgpu_winsys {bool ff7_explicit_destroy;uint64_t *ff7_destroy_stats;void *dev;
 int vm_ioctl_lock;uint32_t vm_timeline_syncobj;uint64_t vm_timeline_seq_num;struct {int gfx_level;} info;};
struct ff7_vm_batch {int unused;};
static unsigned calls;static uint64_t flags_seen[2],addr_seen[2],size_seen[2];static int error;
static int ff7_vm_push(struct ff7_vm_batch *b,struct drm_amdgpu_gem_va *v){(void)b;(void)v;assert(0);return 0;}
static int ac_drm_bo_va_op_raw(void *d,uint32_t h,uint64_t o,uint64_t size,uint64_t addr,uint64_t f,uint32_t op){
 (void)d;(void)h;(void)o;assert(op==AMDGPU_VA_OP_UNMAP||op==AMDGPU_VA_OP_CLEAR);assert(calls<2);
 flags_seen[calls]=f;addr_seen[calls]=addr;size_seen[calls]=size;calls++;return error;}
static int ac_drm_bo_va_op_raw2(void *d,uint32_t h,uint64_t o,uint64_t s,uint64_t a,uint64_t f,uint32_t op,uint32_t sy,uint64_t t,int i,int j){
 (void)d;(void)h;(void)o;(void)s;(void)a;(void)f;(void)op;(void)sy;(void)t;(void)i;(void)j;assert(0);return 0;}
static int ac_drm_cs_syncobj_timeline_wait(void *d,uint32_t *s,uint64_t *t,int n,int64_t timeout,int flags,void *p){
 (void)d;(void)s;(void)t;(void)n;(void)timeout;(void)flags;(void)p;assert(0);return 0;}
static uint64_t radv_amdgpu_virtual_bo_get_low_addr(struct radv_amdgpu_winsys *w,struct radv_amdgpu_winsys_bo *b){(void)w;return b->base.va+0x100000;}
'''
post=r'''
int main(void){
 struct radv_amdgpu_winsys ws={.info={.gfx_level=11}};uint64_t counts[12]={0};ws.ff7_destroy_stats=counts;
 struct radv_amdgpu_winsys_bo bo={.base={.size=65537,.va=0x200000}};
 struct {bool api,local,virtual_,eligible;} cases[]={
  {false,true,false,false},{false,false,true,false},{false,false,false,false},
  {true,false,false,false},{true,true,false,true},{true,false,true,true},{true,true,true,true}};
 for(unsigned mode=0;mode<2;mode++)for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  ws.ff7_explicit_destroy=mode;bo.base.ff7_api_owned=cases[i].api;bo.base.is_local=cases[i].local;bo.base.is_virtual=cases[i].virtual_;
  uint64_t f=radv_amdgpu_ff7_destroy_begin(&ws,&bo);assert(!!f==(mode&&cases[i].eligible));
  calls=0;assert(!radv_amdgpu_bo_va_op(&ws,42,0,65537,bo.base.va,0,f,AMDGPU_VA_OP_UNMAP,NULL));
  assert(calls==1 && (flags_seen[0]&FF7_VM_EXPLICIT)==f && size_seen[0]==69632 && addr_seen[0]==bo.base.va);
  assert((flags_seen[0]&(AMDGPU_VM_PAGE_READABLE|AMDGPU_VM_PAGE_WRITEABLE|AMDGPU_VM_PAGE_EXECUTABLE))==
         (AMDGPU_VM_PAGE_READABLE|AMDGPU_VM_PAGE_WRITEABLE|AMDGPU_VM_PAGE_EXECUTABLE));
  calls=0;bo.emulate_sparse_residency=true;
  assert(!radv_amdgpu_virtual_bo_clear_mapping(&ws,&bo,f));
  assert(calls==2 && flags_seen[0]==f && flags_seen[1]==f && addr_seen[1]==bo.base.va+0x100000);
  radv_amdgpu_ff7_destroy_done(&ws,f!=0,bo.base.is_virtual,0);
 }
 assert(counts[4]+counts[5]+counts[6]==14 && counts[7]+counts[8]+counts[9]==14 && counts[10]==0);
 calls=0;error=-5;assert(radv_amdgpu_virtual_bo_clear_mapping(&ws,&bo,FF7_VM_EXPLICIT)==-5 && calls==1);
 radv_amdgpu_ff7_destroy_done(&ws,true,true,error);assert(counts[10]==1);
 puts("PASS: actual RADV lifetime scope excludes internal/nonlocal BOs; real UNMAP retains explicit flags; both virtual VA aliases retain flags; error propagation and counters balance");
}
'''
with tempfile.TemporaryDirectory(prefix='e16-scope-') as td:
    p=Path(td)/'test.c';p.write_text(pre+functions+post);binary=Path(td)/'test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(M/'include'),str(p),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

# Ownership provenance matters as much as the winsys flag. Keep all tags
# confined to actual API resource creation, and explicitly exclude the internal
# diagnostic helper that calls the public memory-allocation entrypoint.
for file,marker in [('radv_buffer.c','buffer->bo->ff7_api_owned = !is_internal;'),
                    ('radv_image.c','image->bindings[0].bo->ff7_api_owned = !is_internal;'),
                    ('radv_device_memory.c','mem->bo->ff7_api_owned = !is_internal && !wsi_info && !import_info && !host_ptr_info &&'),
                    ('tools/radv_debug.c','memory->bo->ff7_api_owned = false;')]:
    assert marker in (M/'src/amd/vulkan'/file).read_text(),file
assert 'ff7_api_owned' not in (M/'src/amd/vulkan/radv_buffer.c').read_text().split('radv_bo_create(')[-1].split('void\nradv_bo_destroy')[0]
print('PASS: application ownership provenance, sparse-image special case, internal debug and imported/display exclusions')
