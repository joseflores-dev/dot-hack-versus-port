/* spurs_job.c — stage and enter a SPURS jobchain job the way Sony's jm2 does.
 *
 * A job binary is not an SPU ELF and not a policy module: it is a raw image
 * built by the SDK's job_elf-to-bin, linked against Sony's job_start_w_crt.o.
 * That CRT is what every job entry actually runs first — it saves r3/r4, runs
 * the ctors, and calls
 *
 *     void cellSpursJobMain2(CellSpursJobContext2 *jobContext,   // r3
 *                            CellSpursJob256      *job);         // r4
 *
 * (target/spu/include/cell/spurs/job_chain.h). Both are LOCAL STORE pointers,
 * so the manager must have laid the whole working set out in LS before entry.
 * Entering a job without that context is what left LBP's job parked with zero
 * DMA traffic: it reads its ioBuffer pointer out of a zeroed context and stalls.
 *
 * The layout is pinned by _cellSpursCheckJob() in
 * target/common/include/cell/spurs/job_descriptor.h, which sums the exact LS a
 * job consumes. For a jobchain it runs with align = 1024, isScratchStackUnified
 * = 1, maxUsableMemorySize = CELL_SPURS_MAX_SIZE_JOB_MEMORY (234 KB):
 *
 *     align1024(sizeBinary<<4)                                   binary + bss
 *   + align1024(sizeInOrInOut)                                   ioBuffer
 *   + align1024(sizeOut)                                         oBuffer
 *   + align1024((sizeScratch<<4) + (sizeStack ? sizeStack<<4 : 8192))
 *                                                                scratch + stack
 *   + per cache entry: align1024(size)                           cacheBuffer[0..3]
 *
 * We reproduce that arithmetic rather than invent a layout, so a job that
 * range-checks its own buffers sees what it would on hardware.
 *
 * Load address: the job binary is position-independent (job_start_w_crt derives
 * its load delta with a brsl-to-next / subtract-link-time-address idiom), so jm2
 * may place it anywhere. We place it at LS 0 — the lifted C is keyed to
 * link-time addresses, which makes the delta zero and the lifted branch targets
 * exact.
 */

#include "spu_coherency.h"
#include "spu_workload.h"
#include "spu_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>   /* offsetof */
#include <pthread.h>
#include "ps3emu/log_first.h"

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

/* ---- guest reads (big-endian) ------------------------------------------ */
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t g16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
static uint64_t g64(uint32_t ea)
{
    return ((uint64_t)g32(ea) << 32) | (uint64_t)g32(ea + 4);
}

/* ---- local-store writes (big-endian) ----------------------------------- */
static void ls32(uint8_t* ls, uint32_t o, uint32_t v)
{
    ls[o] = (uint8_t)(v >> 24); ls[o+1] = (uint8_t)(v >> 16);
    ls[o+2] = (uint8_t)(v >> 8); ls[o+3] = (uint8_t)v;
}
static void ls16(uint8_t* ls, uint32_t o, uint16_t v)
{
    ls[o] = (uint8_t)(v >> 8); ls[o+1] = (uint8_t)v;
}

/* CellSpursJobHeader, 48 B — job_descriptor.h */
enum {
    JH_EA_BINARY           = 0x00,   /* be u64; low bit = CACHE_INHIBIT flag */
    JH_SIZE_BINARY         = 0x08,   /* be u16; bytes >> 4                   */
    JH_SIZE_DMA_LIST       = 0x0A,   /* be u16; input list size in BYTES     */
    JH_EA_HIGH_INPUT       = 0x0C,
    JH_USE_INOUT_BUFFER    = 0x10,
    JH_SIZE_IN_OR_INOUT    = 0x14,
    JH_SIZE_OUT            = 0x18,
    JH_SIZE_STACK          = 0x1C,   /* be u16; quadwords */
    JH_SIZE_SCRATCH        = 0x1E,   /* be u16; quadwords */
    JH_EA_HIGH_CACHE       = 0x20,
    JH_SIZE_CACHE_DMA_LIST = 0x24,   /* be u32; bytes */
    JH_JOB_TYPE            = 0x2C,
    JH_SIZE                = 0x30,
};

/* CellSpursJobContext2, 48 B — job_context_types.h. SPU pointers are 32-bit LS
 * offsets and the struct lives in local store, so every field is big-endian. */
enum {
    JC_IO_BUFFER          = 0x00,
    JC_CACHE_BUFFER       = 0x04,   /* void *cacheBuffer[4] */
    JC_SIZE_JOB_DESC      = 0x14,   /* sizeJobDescriptor:4 + pad:28 */
    JC_NUM_IO_BUFFER      = 0x18,
    JC_NUM_CACHE_BUFFER   = 0x1A,
    JC_O_BUFFER           = 0x1C,
    JC_S_BUFFER           = 0x20,
    JC_DMA_TAG            = 0x24,
    JC_EA_JOB_DESCRIPTOR  = 0x28,   /* be u64 */
    JC_SIZE               = 0x30,
};

#define ALIGN1024(x)  (((x) + 1023u) & ~1023u)
#define JOB_DMA_TAG   20u          /* "one of: {20,21}" — job_context_types.h */
#define JOB_STACK_DEFAULT 8192u    /* _cellSpursCheckJob's sizeStack==0 default */

/* Local store of the most recent job, kept alive past the run.
 *
 * A title can poll the SPU that runs a job chain with sys_spu_thread_read_LS,
 * identifying it by the CHAIN HANDLE rather than an lv2 thread id -- Tokyo
 * Jungle reads its bus counts that way, 45k polls of offset 0 in a 45 s run.
 * The job context is a stack local, so by the time the PPU looks there is
 * nothing to read and every poll fails. Retain the store and let the syscall
 * resolve a chain handle to it. */
static uint8_t  s_job_ls[SPU_LS_SIZE];
static int      s_job_ls_valid;
uint32_t        g_spurs_job_ls_handle;   /* set by the chain walker */

const uint8_t* spurs_job_ls_for_handle(uint32_t handle)
{
    if (!s_job_ls_valid || !handle || handle != g_spurs_job_ls_handle) return 0;
    return s_job_ls;
}

/* Last outbound mailbox posted by a finished job (see below). */
/* THREAD-LOCAL. A job's answer is read back by the completion event that
 * follows it, and jc_execute signals immediately after each job -- but the walk
 * runs on whichever guest thread drives the chain, and more than one can be in
 * flight. As plain globals these were clobbered between a job finishing and its
 * own completion being pushed, so a query could be answered with an unrelated
 * job's mailbox. Per-thread keeps each job paired with its own answer. */
/* __declspec(thread) for MSVC only: MinGW GCC ignores it, which would make
 * these process-wide. spurs_job.c and cellSpurs.c must agree. */
#if defined(_MSC_VER)
#  define SPURS_JOB_TLS __declspec(thread)
#else
#  define SPURS_JOB_TLS __thread
#endif
SPURS_JOB_TLS uint32_t g_spurs_job_mbox, g_spurs_job_mbox_intr;
/* The command this job ran, read from its own command block. Only a query
 * expects an answer back through the completion event. */
SPURS_JOB_TLS uint32_t g_spurs_job_cmd;
SPURS_JOB_TLS int g_spurs_job_mbox_valid;
/* The SPU number a job sees (cellSpursGetCurrentSpuId, kernel LS 0x1C8). A host
 * thread that runs jobs concurrently with others must claim its own: titles
 * key per-SPU resources on it (Edge output rings, one RSX label per SPU), and
 * two jobs that both think they are SPU 0 corrupt each other's ring. */
SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;
void spurs_job_set_spu_num(uint32_t n) { g_spurs_job_spu_num = n; }

/* HV_SKINDUMP recorder: one record per DMA of the captured job --
 * "P"/"G" tag, LSA, EA, size, then the data. */
extern SPURS_JOB_TLS void (*g_spu_dma_rec)(spu_context*, uint32_t, uint32_t, uint32_t, uint32_t);
static SPURS_JOB_TLS FILE* spurs_job_rec_file;
static void spurs_job_rec_dma(spu_context* s, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t cmd)
{
    FILE* f = spurs_job_rec_file;
    if (!f || !size || size > 0x4000u) return;
    const int get = (cmd & 0x40) != 0, put = (cmd & 0x20) != 0 && !get;   /* as spu_dma.h's mfc_is_get/put */
    if (!put && !get) return;
    uint32_t h[4] = { put ? 0x54555050u /* "PPUT" */ : 0x54454747u /* "GGET" */, lsa, ea, size };
    fwrite(h, 4, 4, f);
    fwrite(put ? s->ls + (lsa & (SPU_LS_SIZE - 1u)) : vm_base + ea, 1, size, f);
}

/* ---------------------------------------------------------------------------
 * Native skinning (.hack//Versus "Skinning Geometry", the 27664-byte job).
 *
 * Worked out from captured jobs (HV_SKINDUMP) and checked against them. The job
 * descriptor's user data (big-endian):
 *   +0x30 size, +0x34 EA   matrices for positions   (input DMA list entry 0)
 *   +0x38 size, +0x3C EA   matrices for directions  (entry 1, same size)
 *   +0x54 vertex type      0x11 or 0x13
 *   +0x5C vertex count     (<= 256)
 *   +0x64 EA of the input vertices, +0x6C EA of the output vertices
 * A matrix is 4x4 floats, 64 bytes, stored by columns; a bone index selects one.
 * Input vertex: N vectors of 4 floats (0x11: position, normal; 0x13: position,
 * normal, tangent, binormal), then 4 u32 bone indices and 4 float weights.
 * Output vertex: the N vectors, each blended over the 4 bones -- the position
 * with the first matrix set (w taken as 1), the others with the second (w 0) --
 * and written with w = 0. The job reads and writes nothing else.
 *
 * On by default (HV_NATIVE_SKIN=1): it runs instead of the lifted SPU code, 5x
 * faster on the S22 (50 vs 247 us a job, ~170 jobs a frame in a fight). =0 runs
 * the SPU code; =2 runs both and compares (the SPU output is kept), logging the
 * largest difference -- 450k jobs matched within 1.7e-5 relative.
 * -------------------------------------------------------------------------*/
static inline float hv_bef(const uint8_t* p)
{
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    float f; memcpy(&f, &v, 4); return f;
}
static inline void hv_putbef(uint8_t* p, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

#include <time.h>
/* HV_SKINTIME=1: average wall time of a skinning job (native or SPU), every 5000 jobs. */
static double hv_now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
static void hv_skin_time(int native, double us)
{
    static int on = -1;
    if (on < 0) on = getenv("HV_SKINTIME") != NULL;
    if (!on) return;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static double tot[2]; static unsigned long long cnt[2];
    pthread_mutex_lock(&mu);
    tot[native] += us; cnt[native]++;
    if ((cnt[native] % 5000) == 0)
        fprintf(stderr, "[skin-time] %s: %llu jobs, %.1f us per job\n", native ? "native" : "spu",
                cnt[native], tot[native] / cnt[native]);
    pthread_mutex_unlock(&mu);
}

/* Returns 0 when done, -1 when the job is not one it knows (the SPU code runs). */
static int hv_native_skin(uint32_t job_ea, uint8_t* out_override)
{
    const uint32_t msz0 = g32(job_ea + 0x30), mea0 = g32(job_ea + 0x34);
    const uint32_t msz1 = g32(job_ea + 0x38), mea1 = g32(job_ea + 0x3C);
    const uint32_t type = g32(job_ea + 0x54), n = g32(job_ea + 0x5C);
    const uint32_t ein = g32(job_ea + 0x64), eout = g32(job_ea + 0x6C);
    if ((type != 0x11 && type != 0x13) || !n || n > 256 || !msz0 || msz0 != msz1 ||
        (msz0 & 63) || msz0 > 0x4000 || !mea0 || !mea1 || !ein || !eout)
        return -1;
    const uint32_t nb = msz0 / 64, nvec = type == 0x11 ? 2 : 4;
    const uint32_t si = nvec * 16 + 32, so = nvec * 16;
    /* matrices by rows: m[b][r][c] = column c, row r. Converted the first time a
     * vertex of this job uses that bone (a job's 256 vertices use a handful of the
     * model's up to ~200 bones); the stamp says which job converted it. */
    static __thread float mp[256][4][4], md[256][4][4];
    static __thread uint32_t stamp[256], job_no;
    if (++job_no == 0) { memset(stamp, 0, sizeof stamp); job_no = 1; }
    const uint8_t* in = vm_base + ein;
    uint8_t* out = out_override ? out_override : vm_base + eout;
    for (uint32_t i = 0; i < n; i++, in += si, out += so) {
        const uint8_t* ip = in + nvec * 16;
        uint32_t bi[4]; float w[4];
        for (int k = 0; k < 4; k++) {
            bi[k] = ((uint32_t)ip[4 * k] << 24) | ((uint32_t)ip[4 * k + 1] << 16) |
                    ((uint32_t)ip[4 * k + 2] << 8) | ip[4 * k + 3];
            if (bi[k] >= nb) bi[k] = nb - 1;
            w[k] = hv_bef(ip + 16 + 4 * k);
            const uint32_t b = bi[k];
            if (stamp[b] != job_no) {
                stamp[b] = job_no;
                for (int c = 0; c < 4; c++)
                    for (int r = 0; r < 4; r++) {
                        mp[b][r][c] = hv_bef(vm_base + mea0 + b * 64 + c * 16 + r * 4);
                        md[b][r][c] = hv_bef(vm_base + mea1 + b * 64 + c * 16 + r * 4);
                    }
            }
        }
        for (uint32_t v = 0; v < nvec; v++) {
            const float x = hv_bef(in + v * 16), y = hv_bef(in + v * 16 + 4), z = hv_bef(in + v * 16 + 8);
            const float t = v == 0 ? 1.0f : 0.0f;
            float (*m)[4][4] = v == 0 ? mp : md;
            float acc[3] = { 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                const float (*mk)[4] = m[bi[k]];
                for (int r = 0; r < 3; r++)
                    acc[r] += w[k] * (mk[r][0] * x + mk[r][1] * y + mk[r][2] * z + mk[r][3] * t);
            }
            hv_putbef(out + v * 16, acc[0]);
            hv_putbef(out + v * 16 + 4, acc[1]);
            hv_putbef(out + v * 16 + 8, acc[2]);
            hv_putbef(out + v * 16 + 12, 0.0f);
        }
    }
    return 0;
}

int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size)
{
    if (!entry || !job_ea) return -1;

    /* ---- descriptor ---------------------------------------------------- */
    uint32_t ea_bin   = (uint32_t)(g64(job_ea + JH_EA_BINARY) & ~1ull);
    uint32_t size_bin = (uint32_t)g16(job_ea + JH_SIZE_BINARY) << 4;
    uint32_t n_dma    = (uint32_t)g16(job_ea + JH_SIZE_DMA_LIST) / 8u;
    uint32_t use_io   = g32(job_ea + JH_USE_INOUT_BUFFER);
    uint32_t size_io  = g32(job_ea + JH_SIZE_IN_OR_INOUT);
    uint32_t size_out = g32(job_ea + JH_SIZE_OUT);
    uint32_t size_stk = (uint32_t)g16(job_ea + JH_SIZE_STACK) << 4;
    uint32_t size_scr = (uint32_t)g16(job_ea + JH_SIZE_SCRATCH) << 4;
    uint32_t n_cache  = g32(job_ea + JH_SIZE_CACHE_DMA_LIST) / 8u;

    /* HV_NATIVE_SKIN (see hv_native_skin): 1 = native instead of the SPU code,
     * 2 = both, compared. */
    static int s_native = -1;
    if (s_native < 0) { const char* e = getenv("HV_NATIVE_SKIN"); s_native = e ? atoi(e) : 1; }   /* on by default */
    uint8_t* skin_ref = NULL;
    if (s_native && size_bin == 27664u) {
        const double t0 = hv_now_us();
        if (s_native == 1 && hv_native_skin(job_ea, NULL) == 0) {
            hv_skin_time(1, hv_now_us() - t0);
            g_spurs_job_mbox = g_spurs_job_mbox_intr = 0;
            g_spurs_job_mbox_valid = 0;
            { uint32_t _cb = g32(job_ea + 0x4C); g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
            return 0;
        }
        if (s_native == 2) {
            skin_ref = (uint8_t*)malloc(256 * 64);
            if (skin_ref && hv_native_skin(job_ea, skin_ref) != 0) { free(skin_ref); skin_ref = NULL; }
        }
    }

    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;
    if (!size_bin || size_bin > SPU_LS_SIZE) {
        fprintf(stderr, "[spurs-job] job 0x%08X: bad sizeBinary=%u -- not run\n",
                job_ea, size_bin);
        return -1;
    }

    /* Staged straight into the context's own local store. It used to be a
     * fresh calloc'd 256 KB (on Android an mmap: 64 page faults), copied into
     * the context and freed -- per job, and .hack//Versus runs ~1500 a second. */
    static __thread spu_context* t_ctx;
    if (!t_ctx) {
        t_ctx = (spu_context*)calloc(1, sizeof(spu_context));
        if (!t_ctx) return -1;
    }
    spu_context* ctxp = t_ctx;
    /* Registers, channels and state fresh; the local store kept unless
     * HV_JOB_LS_ZERO=1 (then zeroed, as the old per-job calloc gave it). */
    {   static int zero = -1;
        if (zero < 0) zero = getenv("HV_JOB_LS_ZERO") != NULL;
        memset(ctxp, 0, offsetof(spu_context, ls_store));
        memset((uint8_t*)ctxp + offsetof(spu_context, ls), 0, sizeof(spu_context) - offsetof(spu_context, ls));
        if (zero) memset(ctxp->ls_store, 0, SPU_LS_SIZE);
        ctxp->ls = ctxp->ls_store;
        ctxp->status = SPU_STATUS_STOPPED;
    }
    uint8_t* ls = ctxp->ls;

    /* ---- binary at LS 0 (PIC, so delta 0 keeps the lifted addresses) ---- */
    memcpy(ls, vm_base + ea_bin, size_bin);

    /* ---- regions, following _cellSpursCheckJob's own arithmetic --------- */
    uint32_t p         = ALIGN1024(size_bin);
    /* Same rule as the scratch pointer: these are REGION BASES and stay valid
     * when the region is empty. NULL invites the job to dereference 0 -- and
     * since the job binary loads at LS 0, reading through a null base returns
     * the job's OWN INSTRUCTION WORDS. That is exactly what Tokyo Jungle's
     * wedged jobs were doing: each spun DMAing to the word at offset 0x64 of
     * its own image, which is why the addresses were misaligned and looked
     * like floats -- they were opcodes. */
    uint32_t io_ls     = p;                p += ALIGN1024(size_io);
    uint32_t out_ls    = p;                p += ALIGN1024(size_out);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    uint32_t ss_size   = ALIGN1024(size_scr + size_stk);
    /* The scratch pointer is the BASE OF THE SCRATCH+STACK BLOCK, and it is
     * valid even when sizeScratch is 0 -- a zero-length buffer still has an
     * address. Handing the job a NULL here meant it dereferenced 0 and read
     * ITS OWN CODE as data: every wedged job in Tokyo Jungle was loading a
     * word from LS 0x64 and using that SPU instruction word as a DMA address,
     * which is why the addresses looked like misaligned floats. */
    uint32_t s_ls      = p;
    uint32_t stack_top = p + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs ---------------------------------- */
    uint32_t dma_base   = job_ea + JH_SIZE;          /* input list ... */
    uint32_t cache_base = dma_base + n_dma * 8u;     /* ... then cache list */
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        uint64_t e   = g64(cache_base + i * 8u);
        uint32_t sz  = (uint32_t)(e >> 32);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (p + sz > SPU_LS_SIZE) break;
        cache_ls[i] = p;
        memcpy(ls + p, vm_base + eal, sz);
        p += ALIGN1024(sz);
    }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ---------- */
    uint32_t io_cur = io_ls, io_used = 0;
    for (uint32_t i = 0; i < n_dma; i++) {
        uint64_t e   = g64(dma_base + i * 8u);
        uint32_t sz  = (uint32_t)((e >> 32) & 0x7FFFu);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > SPU_LS_SIZE) break;
        memcpy(ls + io_cur, vm_base + eal, sz);
        io_cur  += (sz + 15u) & ~15u;
        io_used += (sz + 15u) & ~15u;
    }

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space. */
    uint32_t desc_ls = SPU_LS_SIZE - 0x400;     /* descriptors are <= 1024 B */
    uint32_t ctx_ls  = desc_ls - 0x40;          /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    if (p > ctx_ls)
        fprintf(stderr, "[spurs-job] job 0x%08X: WARNING regions reach 0x%X, "
                        "past the context at 0x%X\n", job_ea, p, ctx_ls);
    memcpy(ls + desc_ls, vm_base + job_ea, dsz);

    ls32(ls, ctx_ls + JC_IO_BUFFER, io_ls);
    for (int i = 0; i < 4; i++)
        ls32(ls, ctx_ls + JC_CACHE_BUFFER + (uint32_t)i * 4u, cache_ls[i]);
    /* sizeJobDescriptor:4 is the first bitfield of a big-endian u32, so it
     * occupies the TOP 4 bits. 0 means 64 bytes, otherwise n*128. */
    /* The low 28 bits are not padding to a "JOBCRT Ver13" crt (WWS/Edge jobs,
     * Twisted Metal's JobQueue jobs): jm2 parks an LS pointer to its kernel
     * block there, and the crt returns 0x80410A11 and STOPs before main when
     * it is 0 -- so the job never runs and never clears its completion
     * counter. Hand it the free 16 bytes after the context (zeroed); the crt
     * only stashes the pointer and clears the field again. */
    uint32_t jm_iface = memcmp(ls + 0x30, "JOBCRT Ver13", 12) == 0 ? ctx_ls + 0x30 : 0;
    /* The same crt also reads the SPURS kernel context with ABSOLUTE loads
     * (lqa): spuNum at 0x1C8 (cellSpursGetCurrentSpuId), the trace buffer at
     * 0x210, a trace-enable byte at 0x175. On hardware the kernel owns
     * 0x100-0x2FF; here the job is loaded at 0, so those reads returned the
     * job's own instruction words and it indexed per-SPU tables with an opcode
     * and halted. The lifted code never reads those bytes as data, so overlay
     * the kernel's view: SPU 0, our DMA tag, tracing off. */
    if (jm_iface) {
        memset(ls + 0x170, 0, 0x10);
        memset(ls + 0x1C0, 0, 0x10);
        ls32(ls, 0x1C8, g_spurs_job_spu_num);
        ls32(ls, 0x1CC, JOB_DMA_TAG);
        memset(ls + 0x210, 0, 0x10);
    }
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         ((dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28) | jm_iface);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  JOB_DMA_TAG);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0 | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* SPURS_JOB_DESCDUMP=1: hexdump the guest job descriptor. The SPU derives
     * its DMA and atomic EAs from these words, so when a job spins on a
     * lock-line address that is not backed, this is what to read first. */
    { static int _d = 0;
      if (getenv("SPURS_JOB_DESCDUMP") && PS3_LOG_FIRST(_d, 4)) {
          fprintf(stderr, "[spurs-job] desc @0x%08X (%u bytes):", job_ea, dsz);
          for (uint32_t o = 0; o < dsz && o < 128; o += 4) {
              if ((o & 31) == 0) fprintf(stderr, "\n    +%02X:", o);
              fprintf(stderr, " %08X", g32(job_ea + o));
          }
          fprintf(stderr, "\n");
          /* SPURS_JOB_DESCDUMP=2: follow plausible guest EAs in the job user
           * data (past JH_SIZE) and dump what they point at. The job reads its
           * parameters from there, so a wild DMA/atomic address usually
           * originates in one of these blocks. */
          if (atoi(getenv("SPURS_JOB_DESCDUMP")) >= 2) {
              for (uint32_t o = JH_SIZE; o < dsz && o < 128; o += 4) {
                  uint32_t v = g32(job_ea + o);
                  if (v < 0x10000u || v >= 0xD0000000u) continue;
                  fprintf(stderr, "[spurs-job]   user+0x%02X -> 0x%08X:", o, v);
                  for (uint32_t q = 0; q < 8; q++)
                      fprintf(stderr, " %08X", g32(v + q * 4));
                  fprintf(stderr, "\n");
              }
          }
      } }

    /* LBP job protocol probe (SPURS_JOB_DUMP): the game's one shared job binary
     * ("JOBCRT Ver13" crt + main @0x1570) reads a be u64 EA out of the
     * descriptor's USER DATA at +0x30, GETs a 128-byte command block from it,
     * then jump-tables on the command TYPE (word1 of the block, cases 0..5).
     * The header's input-DMA-list/io fields are legitimately zero for it. Dump
     * the user data + that command block so the bail path is attributable. */
    uint32_t dump_cmd = 0;
    if (getenv("SPURS_JOB_DUMP")) {
        uint64_t ud  = g64(job_ea + JH_SIZE);        /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)g64(job_ea + JH_SIZE + 8),
                (unsigned long long)g64(job_ea + JH_SIZE + 16),
                (unsigned long long)g64(job_ea + JH_SIZE + 24));
        if (cmd && cmd < 0x50000000u) {
            dump_cmd = cmd;
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X:", cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(cmd + o));
            }
            fprintf(stderr, "\n");
            /* cmdblock+0x1C points at a 2048-byte WORK-ITEM table the type-0/1
             * handler GETs (pc=0x1A78) before entering its per-item worker with
             * count = cmdblock+0x24. Dump its head: all-zero means the game
             * never filled the items (upstream/ordering); real entries mean the
             * SPU worker mishandles them. */
            uint32_t tbl = g32(cmd + 0x1C);
            if (tbl && tbl < 0x50000000u) {
                fprintf(stderr, "[spurs-job]   worktable @0x%08X (n=%u):", tbl, g32(cmd + 0x24));
                for (int o = 0; o < 96; o += 4) {
                    if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                    fprintf(stderr, " %08X", g32(tbl + o));
                }
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }

    /* HV_SKINDUMP=<n>: capture the first n skinning jobs (.hack//Versus "Skinning
     * Geometry", the 27664-byte binary) to HV_SKINDUMP_DIR (default "skindump"):
     * the local store as the job starts, then every DMA it issues with its data
     * (PUT: the LS bytes sent; GET: the guest bytes read). Reference data for the
     * native skinning path. */
    FILE* skd = NULL;
    {   static int lim = -1; static int taken;
        if (lim < 0) { const char* e = getenv("HV_SKINDUMP"); lim = e ? atoi(e) : 0; }
        if (lim > 0 && size_bin == 27664u) {
            int k = __atomic_fetch_add(&taken, 1, __ATOMIC_RELAXED);
            if (k < lim) {
                const char* d = getenv("HV_SKINDUMP_DIR");
                char path[512];
                snprintf(path, sizeof path, "%s/job_%05d.bin", d && *d ? d : "skindump", k);
                skd = fopen(path, "wb");
            }
        }
    }
    if (skd) {
        uint32_t hdr[6] = { 0x31444B53u /* "SKD1" */, job_ea, dsz, ctx_ls, desc_ls, (uint32_t)(stack_top - 32u) & ~15u };
        fwrite(hdr, 4, 6, skd);
        fwrite(ls, 1, SPU_LS_SIZE, skd);
        spurs_job_rec_file = skd;
        g_spu_dma_rec = spurs_job_rec_dma;
    }

    /* ---- enter the job -------------------------------------------------- */
    ctxp->image_id = image_id;
    /* SPU stack grows down. A function saves its link register in its
     * caller's frame, at 16($sp), so the entry's first store lands 16 bytes
     * above $sp. The cache buffers start right at stack_top: with $sp at
     * stack_top - 16 that save overwrote the first quadword of
     * cacheBuffer[0] (Twisted Metal's Edge jobs read their viewport there and
     * got width = height = 0). Leave the caller's 32-byte frame inside the
     * stack. */
    ctxp->gpr[1]._u32[0] = (stack_top - 32u) & ~15u;
    /* Link register: where the job returns when it is done. The job manager
     * would pass an address inside itself; 0 makes the job re-enter its own
     * entry at LS 0 and run a bogus second lap. */
    ctxp->gpr[0]._u32[0] = 0x3FF00u;                   /* SPU_JOB_RETURN_LS */
    ctxp->gpr[3]._u32[0] = ctx_ls;                     /* CellSpursJobContext2* */
    ctxp->gpr[4]._u32[0] = desc_ls;                    /* CellSpursJob256*      */

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[spurs-job] ENTER r3=%08X r4=%08X r1=%08X entry-fn image=%d\n",
                  ctxp->gpr[3]._u32[0], ctxp->gpr[4]._u32[0], ctxp->gpr[1]._u32[0],
                  image_id); }
    /* NOTE: after its work the job's runtime jumps to LS 0 ("return to the
     * jm2 kernel" -- real layout has the kernel at 0, jobs above; we load the
     * job at 0). That second lap re-enters the job's crt with dead registers
     * and trips its parameter guard, which HALTS -- ending the run. So the
     * HALT-ASSERT heqi 0x1650 seen once per jm2 job is NORMAL COMPLETION
     * noise, not a failure: the job's real work finished before the jump. */
    const double t_run = hv_now_us();
    spu_run_with_halt(entry, ctxp);
    if (size_bin == 27664u && !skin_ref) hv_skin_time(0, hv_now_us() - t_run);
    if (skin_ref) {
        /* HV_NATIVE_SKIN=2: the SPU's output against the native one, float by float */
        static double s_maxd; static unsigned long long s_jobs, s_bad, s_floats;
        const uint32_t type = g32(job_ea + 0x54), n = g32(job_ea + 0x5C), eout = g32(job_ea + 0x6C);
        const uint32_t nf = n * (type == 0x11 ? 8u : 16u);
        double md = 0; unsigned bad = 0;
        for (uint32_t k = 0; k < nf; k++) {
            const float a = hv_bef(vm_base + eout + 4 * k), b = hv_bef(skin_ref + 4 * k);
            const double d = a > b ? (double)a - b : (double)b - a;
            const double rel = d / (1.0 + (a < 0 ? -a : a));
            if (rel > md) md = rel;
            if (rel > 1e-4) bad++;
        }
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&mu);
        s_jobs++; s_floats += nf; s_bad += bad;
        if (md > s_maxd) s_maxd = md;
        if (bad && s_bad == bad)
            fprintf(stderr, "[native-skin] first mismatch: job 0x%08X type 0x%X n %u: %u floats off, max rel %.3g\n",
                    job_ea, type, n, bad, md);
        if ((s_jobs % 2000) == 0)
            fprintf(stderr, "[native-skin] %llu jobs, %llu floats: %llu off by > 1e-4, max rel diff %.3g\n",
                    s_jobs, s_floats, s_bad, s_maxd);
        pthread_mutex_unlock(&mu);
        free(skin_ref);
    }
    if (skd) {
        g_spu_dma_rec = NULL;
        spurs_job_rec_file = NULL;
        uint32_t end[2] = { 0x444E4553u /* "SEND" */, 0 };
        fwrite(end, 4, 2, skd);
        fclose(skd);
    }

    /* Capture the job's outbound mailbox. These are LS POINTERS TO STRINGS for
     * the SPU printf service, NOT query results -- value 0x83C0 from the sound
     * job points at " compressor, 1/n..." inside its own image. An earlier
     * comment here claimed they carried the title's bus counts; they do not,
     * and feeding them into the completion event turned 0x40000000 into a 1 GB
     * allocation request. Kept because the printf service needs them. */
    /* Keep the store readable for the SPU printf service, which follows a
     * mailbox pointer into it -- only a job that posted one needs it, and the
     * 256 KB copy under a lock serialised the parallel jobs (HV_JC_PAR).
     * SPURS_JOB_LS_KEEP=1 copies after every job, as before. */
    {   static int keep = -1;
        if (keep < 0) keep = getenv("SPURS_JOB_LS_KEEP") != NULL;
        if (keep || spu_channel_has_data(&ctxp->ch_out_mbox) || spu_channel_has_data(&ctxp->ch_out_intr_mbox)) {
            static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&mu);
            memcpy(s_job_ls, ctxp->ls, SPU_LS_SIZE);
            pthread_mutex_unlock(&mu);
        }
    }
    /* TEMP SPURS_JOB_LSWORD=<img>:<lsa>: after a job of that image, print the
     * LS word at lsa and every other LS offset holding the same value. */
    { static int img = -2; static uint32_t lsa; static int n = 0;
      if (img == -2) { const char* e = getenv("SPURS_JOB_LSWORD"); img = -1;
          if (e) { img = atoi(e); const char* c = strchr(e, ':'); lsa = c ? (uint32_t)strtoul(c + 1, 0, 16) : 0; } }
      if (img == image_id && n++ < 3) {
          const uint8_t* q = ctxp->ls + (lsa & 0x3FFFC);
          uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
          fprintf(stderr, "[lsword] img=%d job=0x%08X LS[0x%05X]=0x%08X io=0x%05X also at:", image_id, job_ea, lsa, v, io_ls);
          for (uint32_t o = 0; o + 4 <= SPU_LS_SIZE; o += 4) {
              const uint8_t* p = ctxp->ls + o;
              if (o != (lsa & 0x3FFFC) && ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) == v)
                  fprintf(stderr, " 0x%05X", o);
          }
          fprintf(stderr, "\n");
      } }
    s_job_ls_valid = 1;

    g_spurs_job_mbox      = spu_channel_has_data(&ctxp->ch_out_mbox)
                          ? ctxp->ch_out_mbox.value : 0;
    g_spurs_job_mbox_intr = spu_channel_has_data(&ctxp->ch_out_intr_mbox)
                          ? ctxp->ch_out_intr_mbox.value : 0;
    g_spurs_job_mbox_valid = g_spurs_job_mbox || g_spurs_job_mbox_intr;
    { uint32_t _cb = g32(job_ea + 0x4C);
      g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_JOB_MBOX") ? 1 : 0;
      static int n = 0;
      if (s_t && PS3_LOG_FIRST(n, 16) && g_spurs_job_mbox_valid)
          fprintf(stderr, "[spurs-job] job 0x%08X posted mbox=0x%08X intr=0x%08X\n",
                  job_ea, g_spurs_job_mbox, g_spurs_job_mbox_intr); }

    if (getenv("SPURS_JOB_DUMP")) {
        fprintf(stderr, "[spurs-job] job 0x%08X exit: status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, ctxp->status, ctxp->stop_code, ctxp->pc);
        /* Post-run view of the same command block: the game polls result/status
         * fields the job writes back -- a before/after diff shows whether the
         * handler delivered its completion or the PPU waits on stale state. */
        if (dump_cmd) {
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X AFTER:", dump_cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(dump_cmd + o));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    spu_mfc_release(ctxp);
    spu_coh_unregister(ctxp);   /* stack local: out of the reserver set */
    { extern void spu_mfc_release(spu_context*); spu_mfc_release(ctxp); }
    return 0;
}
