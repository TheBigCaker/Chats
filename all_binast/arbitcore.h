/* arbitcore.h -- the cosmic-sea allocator, and the thread pool, as pure C.
 *
 * SOURCE OF TRUTH IS THE .ccp, NOT THIS FILE. These are the @c blocks that
 * cygemm/arbit.ccp and cygemm/partitricks.ccp emit; the .ccp originals live on
 * another device, so what is reproduced here is the emitted C, and the comments
 * cite where each part comes from. If you change behaviour, change the .ccp and
 * re-emit -- editing here creates a second source of truth, which is exactly the
 * failure this note exists to prevent.
 *
 * WHERE THIS CAME FROM
 *   allocator   drygon.dry lines 11389-12200 (ArbitSea, ArbitBuffer, _FreeList,
 *               fibonacci buckets, overflow chain, carve chain, fast arena)
 *   struct/API  cygemm/arbit_pxd.dry (ArbitBuffer fields, the public signature list)
 *   pool        cygemm/partitricks.h (ccp_worker_t, clone(2) spawn, futex park)
 *   syscalls    drygon.dry lines 300-430 (raw svc/syscall, mmap 222/215,
 *               write 64, exit 93, futex 98/202)
 *
 * ONE DEVIATION, DELIBERATE AND NAMED: drygon's `sys_mmap(addr, int len, ...)`
 * takes the length as `int`. A 3.66M-word corpus needs 128 MiB contiguous, which
 * does not fit an int in every caller's head, so the length is `long` here. The
 * syscall ABI is unchanged (a register is a register); only the C prototype
 * widened. Everything else is the emitted code.
 *
 * WHAT IS *NOT* HERE: arbit_persist / arbit_load (filesystem), the NEON float
 * kernels, and the TAP breath tick's migration pass. binast does not need them;
 * porting stubs for them would be the "partial implementation" this project
 * forbids, so they are simply absent rather than present-and-wrong.
 */
#ifndef ARBITCORE_H
#define ARBITCORE_H

#include <stddef.h>

/* ============================================================== raw syscalls ==
 * No libc. On aarch64: svc 0 with x8 as the number. Numbers are the stable
 * kernel ABI, quoted from drygon.dry (which uses 222/215 for mmap/munmap --
 * the generic table, not the legacy 192/... one).
 */
typedef char           ac_i8;
typedef short          ac_i16;
typedef int            ac_i32;
typedef long           ac_i64;
typedef unsigned char  ac_u8;
typedef unsigned short ac_u16;
typedef unsigned int   ac_u32;
typedef unsigned long  ac_u64;

#if defined(__aarch64__)
#define AC_SYS_READ    63
#define AC_SYS_WRITE   64
#define AC_SYS_EXIT    93
#define AC_SYS_MMAP   222
#define AC_SYS_MUNMAP 215
#define AC_SYS_FUTEX   98
#define AC_SYS_CLONE  220
#elif defined(__x86_64__)
#define AC_SYS_READ     0
#define AC_SYS_WRITE    1
#define AC_SYS_EXIT    60
#define AC_SYS_MMAP     9
#define AC_SYS_MUNMAP  11
#define AC_SYS_FUTEX  202
#define AC_SYS_CLONE   56
#endif

static inline long ac_syscall6(long n, long a1, long a2, long a3,
                               long a4, long a5, long a6) {
#if defined(__aarch64__)
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    register long x4 __asm__("x4") = a5;
    register long x5 __asm__("x5") = a6;
    __asm__ volatile("svc 0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
#elif defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
#else
    (void)n; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return -1;
#endif
}

/* PROT_READ|PROT_WRITE = 3, MAP_PRIVATE|MAP_ANONYMOUS = 34. */
static inline long ac_mmap(long len) {
    return ac_syscall6(AC_SYS_MMAP, 0, len, 3, 34, -1, 0);
}
static inline int ac_munmap(void *addr, long len) {
    return (int)ac_syscall6(AC_SYS_MUNMAP, (long)addr, len, 0, 0, 0, 0);
}
static inline long ac_write(int fd, const void *buf, long n) {
    return ac_syscall6(AC_SYS_WRITE, fd, (long)buf, n, 0, 0, 0);
}
static inline long ac_read(int fd, void *buf, long n) {
    return ac_syscall6(AC_SYS_READ, fd, (long)buf, n, 0, 0, 0);
}
static inline void ac_exit(int code) {
    ac_syscall6(AC_SYS_EXIT, code, 0, 0, 0, 0, 0);
    for (;;) { }
}
/* FUTEX_WAIT = 0 (block while *addr == expected), FUTEX_WAKE = 1. */
#define AC_FUTEX_WAIT 0
#define AC_FUTEX_WAKE 1
static inline long ac_futex_wait(volatile int *addr, int expected) {
    return ac_syscall6(AC_SYS_FUTEX, (long)addr, AC_FUTEX_WAIT, expected, 0, 0, 0);
}
static inline long ac_futex_wake(volatile int *addr, int n) {
    return ac_syscall6(AC_SYS_FUTEX, (long)addr, AC_FUTEX_WAKE, n, 0, 0, 0);
}

/* cymap_copy / cymap_zero, verbatim from arbit_pxd.dry: the project's total
 * replacement for memcpy/memset. Not named memcpy on purpose -- the point is
 * that no libc memset is reachable from here. */
static inline void cymap_copy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static inline void cymap_zero(void *dst, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0;
}

/* ======================================================== the cosmic sea ====
 * Geometry, quoted from drygon.dry 11344-11363. The Bekenstein ceiling is
 * phi^13 x 4 KiB per region; past it, regions chain. Buckets are
 * (6 layers x 32 zones) carved out of the mapped sea.
 */
#define ARBIT_N_ZONES   32
#define ARBIT_N_LAYERS   6
#define ARBIT_N_BUCKETS (ARBIT_N_LAYERS * ARBIT_N_ZONES)

static const int FIB_SIZES[16] = { 1, 2, 3, 5, 8, 13, 21, 34, 55, 89,
                                   144, 233, 377, 610, 987, 1597 };
#define ARBIT_N_FIB 16

static double _PHI = 1.618033988749895;
#define PHI_13 521.0019190056607
static long _BE_CEILING_BYTES = (long)(PHI_13 * 4096.0);   /* 2,133,696 */
static long _BUCKET_BYTES_MIN = 4096;
static double _ARBIT_SEA_DEFAULT_FRAC = 0.6;

typedef struct ArbitSea {
    unsigned char *base;
    long size_bytes;
    int  initialized;
    long bucket_used[ARBIT_N_BUCKETS];
    long bucket_cap[ARBIT_N_BUCKETS];
    long bucket_next_chunk[ARBIT_N_BUCKETS];
    long total_used;
    long peak_used;
    void *sea_holder;
    long regions[8];
    int  regions_count;
    long region_size;
} ArbitSea;

typedef struct ArbitBuffer {
    int  length;
    long ptr_offset;
    long ptr_addr;
    int  size_bytes;
    int  cascade_l;
    int  biotemplate;
    int  chunk_idx;
    int  breath_n;
    int  core_floats;
    int  iface_floats;
    int  active;
    int  in_overflow;
} ArbitBuffer;

typedef struct _FreeList { int chunks[32]; int count; } _FreeList;

static ArbitSea  _SEA;
static _FreeList _FREELISTS[ARBIT_N_BUCKETS];
static int       _FREELISTS_INITED = 0;

/* carve chain -- raw regions for anything the fibonacci buckets cannot hold */
struct _CarveEntry { void *holder; unsigned char *base; long size; int active; };
static int          _CARVE_LOG_MAX = 4096;
static struct _CarveEntry _CARVE_LOG[4096];
static int          _CARVE_COUNT = 0;

/* overflow chain -- capacity shared across buckets when a routed bucket is full */
struct _OverflowEntry {
    unsigned char *base; long size; int layer; int zone; int chunk_idx; int active;
};
static int _OVERFLOW_LOG_MAX = 4096;
static struct _OverflowEntry _OVERFLOW_LOG[4096];
static int  _OVERFLOW_COUNT = 0;
static long _OVERFLOW_BASE = 0, _OVERFLOW_SLAB_BYTES = 0, _OVERFLOW_END = 0;

static void ArbitSea__cinit(ArbitSea *self, long size_bytes) {
    if (size_bytes <= 0) size_bytes = (long)(_BE_CEILING_BYTES * _ARBIT_SEA_DEFAULT_FRAC);
    self->base = 0;
    self->size_bytes = size_bytes;
    self->initialized = 0;
    self->total_used = 0;
    self->peak_used = 0;
    self->regions_count = 0;
    self->region_size = _BE_CEILING_BYTES;
    for (int i = 0; i < ARBIT_N_BUCKETS; i++) {
        self->bucket_used[i] = 0;
        self->bucket_next_chunk[i] = 0;
        self->bucket_cap[i] = 0;
    }
}

static void ArbitSea_init(ArbitSea *self) {
    if (self->initialized) return;
    long n_buckets = ARBIT_N_BUCKETS;
    long bucket_bytes = self->size_bytes / n_buckets;
    for (int i = 0; i < n_buckets; i++) self->bucket_cap[i] = bucket_bytes;
    long mapped = ac_mmap(self->size_bytes);
    self->base = (unsigned char *)mapped;
    self->sea_holder = (void *)mapped;
    self->regions[0] = mapped;
    self->regions_count = 1;
    self->initialized = 1;
}

static int ArbitSea__bucket_idx(ArbitSea *self, int layer, int zone) {
    (void)self;
    return layer * ARBIT_N_ZONES + zone;
}

/* ---- free lists ---------------------------------------------------------- */
static void _FreeList__cinit(_FreeList *self) {
    for (int i = 0; i < 32; i++) self->chunks[i] = -1;
    self->count = 0;
}
static int _FreeList_push(_FreeList *self, int chunk_idx) {
    if (self->count < 32) { self->chunks[self->count] = chunk_idx; self->count += 1; return 1; }
    return 0;
}
static int _FreeList_pop(_FreeList *self) {
    if (self->count == 0) return -1;
    self->count -= 1;
    return self->chunks[self->count];
}

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif
/* NEON-vectorized byte copy, from the emitted arbit.c block: 64 bytes per
 * iteration (4x16B vectors), scalar fallback otherwise. */
static inline void ac_neon_copy_64(unsigned char *dst, const unsigned char *src, int n64) {
#if defined(__ARM_NEON)
    for (int i = 0; i < n64; i++) {
        uint8x16_t v0 = vld1q_u8(src +  0), v1 = vld1q_u8(src + 16);
        uint8x16_t v2 = vld1q_u8(src + 32), v3 = vld1q_u8(src + 48);
        vst1q_u8(dst +  0, v0); vst1q_u8(dst + 16, v1);
        vst1q_u8(dst + 32, v2); vst1q_u8(dst + 48, v3);
        src += 64; dst += 64;
    }
#else
    for (int i = 0; i < n64; i++) { cymap_copy(dst, src, 64); src += 64; dst += 64; }
#endif
}

/* ---- carve --------------------------------------------------------------- */
static long arbit_carve(long size_bytes) {
    if (size_bytes <= 0) return 0;
    /* DEVIATION 2, NAMED: reuse a released slot before extending. The emitted version
     * returns 0 the moment _CARVE_COUNT hits _CARVE_LOG_MAX, and _CARVE_COUNT only ever
     * grows -- so an allocator that carves and releases in a loop (which the malloc family
     * in arbishim.h does every time a large block is freed) would exhaust 4096 slots and
     * start failing regardless of how much memory is free. Scanning for a dead slot keeps
     * the emitted semantics for a fresh slot and removes the leak of slots. */
    int slot = -1;
    for (int i = 0; i < _CARVE_COUNT; i++) {
        if (!_CARVE_LOG[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        if (_CARVE_COUNT >= _CARVE_LOG_MAX) return 0;
        slot = _CARVE_COUNT++;
    }
    long mapped = ac_mmap(size_bytes);
    if (mapped <= 0 || mapped == -1) return 0;
    struct _CarveEntry *e = &_CARVE_LOG[slot];
    e->holder = (void *)mapped;
    e->base = (unsigned char *)mapped;
    e->size = size_bytes;
    e->active = 1;
    return (long)e->base;
}
static int arbit_release_carve(long base_addr) {
    for (int i = 0; i < _CARVE_COUNT; i++) {
        if (_CARVE_LOG[i].active && (long)_CARVE_LOG[i].base == base_addr) {
            ac_munmap(_CARVE_LOG[i].base, _CARVE_LOG[i].size);
            _CARVE_LOG[i].active = 0;
            _CARVE_LOG[i].base = 0;
            _CARVE_LOG[i].size = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- init / grow --------------------------------------------------------- */
static int arbit_init(long sea_size_bytes) {
    if (_SEA.initialized) return 0;
    /* __cinit__ ALWAYS runs: it is what assigns size_bytes (either the caller's
     * value or the TAP default). Guarding it behind `sea_size_bytes > 0` leaves
     * size_bytes at 0 and the sea maps zero bytes -- which is exactly the bug
     * this line replaced. */
    ArbitSea__cinit(&_SEA, sea_size_bytes);
    ArbitSea_init(&_SEA);
    if (!_FREELISTS_INITED) {
        for (int i = 0; i < ARBIT_N_BUCKETS; i++) _FreeList__cinit(&_FREELISTS[i]);
        _FREELISTS_INITED = 1;
    }
    return 0;
}

static long _arbit_grow_target_bytes(long current_size) {
    long target = (long)(current_size * _PHI);
    if (target < _BUCKET_BYTES_MIN) target = _BUCKET_BYTES_MIN;
    if (target > _BE_CEILING_BYTES) target = _BE_CEILING_BYTES;
    return target;
}

/* Grow the sea. Existing ptr_addr values go STALE -- re-acquire after grow.
 * That is the emitted semantics, not an oversight; keep it and say so. */
/* Kept even though binast does not call it: it is part of the emitted API and
 * a header that silently drops entry points is a header that lies about the
 * allocator it reproduces. Marked unused so -Wall stays meaningful. */
__attribute__((unused)) static long arbit_grow(long target_bytes) {
    if (!_SEA.initialized) arbit_init(0);
    if (target_bytes <= 0) target_bytes = _arbit_grow_target_bytes(_SEA.size_bytes);
    else if (target_bytes > _BE_CEILING_BYTES) target_bytes = _BE_CEILING_BYTES;
    if (target_bytes <= _SEA.size_bytes) return _SEA.size_bytes;
    ArbitSea new_sea;
    ArbitSea__cinit(&new_sea, target_bytes);
    ArbitSea_init(&new_sea);
    if (_SEA.base && _SEA.size_bytes > 0)
        cymap_copy(new_sea.base, _SEA.base, (size_t)_SEA.size_bytes);
    new_sea.total_used = _SEA.total_used;
    new_sea.peak_used = _SEA.peak_used;
    long new_cap = target_bytes / ARBIT_N_BUCKETS;
    for (int k = 0; k < ARBIT_N_BUCKETS; k++) {
        new_sea.bucket_used[k] = _SEA.bucket_used[k];
        new_sea.bucket_next_chunk[k] = _SEA.bucket_next_chunk[k];
        new_sea.bucket_cap[k] = new_cap;
    }
    _SEA = new_sea;
    return _SEA.size_bytes;
}

/* ---- overflow ------------------------------------------------------------ */
static long _overflow_alloc(long size_bytes, int layer, int zone, int chunk_idx) {
    if (_OVERFLOW_SLAB_BYTES == 0) {
        long slab = _BE_CEILING_BYTES;
        _OVERFLOW_BASE = arbit_carve(slab);
        if (_OVERFLOW_BASE == 0) return 0;
        _OVERFLOW_SLAB_BYTES = slab;
        _OVERFLOW_END = 0;
    }
    long off = _OVERFLOW_END;
    if (off + size_bytes > _OVERFLOW_SLAB_BYTES) return 0;
    if (_OVERFLOW_COUNT >= _OVERFLOW_LOG_MAX) return 0;
    _OVERFLOW_END = off + size_bytes;
    struct _OverflowEntry *e = &_OVERFLOW_LOG[_OVERFLOW_COUNT];
    e->base = (unsigned char *)(_OVERFLOW_BASE + off);
    e->size = size_bytes;
    e->layer = layer;
    e->zone = zone;
    e->chunk_idx = chunk_idx;
    e->active = 1;
    _OVERFLOW_COUNT += 1;
    return (long)e->base;
}
static int _overflow_release(long base_addr) {
    for (int i = 0; i < _OVERFLOW_COUNT; i++) {
        if (_OVERFLOW_LOG[i].active && (long)_OVERFLOW_LOG[i].base == base_addr) {
            _OVERFLOW_LOG[i].active = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- the allocator ------------------------------------------------------- */
static int _clamp_layer(int layer) { if (layer < 0 || layer > 5) return 5; return layer; }
static int _clamp_zone(int zone) {
    if (zone < 0) return 0;
    if (zone >= ARBIT_N_ZONES) return ARBIT_N_ZONES - 1;
    return zone;
}
static int _pick_fib(int n) {
    for (int i = 0; i < ARBIT_N_FIB; i++) if (FIB_SIZES[i] >= n) return FIB_SIZES[i];
    return FIB_SIZES[ARBIT_N_FIB - 1];
}

static ArbitBuffer arbit_alloc(int n_floats, int cascade_layer, int biotemplate,
                               int breath_n, int force_lock) {
    /* force_lock exists in the emitted signature; the pure-C path has no lock to
     * take (locking is the caller's job, as partitricks.h documents). */
    (void)force_lock;
    ArbitBuffer buf;
    cymap_zero(&buf, sizeof buf);
    if (n_floats <= 0) return buf;
    if (!_SEA.initialized) arbit_init(0);
    int l = _clamp_layer(cascade_layer);
    int z = _clamp_zone(biotemplate);
    int chunk_size = _pick_fib(n_floats);
    int idx = ArbitSea__bucket_idx(&_SEA, l, z);
    long bucket_bytes = _SEA.bucket_cap[idx];
    long size_bytes = (long)chunk_size * 4;

    if (size_bytes > bucket_bytes) {          /* bigger than any routed bucket */
        long addr = _overflow_alloc(size_bytes, l, z, 0);
        if (addr == 0) return buf;
        buf.length = chunk_size;
        buf.ptr_offset = 0;
        buf.ptr_addr = addr;
        buf.size_bytes = (int)size_bytes;
        buf.cascade_l = l;
        buf.biotemplate = z;
        buf.chunk_idx = 0;
        buf.breath_n = breath_n;
        buf.core_floats = (int)(chunk_size * 0.75);
        buf.iface_floats = chunk_size - buf.core_floats;
        buf.active = 1;
        buf.in_overflow = 1;
        _SEA.total_used += size_bytes;
        if (_SEA.total_used > _SEA.peak_used) _SEA.peak_used = _SEA.total_used;
        return buf;
    }

    int chunk_idx = _FreeList_pop(&_FREELISTS[idx]);
    if (chunk_idx < 0) {
        long next = _SEA.bucket_next_chunk[idx];
        long slots = bucket_bytes / size_bytes;
        if (slots <= 0 || next >= slots) {    /* routed bucket full: spill */
            long addr = _overflow_alloc(size_bytes, l, z, 0);
            if (addr == 0) return buf;
            buf.length = chunk_size;
            buf.ptr_addr = addr;
            buf.size_bytes = (int)size_bytes;
            buf.cascade_l = l;
            buf.biotemplate = z;
            buf.breath_n = breath_n;
            buf.core_floats = (int)(chunk_size * 0.75);
            buf.iface_floats = chunk_size - buf.core_floats;
            buf.active = 1;
            buf.in_overflow = 1;
            _SEA.total_used += size_bytes;
            if (_SEA.total_used > _SEA.peak_used) _SEA.peak_used = _SEA.total_used;
            return buf;
        }
        chunk_idx = (int)next;
        _SEA.bucket_next_chunk[idx] = next + 1;
    }
    long offset = (long)idx * bucket_bytes + (long)chunk_idx * size_bytes;
    buf.length = chunk_size;
    buf.ptr_offset = offset;
    buf.ptr_addr = (long)_SEA.base + offset;
    buf.size_bytes = (int)size_bytes;
    buf.cascade_l = l;
    buf.biotemplate = z;
    buf.chunk_idx = chunk_idx;
    buf.breath_n = breath_n;
    buf.core_floats = (int)(chunk_size * 0.75);
    buf.iface_floats = chunk_size - buf.core_floats;
    buf.active = 1;
    buf.in_overflow = 0;
    _SEA.bucket_used[idx] += 1;
    _SEA.total_used += size_bytes;
    if (_SEA.total_used > _SEA.peak_used) _SEA.peak_used = _SEA.total_used;
    return buf;
}

static int arbit_free(ArbitBuffer buf) {
    if (!buf.active) return 0;
    unsigned char *p = (unsigned char *)buf.ptr_addr;
    cymap_zero(p, (size_t)buf.size_bytes);
    if (buf.in_overflow) {
        _overflow_release(buf.ptr_addr);
        _SEA.total_used -= buf.size_bytes;
        return 1;
    }
    int idx = ArbitSea__bucket_idx(&_SEA, buf.cascade_l, buf.biotemplate);
    _FreeList_push(&_FREELISTS[idx], buf.chunk_idx);
    _SEA.bucket_used[idx] -= 1;
    _SEA.total_used -= buf.size_bytes;
    return 1;
}

__attribute__((unused)) static float *arbit_f32(ArbitBuffer b) { return (float *)b.ptr_addr; }

/* ==================================================== the thread pool ======
 * partitricks, verbatim in structure: raw clone(2) with the worker index passed
 * in the first argument register, per-worker wake/done flags each on their own
 * 64-byte cache line, and futex parking so an idle worker blocks instead of
 * spinning (on Android's CFS a spinning worker gets preempted and cannot see its
 * own wake flag for ~ms). No pthreads anywhere.
 */
typedef struct {
    volatile int wake;      /* 0 idle, 1 run, -1 shutdown */
    volatile int done;
    unsigned char pad0[56];
    void *handle;
    long tid;
    int thread_idx;
    void (*work_func)(void *);
    void *work_arg;
    volatile int exited;
    unsigned char pad1[48];
} ac_worker_t;

static ac_worker_t *ac_pool = NULL;
static int ac_pool_size = 0;
static int ac_active_workers = 0;
static int ac_spin_threshold = 7;

static inline int ac_atomic_load(volatile int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void ac_atomic_store(volatile int *p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void ac_spin_yield(void) {
#if defined(__aarch64__) || defined(__ARM_NEON)
    __asm__ __volatile__("yield" : : : "memory");
#else
    __asm__ __volatile__("pause" : : : "memory");
#endif
}

/* CLONE_* flags, stable kernel ABI values. */
#define AC_CLONE_VM      0x00000100
#define AC_CLONE_FS      0x00000200
#define AC_CLONE_FILES   0x00000400
#define AC_CLONE_SIGHAND 0x00000800
#define AC_CLONE_THREAD  0x00010000
#define AC_CLONE_SYSVSEM 0x00040000
#define AC_CLONE_SETTLS  0x00080000

/* clone(2) that branches straight into the child entry. The child must NOT
 * return through the C stack -- the new stack is empty, so a `ret` would pop
 * garbage. Hence br/jmp with no link, and the worker index in x0/rdi. */
static inline long ac_clone_run(unsigned long flags, void *stack, void *tls,
                                void (*entry)(long), long arg) {
#if defined(__aarch64__)
    register long x8 __asm__("x8") = AC_SYS_CLONE;
    register long x0 __asm__("x0") = (long)flags;
    register long x1 __asm__("x1") = (long)stack;
    register long x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 0;
    register long x4 __asm__("x4") = (long)tls;
    register long x5 __asm__("x5") = 0;
    __asm__ volatile(
        "svc 0;"
        "cbnz x0, 2f;"
        "mov x0, %[arg];"
        "mov x9, %[entry];"
        "br x9;"
        "2:;"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5),
          [entry] "r"((long)entry), [arg] "r"(arg)
        : "memory", "x9");
    return x0;
#else
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = (long)tls;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile(
        "syscall;"
        "test %%rax, %%rax;"
        "jne 2f;"
        "mov %[arg], %%rdi;"
        "mov %[entry], %%r11;"
        "jmp *%%r11;"
        "2:;"
        : "=a"(ret)
        : "a"(AC_SYS_CLONE), "D"(flags), "S"(stack), "d"(0),
          "r"(r10), "r"(r8), "r"(r9), [entry] "r"((long)entry), [arg] "r"(arg)
        : "rcx", "r11", "memory");
    return ret;
#endif
}

static void ac_worker_main(void *arg) {
    ac_worker_t *w = (ac_worker_t *)arg;
    int spin_count = 0;
    for (;;) {
        while (ac_atomic_load(&w->wake) == 0) {
            spin_count++;
            if (spin_count > ac_spin_threshold) {
                ac_spin_yield();
                spin_count = 0;
                ac_futex_wait(&w->wake, 0);
            }
        }
        if (ac_atomic_load(&w->wake) == -1) break;
        if (w->work_func != NULL) w->work_func(w->work_arg);
        ac_atomic_store(&w->wake, 0);
        ac_atomic_store(&w->done, 1);
        ac_futex_wake(&w->done, 1);
    }
    ac_atomic_store(&w->exited, 1);
}

static void ac_linux_entry(long idx) {
    if (idx < 0) idx = 0;
    if (idx >= ac_pool_size) idx = ac_pool_size - 1;
    ac_worker_main(&ac_pool[idx]);
    ac_exit(0);   /* end this thread only -- raw clone, no join */
}

static int ac_pool_create(ac_worker_t *pool, int n, void *stack_region,
                          long stack_size, void *tls_region, long tls_size) {
    ac_pool = pool;
    ac_pool_size = n;
    ac_active_workers = 0;
    for (int i = 0; i < n; i++) {
        pool[i].thread_idx = i;
        pool[i].work_func = NULL;
        pool[i].work_arg = NULL;
        pool[i].wake = 0;
        pool[i].done = 0;
        pool[i].exited = 0;
        pool[i].handle = NULL;
        pool[i].tid = 0;
    }
    for (int i = 0; i < n; i++) {
        void *stack_top = NULL, *tls = NULL;
        if (stack_region != NULL) {
            stack_top = (char *)stack_region + (long)(i + 1) * stack_size;
            stack_top = (void *)(((unsigned long)stack_top) & ~15UL);
        }
        if (tls_region != NULL) tls = (char *)tls_region + (long)i * tls_size;
        long tid = ac_clone_run(AC_CLONE_VM | AC_CLONE_FS | AC_CLONE_FILES |
                                AC_CLONE_SIGHAND | AC_CLONE_THREAD |
                                AC_CLONE_SYSVSEM | AC_CLONE_SETTLS,
                                stack_top, tls, ac_linux_entry, (long)i);
        if (tid < 0) {
            for (int j = 0; j < i; j++) ac_atomic_store(&pool[j].wake, -1);
            for (int j = 0; j < i; j++)
                while (!ac_atomic_load(&pool[j].exited)) ac_spin_yield();
            ac_pool = NULL;
            ac_pool_size = 0;
            return -1;
        }
        pool[i].tid = tid;
    }
    return 0;
}

static void ac_pool_destroy(void) {
    if (ac_pool == NULL) return;
    for (int i = 0; i < ac_pool_size; i++) {
        ac_atomic_store(&ac_pool[i].wake, -1);
        ac_futex_wake(&ac_pool[i].wake, 1);
    }
    for (int i = 0; i < ac_pool_size; i++)
        while (!ac_atomic_load(&ac_pool[i].exited)) ac_spin_yield();
    ac_pool = NULL;
    ac_pool_size = 0;
}

static void ac_dispatch(int idx, void *fn, void *arg) {
    if (ac_pool == NULL || idx < 0 || idx >= ac_pool_size) return;
    ac_pool[idx].work_func = (void (*)(void *))fn;
    ac_pool[idx].work_arg = arg;
    ac_atomic_store(&ac_pool[idx].done, 0);
}
static void ac_start(int n_active) {
    ac_active_workers = n_active;
    for (int i = 0; i < n_active; i++) {
        ac_atomic_store(&ac_pool[i].wake, 1);
        ac_futex_wake(&ac_pool[i].wake, 1);
    }
}
static void ac_wait(void) {
    int n = ac_active_workers, spin_count = 0;
    for (int i = 0; i < n; i++) {
        while (ac_atomic_load(&ac_pool[i].done) == 0) {
            spin_count++;
            if (spin_count > ac_spin_threshold) {
                ac_spin_yield();
                spin_count = 0;
                ac_futex_wait(&ac_pool[i].done, 0);
            }
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* /sys/devices/system/cpu/online, parsed without libc. Returns the core count. */
__attribute__((unused)) static int ac_cpu_count(void) {
    char buf[256];
    long fd = ac_syscall6(56 /*openat*/,
#if defined(__aarch64__)
                          -100, (long)"/sys/devices/system/cpu/online", 0, 0, 0, 0);
#else
                          -100, (long)"/sys/devices/system/cpu/online", 0, 0, 0, 0);
#endif
    if (fd < 0) return 1;
    long n = ac_read((int)fd, buf, sizeof(buf) - 1);
    ac_syscall6(57 /*close*/, fd, 0, 0, 0, 0, 0);
    if (n <= 0) return 1;
    buf[n] = 0;
    int count = 0;
    char *p = buf;
    while (*p) {
        int lo = 0, hi = 0;
        while (*p >= '0' && *p <= '9') { lo = lo * 10 + (*p - '0'); p++; }
        if (*p == '-') { p++; while (*p >= '0' && *p <= '9') { hi = hi * 10 + (*p - '0'); p++; } }
        else hi = lo;
        count += (hi - lo + 1);
        while (*p == ',' || *p == ' ' || *p == 10) p++;
    }
    return count > 0 ? count : 1;
}

#endif /* ARBITCORE_H */
