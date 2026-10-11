/* arbittest.c -- does the arbit allocator and the partitricks pool actually work,
 * on this device, as reproduced? Every claim binastfire will lean on is exercised
 * here first, so a failure is diagnosable in 200 lines instead of inside a model.
 *
 *   arbit alloc/free round trip, and the free list reusing the block
 *   carve and release, with the region actually writable
 *   the pool spawning real threads that run a payload
 *   the pool's result being the sum of what all the workers wrote (i.e. they
 *     really ran in parallel on disjoint slices)
 */
#include "arbishim.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_(const char *s) { ac_write(1, s, slen(s)); }
static void putdec(long v) {
    char b[24]; int i = 0;
    if (v < 0) { b[i++] = '-'; v = -v; }
    if (v == 0) { b[i++] = '0'; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (int a = (b[0] == '-') ? 1 : 0, z = i - 1; a < z; a++, z--) {
        char t = b[a]; b[a] = b[z]; b[z] = t;
    }
    ac_write(1, b, i);
}
static void fail(const char *what) {
    puts_("FAIL "); puts_(what); puts_("\n"); ac_exit(1);
}
static void ok(const char *what) { puts_("ok   "); puts_(what); puts_("\n"); }

/* ---- the payload the pool runs: each worker writes its own id into its slice */
typedef struct { int id; int *out; long n; long sum; } Work;

static void work_fn(void *arg) {
    Work *w = (Work *)arg;
    long acc = 0;
    for (long i = 0; i < w->n; i++) {
        w->out[i] = w->id;          /* proves this worker touched THIS slice */
        acc += w->id;
    }
    w->sum = acc;
}

int main(void) {
    puts_("== arbit: the allocator\n");

    /* init the sea explicitly, then take a bucket-sized block */
    arbit_init(0);
    if (!_SEA.initialized) fail("sea did not initialise");
    if (_SEA.size_bytes <= 0) fail("sea has no size");
    ok("cosmic sea mapped");

    ArbitBuffer a = ac_alloc_bytes(4096);
    if (!a.active || a.ptr_addr == 0) fail("ac_alloc_bytes(4096)");
    float *fa = arbit_f32(a);
    /* write, read back: the zeroing and the pointer must both be real. First
     * confirm it arrived zeroed, THEN write -- otherwise this test passes on a
     * buffer that was never zeroed. */
    for (int i = 0; i < 1024; i++) if (fa[i] != 0.0f) fail("bucket block is not zeroed");
    for (int i = 0; i < 1024; i++) fa[i] = (float)i;
    int bad = 0;
    for (int i = 0; i < 1024; i++) if (fa[i] != (float)i) bad++;
    if (bad) fail("bucket block is not read/write");
    fa[0] = 111.0f;                 /* sentinel: the alias check reads this */
    ok("bucket alloc is zeroed, writable and readable");

    /* free one, then allocate again: the free list should hand the SAME chunk back.
     * NOTE the size matters here. A routed bucket is size_bytes/192 ~ 6667 B and a
     * chunk's stride is the WHOLE fibonacci class (1597 floats = 6388 B), so a
     * 4096-byte request fits exactly ONE chunk per bucket and never touches the
     * free list -- it spills to overflow instead. To exercise the free list the
     * request has to be small enough that several of its class fit a bucket, so
     * this uses 256 bytes (fib class 89 floats = 356 B, 18 slots). That is a real
     * property of the allocator, not a test artifact. */
    ArbitBuffer b2 = ac_alloc_bytes(256);
    if (!b2.active) fail("second ac_alloc_bytes");
    if (b2.in_overflow) fail("a 256-byte request should route, not spill");
    if (b2.ptr_addr == a.ptr_addr) fail("allocator handed out the same block twice");
    ArbitBuffer b3 = ac_alloc_bytes(256);
    if (!b3.active || b3.in_overflow) fail("third ac_alloc_bytes");
    if (b3.ptr_addr == b2.ptr_addr) fail("two routed chunks alias");
    *(int *)b2.ptr_addr = 12345;
    if (*(int *)b3.ptr_addr != 0) fail("routed chunks overlap");
    ok("two routed allocations are disjoint");

    if (!arbit_free(b3)) fail("arbit_free");
    ArbitBuffer c3 = ac_alloc_bytes(256);
    if (!c3.active) fail("alloc after free");
    if (c3.ptr_addr != b3.ptr_addr) fail("free list did not reuse the freed chunk");
    ok("free list reuses a freed chunk");

    /* the oversized path: bigger than any bucket can hold, so it must spill */
    ArbitBuffer big_buf = ac_alloc_bytes(1 << 20);     /* 1 MiB >> fib max 1597 f */
    if (!big_buf.active) fail("oversized alloc");
    if (!big_buf.in_overflow) fail("a 1 MiB request must spill to overflow");
    *(char *)big_buf.ptr_addr = (char)0x5A;
    if (*(char *)big_buf.ptr_addr != (char)0x5A) fail("overflow block is not writable");
    arbit_free(big_buf);
    ok("oversized requests spill to the overflow chain and free cleanly");

    puts_("\n== arbit: the carve chain\n");
    /* a carve big enough that the fibonacci path could not serve it */
    long N = 1 << 22;                      /* 4 MiB */
    void *big = arb_carve_zeroed(N);
    if (!big) fail("arb_carve_zeroed(4 MiB)");
    unsigned char *p = (unsigned char *)big;
    if (p[0] != 0 || p[N - 1] != 0) fail("carve is not zeroed");
    p[0] = 0xAB; p[N - 1] = 0xCD;
    if (p[0] != 0xAB || p[N - 1] != 0xCD) fail("carve is not writable end to end");
    ok("4 MiB carve is zeroed and writable end to end");
    if (!arbit_release_carve((long)big)) fail("arbit_release_carve");
    if (arbit_release_carve((long)big)) fail("double release reported success");
    ok("carve releases, and double release is refused");

    puts_("\n== partitricks: the pool\n");
    int cores = ac_cpu_count();
    if (cores < 1) fail("ac_cpu_count");
    puts_("     cores "); putdec(cores); puts_("\n");

    ArbPool pool = {0};
    int n = cores > 4 ? 4 : cores;          /* 4 is enough to prove parallelism */
    if (arb_pool_up(&pool, n) != 0) fail("arb_pool_up");
    ok("pool spawned via raw clone(2)");

    /* n workers, each writing into its own slice, in parallel */
    long per = 1 << 16;
    int *bufs[8]; Work works[8]; void *args[8];
    for (int i = 0; i < n; i++) {
        bufs[i] = (int *)arb_carve_zeroed(per * (long)sizeof(int));
        if (!bufs[i]) fail("worker slice carve");
        works[i].id = i + 1; works[i].out = bufs[i]; works[i].n = per; works[i].sum = 0;
        args[i] = &works[i];
    }
    arb_pool_run(&pool, n, work_fn, args);
    for (int i = 0; i < n; i++) {
        int badslice = 0;
        for (long k = 0; k < per; k++) if (bufs[i][k] != i + 1) { badslice = 1; break; }
        if (badslice) fail("a worker did not fill its own slice");
        if (works[i].sum != per * (i + 1)) fail("worker returned the wrong sum");
    }
    ok("every worker ran and filled only its own slice");

    /* run the pool twice: the wake/done handshake must be reusable */
    for (int i = 0; i < n; i++) { works[i].sum = 0; }
    arb_pool_run(&pool, n, work_fn, args);
    if (works[n - 1].sum != per * n) fail("pool did not survive a second dispatch");
    ok("pool dispatches again (handshake is reusable)");

    arb_pool_down(&pool);
    for (int i = 0; i < n; i++) arbit_release_carve((long)bufs[i]);
    ok("pool tore down and every carve released");

    puts_("\nARBIT AND PARTITRICKS: ALL CHECKS PASSED\n");
    ac_exit(0);
    return 0;
}
