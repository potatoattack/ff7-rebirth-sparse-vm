/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ff7_vm_batch.h>
struct mock {unsigned n, fail_at; int failure; bool expect_explicit; struct drm_amdgpu_gem_va headers[96], entries[96][16];};
static int send_mock(void *u, struct drm_amdgpu_gem_va *v) {
    struct mock *m=u; assert(m->n<96); unsigned i=m->n++;
    m->headers[i]=*v;
    if(v->operation==FF7_VM_BATCH_OP) {
        assert(v->offset_in_bo>=1 && v->offset_in_bo<=16);
        assert(v->flags==(m->expect_explicit?FF7_VM_EXPLICIT:0) && !v->map_size && !v->_pad && !v->vm_timeline_point &&
               !v->vm_timeline_syncobj_out && !v->num_syncobj_handles && !v->input_fence_syncobj_handles);
        memcpy(m->entries[i],(void *)(uintptr_t)v->va_address,v->offset_in_bo*sizeof(*v));
    }
    return m->fail_at==i+1?m->failure:0;
}
static struct ff7_vm_batch init(struct mock *m, enum ff7_vm_mode mode) {
    memset(m,0,sizeof(*m)); return (struct ff7_vm_batch){.mode=mode,.send=send_mock,.user=m};
}
static struct drm_amdgpu_gem_va entry(unsigned n) {
    return (struct drm_amdgpu_gem_va){.handle=42,.operation=AMDGPU_VA_OP_REPLACE,
        .flags=AMDGPU_VM_PAGE_READABLE|AMDGPU_VM_PAGE_WRITEABLE|AMDGPU_VM_PAGE_EXECUTABLE,
        .va_address=0x100000000ull+n*65536ull,.offset_in_bo=(31u-n)*65536ull,.map_size=65536};
}
static void original_tests(void) {
    struct mock m,q; struct ff7_vm_batch b=init(&m,FF7_VM_BATCH_MODE);
    for(unsigned i=0;i<17;i++){struct drm_amdgpu_gem_va v=entry(i);assert(!ff7_vm_push(&b,&v));}
    assert(m.n==1 && b.count==1 && b.stats.entries==16);
    for(unsigned i=0;i<16;i++){struct drm_amdgpu_gem_va v=entry(i);assert(!memcmp(&v,&m.entries[0][i],sizeof v));}
    assert(!ff7_vm_flush(&b));assert(m.n==2 && m.headers[1].offset_in_bo==1 && !b.count);
    for(unsigned mode=FF7_VM_BASELINE;mode<=FF7_VM_DIRECT_MODE;mode++) {
        b=init(&m,mode);struct drm_amdgpu_gem_va v=entry(0),expected=v;
        expected.flags|=FF7_VM_ACCOUNT|(mode==FF7_VM_DIRECT_MODE?FF7_VM_DIRECT:0);
        assert(!ff7_vm_push(&b,&v));assert(m.n==1 && !memcmp(&expected,&m.headers[0],sizeof expected));
    }
    /* Different BO, flags, duplicate VA and large/PRT mappings flush in order. */
    b=init(&m,FF7_VM_BATCH_MODE);struct drm_amdgpu_gem_va v=entry(0);assert(!ff7_vm_push(&b,&v));
    assert(!ff7_vm_push(&b,&v));assert(m.n==1);
    v=entry(1);v.handle++;assert(!ff7_vm_push(&b,&v));assert(m.n==2);
    v=entry(2);v.handle++;v.flags&=~AMDGPU_VM_PAGE_WRITEABLE;assert(!ff7_vm_push(&b,&v));assert(m.n==3);
    v=entry(3);v.map_size*=2;assert(!ff7_vm_push(&b,&v));assert(m.n==5 && !b.count);
    v=entry(4);v.handle=0;v.flags=AMDGPU_VM_PAGE_PRT;assert(!ff7_vm_push(&b,&v));assert(m.n==6 && b.stats.fallback==2);
    /* Unsupported is the sole replayable kernel error. Preserve every field. */
    b=init(&m,FF7_VM_BATCH_MODE);m.fail_at=1;m.failure=-EOPNOTSUPP;
    for(unsigned i=0;i<3;i++){v=entry(i);assert(!ff7_vm_push(&b,&v));}
    assert(!ff7_vm_flush(&b));assert(m.n==4 && b.stats.fallback==3 && !b.count);
    for(unsigned i=0;i<3;i++){v=entry(i);v.flags|=FF7_VM_ACCOUNT|FF7_VM_DIRECT;assert(!memcmp(&v,&m.headers[i+1],sizeof v));}
    for(unsigned mode=FF7_VM_BASELINE;mode<=FF7_VM_BATCH_MODE;mode++) {
        b=init(&m,mode);m.fail_at=1;m.failure=-EIO;v=entry(0);
        int r=ff7_vm_push(&b,&v);if(!r)r=ff7_vm_flush(&b);assert(r==-EIO && m.n==1);
        assert(ff7_vm_push(&b,&v)==-EIO && ff7_vm_flush(&b)==-EIO && m.n==1 && b.stats.errors==1);
    }
    /* Independent/interleaved submissions neither drain nor share each other. */
    b=init(&m,FF7_VM_BATCH_MODE);struct ff7_vm_batch c=init(&q,FF7_VM_BATCH_MODE);
    v=entry(0);assert(!ff7_vm_push(&b,&v));v=entry(1);assert(!ff7_vm_push(&c,&v));
    assert(!ff7_vm_flush(&c) && q.n==1 && m.n==0 && b.count==1);assert(!ff7_vm_flush(&b));
    uint32_t badflags[]={FF7_VM_EXPLICIT,FF7_VM_ACCOUNT,FF7_VM_DIRECT,AMDGPU_VM_DELAY_UPDATE};
    for(unsigned i=0;i<4;i++){b=init(&m,FF7_VM_BATCH_MODE);v=entry(0);v.flags|=badflags[i];assert(ff7_vm_push(&b,&v)==-EINVAL && m.n==0);}
    b=init(&m,FF7_VM_BATCH_MODE);v=entry(0);v.vm_timeline_point=1;assert(ff7_vm_push(&b,&v)==-EINVAL && !m.n);
    b=init(&m,FF7_VM_OFF);v=entry(0);assert(ff7_vm_push(&b,&v)==-EINVAL && !m.n);
    puts("PASS: bounded vectors, fields, boundaries, fallback, no replay, independent contexts, private ABI guards");
}

/* A mutation-aware oracle: every accepted mapping must occur exactly once,
 * in input order, with all fields intact. Rejected vectors change nothing. */
struct oracle { unsigned mask, calls, attempts, applied, error_at; };
static unsigned index_of(const struct drm_amdgpu_gem_va *v) {
    assert(v->va_address>=0x100000000ull);
    unsigned n=(unsigned)((v->va_address-0x100000000ull)/65536ull);
    assert(n<16);return n;
}
static void apply_one(struct oracle *o,const struct drm_amdgpu_gem_va *v,bool scalar) {
    unsigned i=index_of(v);assert(i==o->applied++);
    struct drm_amdgpu_gem_va want=entry(i);
    if(scalar)want.flags|=FF7_VM_ACCOUNT|FF7_VM_DIRECT;
    assert(!memcmp(&want,v,sizeof want));
}
static int oracle_send(void *user,struct drm_amdgpu_gem_va *v) {
    struct oracle *o=user;++o->calls;
    if(o->error_at==o->calls)return -EIO;
    if(v->operation!=FF7_VM_BATCH_OP){apply_one(o,v,true);return 0;}
    ++o->attempts;
    const struct drm_amdgpu_gem_va *e=(void *)(uintptr_t)v->va_address;
    assert(v->offset_in_bo>=1 && v->offset_in_bo<=16);
    for(unsigned j=0;j<v->offset_in_bo;++j)
        if(o->mask&(1u<<index_of(&e[j])))return -EOPNOTSUPP;
    for(unsigned j=0;j<v->offset_in_bo;++j)apply_one(o,&e[j],false);
    return 0;
}
static void explicit_tests(void) {
    for(unsigned fallback=0;fallback<2;++fallback) {
        struct mock m;struct ff7_vm_batch b=init(&m,FF7_VM_BATCH_MODE);
        b.explicit_unmap=true;m.expect_explicit=true;
        if(fallback){m.fail_at=1;m.failure=-EOPNOTSUPP;}
        for(unsigned i=0;i<3;++i){struct drm_amdgpu_gem_va v=entry(i);assert(!ff7_vm_push(&b,&v));}
        assert(!ff7_vm_flush(&b));assert(m.headers[0].flags==FF7_VM_EXPLICIT);
        for(unsigned i=0;i<3;++i) {
            struct drm_amdgpu_gem_va want=entry(i);
            assert(!memcmp(&want,&m.entries[0][i],sizeof want));
            if(fallback){want.flags|=FF7_VM_EXPLICIT|FF7_VM_ACCOUNT|FF7_VM_DIRECT;
                assert(!memcmp(&want,&m.headers[i+1],sizeof want));}
        }
        struct drm_amdgpu_gem_va prt=entry(4);prt.handle=0;prt.flags=AMDGPU_VM_PAGE_PRT;
        assert(!ff7_vm_push(&b,&prt));
        assert(m.headers[m.n-1].flags==(AMDGPU_VM_PAGE_PRT|FF7_VM_EXPLICIT|FF7_VM_ACCOUNT|FF7_VM_DIRECT));
    }
    puts("PASS: explicit mode on vector headers and every scalar fallback, unchanged PTE records and PRT handling");
}
int main(void) {
    explicit_tests();
    original_tests();
    for(unsigned count=1;count<=16;++count)for(unsigned mask=0;mask<(1u<<count);++mask) {
        struct oracle o={.mask=mask};
        struct ff7_vm_batch b={.mode=FF7_VM_BATCH_MODE,.split_rejected=true,.send=oracle_send,.user=&o};
        for(unsigned i=0;i<count;++i){struct drm_amdgpu_gem_va v=entry(i);assert(!ff7_vm_push(&b,&v));}
        assert(!ff7_vm_flush(&b));
        assert(o.applied==count && b.stats.entries+b.stats.scalar==count && !b.count);
        assert(o.attempts<=15 && o.calls<=31 && !b.error && !b.stats.errors);
        if(!mask)assert(o.calls==1 && b.stats.entries==count);
    }
    /* Prefix may have committed before a later real error: never replay it. */
    for(unsigned fail=1;fail<=15;++fail) {
        struct oracle o={.mask=0x0101,.error_at=fail};
        struct ff7_vm_batch b={.mode=FF7_VM_BATCH_MODE,.split_rejected=true,.send=oracle_send,.user=&o};
        int r=0;
        for(unsigned i=0;i<16 && !r;++i){struct drm_amdgpu_gem_va v=entry(i);r=ff7_vm_push(&b,&v);}
        if(!r)r=ff7_vm_flush(&b);
        if(r){unsigned calls=o.calls;struct drm_amdgpu_gem_va v=entry(0);
            assert(r==-EIO && b.stats.errors==1 && ff7_vm_flush(&b)==r && ff7_vm_push(&b,&v)==r && o.calls==calls);}
        else assert(o.applied==16 && o.calls<fail);
    }
    puts("PASS: exhaustive 1..16-entry eligibility masks, ordered exactly-once writes, bounded retries and terminal errors");
    return 0;
}
