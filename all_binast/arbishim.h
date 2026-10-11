/* arbishim.h -- which arbitr functions the tools use, and nothing else.
 *
 * Every allocation in this tree is arbit's own:
 *
 *     arbit_carve(n)            a region of n bytes. Returns a raw ADDRESS, so it works
 *                               with pointers directly. This is the one used for buffers.
 *     arbit_release_carve(addr) give it back.
 *     cymap_zero(p, n)          zero it (his memset replacement).
 *     cymap_copy(d, s, n)       copy (his memcpy replacement).
 *     arbit_alloc(n_floats, layer, zone, breath, lock)
 *                               a routed bucket. Returns an ArbitBuffer HANDLE, for
 *                               bucket-shaped things that want the free list. Small,
 *                               fixed-size, long-lived state only.
 *     arbit_free(handle)        release that handle.
 *
 * There is no shim here and no alloc-family name anywhere in the tree: no malloc, no
 * calloc, no realloc, no free, not as a call and not as a #define. A wrapper layer was
 * tried and removed -- arbit already has the functions needed, and a second allocator on
 * top of the first is exactly the kind of thing that hides which allocator is in use.
 *
 * SIZING NOTE for arbit_carve: the emitted arbit_carve refuses at _CARVE_LOG_MAX, so
 * carve is for a bounded number of large, long-lived regions (a file image, a corpus
 * buffer, the pool's stacks). The routed buckets are the path for many small pieces.
 */
#ifndef ARBISHIM_H
#define ARBISHIM_H
#include "arbitcore.h"

/* his carve, zeroed -- arbit_carve returns the address, cymap_zero clears it. */
__attribute__((unused)) static void *arb_carve_zeroed(long nbytes) {
    if (nbytes <= 0) nbytes = 1;
    void *p = (void *)arbit_carve(nbytes);
    if (p) cymap_zero(p, (size_t)nbytes);
    return p;
}

/* typed allocation over his carve, so a call site reads as what it allocates. Two lines,
 * not a family -- and if either disappears the build fails rather than silently using
 * something else. */
__attribute__((unused)) static void *arb_bytes(long n) {
    if (n <= 0) n = 1;
    void *p = (void *)arbit_carve(n);
    if (p) cymap_zero(p, (size_t)n);
    return p;
}

/* The bucket handle form: for small fixed-size long-lived state that wants arbit's free
 * list rather than a carve. Returns the handle so arbit_free gets the SAME block
 * arbit_alloc produced. */
__attribute__((unused)) static ArbitBuffer ac_alloc_bytes(long nbytes) {
    int n_floats = (int)((nbytes + 3) / 4);
    if (n_floats <= 0) n_floats = 1;
    ArbitBuffer b = arbit_alloc(n_floats, 0, 0, -1, 0);
    if (b.active && b.ptr_addr) cymap_zero((void *)b.ptr_addr, (size_t)b.size_bytes);
    return b;
}

/* ============================================================ worker pool ==
 * Pool backing from the same allocator, mirroring preprocessed.dry's partitricks_init:
 * pool struct from arbit_alloc, stacks and TLS from two carves.
 */
#define ARB_STACK_SIZE (1L << 20)   /* 1 MiB per worker stack (drygon.dry:14843) */
#define ARB_TLS_SIZE   256L

typedef struct {
    ac_worker_t *workers;
    ArbitBuffer  pool_buf;
    long         stack_base;
    long         tls_base;
    int          n;
    int          up;
} ArbPool;

__attribute__((unused)) static int arb_pool_up(ArbPool *p, int n_threads) {
    if (p->up) return 0;
    if (n_threads < 1) n_threads = 1;
    long pool_bytes = (long)n_threads * (long)sizeof(ac_worker_t);
    p->pool_buf = ac_alloc_bytes(pool_bytes);
    if (!p->pool_buf.active || p->pool_buf.ptr_addr == 0) return -1;
    void *pool_ptr = (void *)p->pool_buf.ptr_addr;
    p->stack_base = arbit_carve((long)n_threads * ARB_STACK_SIZE);
    p->tls_base   = arbit_carve((long)n_threads * ARB_TLS_SIZE);
    if (p->stack_base == 0 || p->tls_base == 0) return -1;
    if (ac_pool_create((ac_worker_t *)pool_ptr, n_threads,
                       (void *)p->stack_base, ARB_STACK_SIZE,
                       (void *)p->tls_base, ARB_TLS_SIZE) != 0) return -1;
    p->workers = (ac_worker_t *)pool_ptr;
    p->n = n_threads;
    p->up = 1;
    return 0;
}

__attribute__((unused)) static void arb_pool_down(ArbPool *p) {
    if (!p->up) return;
    ac_pool_destroy();
    if (p->pool_buf.active) arbit_free(p->pool_buf);
    if (p->stack_base) arbit_release_carve(p->stack_base);
    if (p->tls_base)   arbit_release_carve(p->tls_base);
    p->up = 0;
}

/* Run fn(arg) on `count` workers at once and wait. The payload is plain C: a worker never
 * touches a lock or any higher-level object. */
__attribute__((unused)) static void arb_pool_run(ArbPool *p, int count, void (*fn)(void *), void **args) {
    if (count > p->n) count = p->n;
    for (int i = 0; i < count; i++) ac_dispatch(i, (void *)fn, args[i]);
    ac_start(count);
    ac_wait();
}

#endif /* ARBISHIM_H */
