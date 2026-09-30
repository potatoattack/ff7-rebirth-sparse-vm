#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the actual new opt-in functions against controlled ioctl boundaries."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

if len(sys.argv) != 3:
    raise SystemExit('usage: test_optin.py PATCHED_KERNEL_SOURCE PATCHED_MESA_SOURCE')
kernel, mesa = map(Path, sys.argv[1:])
ku = (kernel / 'include/uapi/drm/amdgpu_drm.h').read_text()
mu = (mesa / 'include/drm-uapi/amdgpu_drm.h').read_text()
names = ('AMDGPU_VM_OP_SPARSE_QUERY', 'AMDGPU_VM_OP_SPARSE_ENABLE', 'AMDGPU_VM_SPARSE_V1_TOKEN')
defines = []
for name in names:
    pattern = rf'^#define {name}\s+(\S+)'
    a, b = re.search(pattern, ku, re.M), re.search(pattern, mu, re.M)
    assert a and b and a[1] == b[1], f'kernel/RADV protocol mismatch: {name}'
    defines.append(f'#define {name} {a[1]}')


def function(text, start):
    a = text.index(start)
    return text[a:text.index('\n}', a) + 2]


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
union drm_amdgpu_vm {
    struct { uint32_t op, flags; } in;
    struct { uint64_t flags; } out;
};
'''+ '\n'.join(defines) + '\n'
ks = (kernel / 'drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c').read_text()
kernel_test = common + r'''
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, y) ((x)=(y))
static int amdgpu_vm_sdma_funcs;
#define AMD_IS_APU 1u
#define CHIP_VEGA10 50u
struct amdgpu_device {
    bool debug_vm, sriov_vf;
    unsigned flags, asic_type;
    struct { unsigned device; } *pdev;
    struct { unsigned block_size, fragment_size; } vm_manager;
};
struct amdgpu_vm {
    struct { void *bo; } root;
    const int *update_funcs;
    bool is_compute_context;
    unsigned sparse_vm_abi;
};
static bool amdgpu_sriov_vf(struct amdgpu_device *a) { return a->sriov_vf; }
static int reserve_result, locks;
static int amdgpu_bo_reserve(void *bo, bool wait) {
    (void)bo; assert(wait);
    if (!reserve_result) ++locks;
    return reserve_result;
}
static void amdgpu_bo_unreserve(void *bo) { (void)bo; assert(locks==1); --locks; }
''' + function(ks, 'static bool amdgpu_vm_sparse_supported(') + '\n' + function(ks, 'static int amdgpu_vm_sparse_ioctl(') + r'''
static int request(struct amdgpu_device *a, struct amdgpu_vm *v, unsigned op, unsigned flags) {
    union drm_amdgpu_vm q={.in={op,flags}};
    int r=amdgpu_vm_sparse_ioctl(a,v,&q);
    assert(!locks);
    if (!r) assert(q.out.flags==AMDGPU_VM_SPARSE_V1_TOKEN);
    return r;
}
int main(void) {
    struct amdgpu_device a={.asic_type=CHIP_VEGA10, .vm_manager={9,9}};
    typeof(*a.pdev) pci={0x744c}; a.pdev=&pci;
    struct amdgpu_vm first={.update_funcs=&amdgpu_vm_sdma_funcs}, other=first;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_QUERY,0)==0);
    assert(!first.sparse_vm_abi);
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_QUERY,1)==-EINVAL);
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,0)==-EINVAL);
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,2)==-EINVAL);
    reserve_result=-EINTR;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EINTR);
    assert(!first.sparse_vm_abi); reserve_result=0;
    /* A different PCI ID is allowed when the actual requirements are met. */
    pci.device=0x73bf;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_QUERY,0)==0);
    assert(!first.sparse_vm_abi);
    a.flags=AMD_IS_APU;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EOPNOTSUPP);
    a.flags=0; a.sriov_vf=true;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EOPNOTSUPP);
    a.sriov_vf=false; a.debug_vm=true;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EOPNOTSUPP);
    a.debug_vm=false; first.is_compute_context=true;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EOPNOTSUPP);
    first.is_compute_context=false; first.update_funcs=NULL;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EOPNOTSUPP);
    first.update_funcs=&amdgpu_vm_sdma_funcs; a.vm_manager.block_size=4;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EOPNOTSUPP);
    a.vm_manager.block_size=9;
    a.asic_type=CHIP_VEGA10-1; a.vm_manager.fragment_size=3;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==-EOPNOTSUPP);
    a.vm_manager.fragment_size=4;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_QUERY,0)==0);
    a.asic_type=CHIP_VEGA10; a.vm_manager.fragment_size=0;
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_QUERY,0)==0);
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==0);
    assert(first.sparse_vm_abi==1 && other.sparse_vm_abi==0);
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_ENABLE,1)==0);
    assert(request(&a,&first,AMDGPU_VM_OP_SPARSE_QUERY,0)==0);
    puts("PASS: query, opt-in, independent VMs, idempotency, PCI-independent eligibility, updater/geometry/version/error rejection");
}
'''

rs = (mesa / 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c').read_text()
radv_test = common + r'''
#define RADV_EXPERIMENTAL_SPARSE_VM (1u << 8)
#define RADV_EXPERIMENTAL_SPARSE (1u << 5)
#define FF7_VM_BATCH_MODE 3
#define DRM_AMDGPU_VM 0x13
struct radv_amdgpu_winsys {
    struct { unsigned pci_id; bool has_dedicated_vram, has_graphics, has_sparse; } info;
    struct { bool ff7_explicit_unmap; } base;
    int fd;
    unsigned ff7_vm_mode, ff7_split_rejected;
    bool ff7_explicit_destroy;
};
static unsigned calls, init_calls;
static int query_error, enable_error;
static bool bad_query_token, bad_enable_token, counter_failure;
static int drm_ioctl_write_read(int fd, unsigned op, void *data, size_t size) {
    (void)fd; assert(op==DRM_AMDGPU_VM && size==sizeof(union drm_amdgpu_vm));
    union drm_amdgpu_vm *q=data; ++calls;
    if (q->in.op==AMDGPU_VM_OP_SPARSE_QUERY) {
        assert(q->in.flags==0);
        q->out.flags=bad_query_token ? 0 : AMDGPU_VM_SPARSE_V1_TOKEN;
        return query_error;
    }
    assert(q->in.op==AMDGPU_VM_OP_SPARSE_ENABLE && q->in.flags==1);
    q->out.flags=bad_enable_token ? 0 : AMDGPU_VM_SPARSE_V1_TOKEN;
    return enable_error;
}
static bool radv_amdgpu_ff7_destroy_init(struct radv_amdgpu_winsys *w) {
    assert(w->ff7_explicit_destroy); ++init_calls; return !counter_failure;
}
''' + function(rs, 'bool\nradv_amdgpu_ff7_init(') + r'''
static struct radv_amdgpu_winsys fresh(void) {
    calls=init_calls=0;
    return (struct radv_amdgpu_winsys){.info={.pci_id=0x73bf, .has_dedicated_vram=true, .has_graphics=true, .has_sparse=true}};
}
int main(void) {
    const uint64_t flag=RADV_EXPERIMENTAL_SPARSE_VM;
    struct radv_amdgpu_winsys w=fresh();
    assert(radv_amdgpu_ff7_init(&w,false,0));
    assert(!calls && !init_calls && !w.ff7_vm_mode && !w.base.ff7_explicit_unmap && !w.ff7_explicit_destroy);
    assert(radv_amdgpu_ff7_init(&w,false,1)); /* unrelated experimental flag */
    assert(!calls && !init_calls);
    assert(!radv_amdgpu_ff7_init(&w,true,flag) && !calls);
    w.info.has_sparse=false;
    assert(!radv_amdgpu_ff7_init(&w,false,flag) && !calls);
    /* The feature must not force normally disabled sparse support. */
    assert(!radv_amdgpu_ff7_init(&w,false,flag | RADV_EXPERIMENTAL_SPARSE) && !calls);
    assert(radv_amdgpu_ff7_init(&w,false,0) && !calls);
    w=fresh(); w.info.has_dedicated_vram=false;
    assert(!radv_amdgpu_ff7_init(&w,false,flag) && !calls);
    w=fresh(); w.info.has_graphics=false;
    assert(!radv_amdgpu_ff7_init(&w,false,flag) && !calls);
    w=fresh(); query_error=-EINVAL;
    assert(!radv_amdgpu_ff7_init(&w,false,flag) && calls==1 && !init_calls && !w.ff7_vm_mode);
    w=fresh(); query_error=0; bad_query_token=true;
    assert(!radv_amdgpu_ff7_init(&w,false,flag) && calls==1 && !w.ff7_vm_mode);
    w=fresh(); bad_query_token=false; enable_error=-EIO;
    assert(!radv_amdgpu_ff7_init(&w,false,flag) && calls==2 && !w.ff7_vm_mode);
    w=fresh(); enable_error=0; bad_enable_token=true;
    assert(!radv_amdgpu_ff7_init(&w,false,flag) && calls==2 && !w.ff7_vm_mode);
    w=fresh(); bad_enable_token=false;
    assert(radv_amdgpu_ff7_init(&w,false,flag));
    assert(calls==2 && init_calls==1 && w.ff7_vm_mode==FF7_VM_BATCH_MODE);
    assert(!w.ff7_split_rejected && w.base.ff7_explicit_unmap && w.ff7_explicit_destroy);
    w=fresh(); counter_failure=true;
    assert(!radv_amdgpu_ff7_init(&w,false,flag));
    puts("PASS: default off, unrelated flags, sparse/VRAM/graphics requirements, old/mismatched kernel, complete profile and failure paths");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    for name, source in (('kernel-optin', kernel_test), ('radv-optin', radv_test)):
        path = Path(tmp) / name
        path.with_suffix('.c').write_text(source)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(path.with_suffix('.c')), '-o', str(path)], check=True)
        subprocess.run([str(path)], check=True)
