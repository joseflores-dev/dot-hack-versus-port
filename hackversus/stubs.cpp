/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <thread>
#include <chrono>
#include <string>
#include <strings.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

extern "C" void ppu_dump_all_guest_stacks(const char* tag);

/* `kill -USR2 <pid>` from adb: every guest thread's stack, for hangs where all
 * host threads sit in futex waits and nothing logs. Not async-signal-safe --
 * a debugging aid only. */
static void on_sigusr2(int)
{
    std::fprintf(stderr, "[SIGUSR2] guest stacks:\n");
    ppu_dump_all_guest_stacks("SIGUSR2");
}

extern "C" void hv_prof_start(void);

extern "C" void ps3_load_prx_modules(void)
{
#ifndef _WIN32
    std::signal(SIGUSR2, on_sigusr2);
#endif
    hv_prof_start();   /* HV_PROF=<seconds> */
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */

/* ---------------------------------------------------------------------------
 * Character model mods. func_0004BB04 builds "data/chr/<code>chrpsn.xfbin" with sprintf (the code -- 1kit01, ...
 * -- from func_0004293C); recomp/ppu_recomp_001.cpp calls hv_chr_path_hook right after it with the guest buffer
 * and the player object, whose byte +0x924 is the colour picked in the character menu (0 = 1st, 1 = 2nd).
 * The file name must stay as it is (the title files the loaded resources under it; asking for another entry
 * loaded fine and drew nothing), so a mod changes where the data comes from instead: the title keeps the
 * DATA.CPK index in guest memory, and the entry's row (FileSize, ExtractSize, FileOffset: 16 bytes) is pointed
 * at the mod's data -- appended to the CPK by the installer -- while that player's model is read, and back to
 * the original for any other.
 *   HV_MODS=<file>: lines "swap <code> <offset hex> <value> <row position in the CPK> <original row hex>
 *                   <mod row hex>" (written by the installer, descargas/mods-azurekite/install_pc.sh).
 *   HV_CHRLOG=1: log each model file asked for and what the mods did.
 * -----------------------------------------------------------------------*/
extern uint8_t* vm_base;
extern "C" void (*g_hv_fs_read_hook)(int fd, long long pos, uint32_t buf, size_t n);
extern "C" int ppu_fs_fd_host_path(int fd, char* out, size_t cap);
static int replace_rows(const uint8_t* from, const uint8_t* to);
namespace {
/* colour: 0 swap/swapg, 1 swapc, 2 subst (a character's extra colour), 3 weapon (an extra weapon on a carrier) */
struct ChrSwap { char code[16]; unsigned off, val; long long rowpos; uint8_t orig[16], repl[16], orig_abs[16], repl_abs[16]; uint32_t guest; int on; uint32_t gaddr; int colour;
                 long long subst_at, subst_len; uint8_t* subst; int slot; char patch[1000]; int building; char name[48];
                 uint8_t* icon; int icon_tried; };
enum { MAX_SWAPS = 64 };
ChrSwap s_swaps[MAX_SWAPS];
uint8_t s_icon_sig[64];          /* "weaponicons": the carrier's picture and name (hv_weapon_hook) */
int s_icon_rel = -1;
char s_icon_name[48];
int s_nswaps = -1;
int s_chrlog = -1;

int hexbytes(const char* h, uint8_t* out)
{
    for (int i = 0; i < 16; i++) { unsigned v; if (std::sscanf(h + 2 * i, "%2x", &v) != 1) return 0; out[i] = (uint8_t)v; }
    return 1;
}

void watch_setup();

/* ---- mod patches (.hvpatch, descargas/mods-azurekite/make_patch.py) ----
 * A patch holds only what the mod changes. The entry it replaces is built here from the player's own DATA.CPK the
 * first time the title reads that file: its 128 KB chunks are unpacked (CRILAYLA), the patch's copy / insert / zero
 * operations make the modded file, and that is packed back into the entry's format and size. The result is kept
 * next to the patch (<patch>.built), so this happens once. */
uint32_t crc32_of(const uint8_t* p, size_t n)
{
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* CRILAYLA: the first 0x100 bytes stored plain at the end; the rest coded from the last byte to the first in a bit
 * stream read backwards (0 + 8-bit literal | 1 + 13-bit distance - 3 + length - 3 in groups of 2, 3, 5, 8, 8, ...) */
int crilayla_unpack(const uint8_t* s, size_t n, uint8_t* out, uint32_t want)
{
    if (n < 16 || std::memcmp(s, "CRILAYLA", 8)) return 0;
    uint32_t usize, hoff;
    std::memcpy(&usize, s + 8, 4); std::memcpy(&hoff, s + 12, 4);
    if (usize + 0x100 != want || 16 + (size_t)hoff + 0x100 > n) return 0;
    std::memcpy(out, s + 16 + hoff, 0x100);
    long ptr = 16 + (long)hoff - 1;
    uint32_t pool = 0; int left = 0;
    auto bits = [&](int k) { uint32_t v = 0; while (k) { if (!left) { pool = ptr >= 16 ? s[ptr--] : 0; left = 8; }
                             const int t = left < k ? left : k; v = (v << t) | ((pool >> (left - t)) & ((1u << t) - 1)); left -= t; k -= t; } return v; };
    const long end = 0x100 + (long)usize - 1;
    long done = 0;
    static const int vle[4] = { 2, 3, 5, 8 };
    while (done < (long)usize) {
        if (bits(1)) {
            long ref = end - done + bits(13) + 3, len = 3;
            for (int lvl = 0;; ) { const uint32_t x = bits(vle[lvl]); len += x; if (x != (1u << vle[lvl]) - 1) break; if (lvl < 3) lvl++; }
            if (ref > end) return 0;
            while (len-- && done < (long)usize) { out[end - done] = out[ref--]; done++; }
        } else {
            out[end - done] = (uint8_t)bits(8); done++;
        }
    }
    return 1;
}

/* the same coder as descargas/mods-azurekite/crilayla_compress.c; returns the size written to out (0: no gain) */
size_t crilayla_pack(const uint8_t* d, long n, uint8_t* out, size_t cap)
{
    if (n <= 0x100) return 0;
    enum { WIN = 8194, HB = 16, CHAIN = 64 };
    uint8_t* bitv = (uint8_t*)std::malloc((size_t)n * 9 + 64);
    int32_t* head = (int32_t*)std::malloc(sizeof(int32_t) << HB);
    int32_t* prev = (int32_t*)std::malloc(sizeof(int32_t) * (size_t)n);
    size_t nb = 0;
    auto put = [&](uint32_t v, int k) { for (int i = k - 1; i >= 0; i--) bitv[nb++] = (v >> i) & 1; };
    auto H = [&](long p) { return ((((uint32_t)d[p] << 10) ^ ((uint32_t)d[p - 1] << 5) ^ d[p - 2]) & ((1u << HB) - 1)); };
    for (int i = 0; i < 1 << HB; i++) head[i] = -1;
    const long lo = 0x100;
    long p = n - 1, inserted = n;
    while (p >= lo && nb + 64 < (size_t)n * 8) {
        while (inserted - 1 >= p + 3) { const long q = --inserted; if (q - 2 >= 0) { const uint32_t h = H(q); prev[q] = head[h]; head[h] = (int32_t)q; } }
        long best = 0, bq = 0;
        if (p - 2 >= lo) {
            int32_t q = head[H(p)]; int depth = 0;
            while (q >= 0 && q - p <= WIN && depth++ < CHAIN) {
                long k = 0;
                while (p - k >= lo && d[p - k] == d[q - k]) { k++; if (k >= 0x7FFF) break; }
                if (k > best) { best = k; bq = q; }
                q = prev[q];
            }
        }
        if (best >= 3) {
            put(1, 1); put((uint32_t)(bq - p - 3), 13);
            long rem = best - 3; static const int vle[4] = { 2, 3, 5, 8 }; int lvl = 0;
            for (;;) { const int b = vle[lvl]; const long mx = (1L << b) - 1; const long v = rem < mx ? rem : mx;
                       put((uint32_t)v, b); rem -= v; if (v != mx) break; if (lvl < 3) lvl++; }
            p -= best;
        } else {
            put(0, 1); put(d[p], 8); p--;
        }
    }
    size_t total = 0;
    const size_t comp = ((nb + 7) / 8 + 3) & ~(size_t)3;
    if (p < lo && 16 + comp + 0x100 < (size_t)n && 16 + comp + 0x100 <= cap) {
        std::memcpy(out, "CRILAYLA", 8);
        const uint32_t usize = (uint32_t)(n - 0x100), hoff = (uint32_t)comp;
        std::memcpy(out + 8, &usize, 4); std::memcpy(out + 12, &hoff, 4);
        uint8_t* c = out + 16;
        std::memset(c, 0, comp);
        for (size_t i = 0; i < nb; i++) if (bitv[i]) c[comp - 1 - i / 8] |= (uint8_t)(1 << (7 - (int)(i % 8)));
        std::memcpy(c + comp, d, 0x100);
        total = 16 + comp + 0x100;
    }
    std::free(bitv); std::free(head); std::free(prev);
    return total;
}

uint8_t* read_file(const char* path, size_t* n)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END); const long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    uint8_t* d = sz > 0 ? (uint8_t*)std::malloc((size_t)sz) : nullptr;
    if (d && std::fread(d, 1, (size_t)sz, f) != (size_t)sz) { std::free(d); d = nullptr; }
    std::fclose(f);
    if (d) *n = (size_t)sz;
    return d;
}

uint32_t le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

void be_put(uint8_t* p, uint64_t v, int k) { for (int i = k - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }

/* the modded entry from the patch and the original entry; nullptr (and a log line) if anything does not match */
uint8_t* build_from_patch(const ChrSwap& w, const uint8_t* entry)
{
    size_t pn = 0;
    uint8_t* pt = read_file(w.patch, &pn);
    if (!pt) { std::fprintf(stderr, "[mods] %s: cannot read %s\n", w.code, w.patch); return nullptr; }
    uint8_t *src = nullptr, *dst = nullptr, *out = nullptr;
    const char* why = nullptr;
    do {
        if (pn < 44 || std::memcmp(pt, "HVPATCH1", 8)) { why = "not a patch"; break; }
        const uint32_t elen = le32(pt + 8), ecrc = le32(pt + 12), slen = le32(pt + 16), dlen = le32(pt + 20), dcrc = le32(pt + 24);
        const uint32_t content = le32(pt + 28), align = le32(pt + 32), nch = le32(pt + 36);
        if (elen != (uint64_t)w.subst_len) { why = "made for another entry size"; break; }
        if (crc32_of(entry, elen) != ecrc) { why = "this DATA.CPK's entry is not the one the patch was made from"; break; }
        const uint8_t* tb = pt + 40;
        const uint8_t* q = tb + 16 * (size_t)nch;
        if (q + 12 > pt + pn) { why = "truncated"; break; }
        src = (uint8_t*)std::malloc(slen); dst = (uint8_t*)std::malloc(dlen);
        size_t at = 0;
        for (uint32_t i = 0; i < nch && !why; i++) {
            const uint32_t o = le32(tb + 16 * i), fs = le32(tb + 16 * i + 4), es = le32(tb + 16 * i + 8);
            if ((uint64_t)o + fs > elen || at + es > slen) why = "bad chunk table";
            else if (fs < es ? !crilayla_unpack(entry + o, fs, src + at, es) : (std::memcpy(src + at, entry + o, es), 0)) why = "a chunk does not unpack";
            at += es;
        }
        if (why) break;
        const uint32_t pos_cs = le32(q), pos_eds = le32(q + 4), nops = le32(q + 8);
        const uint8_t* op = q + 12;
        const uint8_t* ins = op + 9 * (size_t)nops;
        size_t t = 0;
        for (uint32_t i = 0; i < nops && !why; i++, op += 9) {
            const uint32_t a = le32(op + 1), len = le32(op + 5);
            if (t + len > dlen) { why = "operations overrun"; break; }
            if (op[0] == 0) { if ((uint64_t)a + len > slen) why = "copy out of range"; else std::memcpy(dst + t, src + a, len); }
            else if (op[0] == 1) { if (ins + len > pt + pn) why = "truncated"; else { std::memcpy(dst + t, ins, len); ins += len; } }
            else std::memset(dst + t, 0, len);
            t += len;
        }
        if (why) break;
        if (t != dlen || crc32_of(dst, dlen) != dcrc) { why = "the result does not check out"; break; }
        /* pack: the original header and chunk table, then each chunk compressed (plain if that does not shrink it),
         * aligned, in ID order; the sizes in the table follow */
        out = (uint8_t*)std::calloc(elen, 1);
        std::memcpy(out, entry, content);
        size_t pos = content, data = 0, d0 = 0;
        for (uint32_t i = 0; i < nch && !why; i++) {
            const uint32_t es = le32(tb + 16 * i + 8), field = le32(tb + 16 * i + 12);
            pos += (align - pos % align) % align;
            if (pos >= elen) { why = "does not fit"; break; }
            size_t c = crilayla_pack(dst + d0, (long)es, out + pos, elen - pos);
            if (!c) { if (pos + es > elen) { why = "does not fit"; break; } std::memcpy(out + pos, dst + d0, es); c = es; }
            be_put(out + field, c, 4);
            pos += c; data += c; d0 += es;
        }
        if (why) break;
        if (pos_cs != 0xFFFFFFFFu) be_put(out + pos_cs, pos - content, 8);
        if (pos_eds != 0xFFFFFFFFu) be_put(out + pos_eds, data, 8);
    } while (0);
    std::free(pt); std::free(src); std::free(dst);
    if (why) { std::fprintf(stderr, "[mods] %s: %s: %s\n", w.code, w.patch, why); std::free(out); return nullptr; }
    return out;
}

void build_patch_thread(int i, std::string cpk)
{
    ChrSwap& w = s_swaps[i];
    const auto t0 = std::chrono::steady_clock::now();
    uint8_t* entry = (uint8_t*)std::malloc((size_t)w.subst_len);
    FILE* f = std::fopen(cpk.c_str(), "rb");
    #ifdef _WIN32
    const bool sought = f && _fseeki64(f, w.subst_at, SEEK_SET) == 0;
#else
    const bool sought = f && fseeko(f, (off_t)w.subst_at, SEEK_SET) == 0;
#endif
    const bool ok = sought && entry &&
                    std::fread(entry, 1, (size_t)w.subst_len, f) == (size_t)w.subst_len;
    if (f) std::fclose(f);
    uint8_t* built = ok ? build_from_patch(w, entry) : nullptr;
    std::free(entry);
    if (!built) { if (!ok) std::fprintf(stderr, "[mods] %s: cannot read the entry from %s\n", w.code, cpk.c_str()); return; }
    char cache[1100];
    std::snprintf(cache, sizeof cache, "%s.built", w.patch);
    if (FILE* o = std::fopen(cache, "wb")) { std::fwrite(built, 1, (size_t)w.subst_len, o); std::fclose(o); }
    __atomic_store_n(&w.subst, built, __ATOMIC_RELEASE);
    std::fprintf(stderr, "[mods] %s: built from %s and the patch in %.1f s\n", w.code, cpk.c_str(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

void load_mods()
{
    if (s_nswaps >= 0) return;
    s_nswaps = 0;
    s_chrlog = std::getenv("HV_CHRLOG") != nullptr;
    /* HV_MODS, or data/hv_mods.txt next to the PC build, or hv_mods.txt in the APK's data folder */
    const char* path = std::getenv("HV_MODS");
    static char def[600];
    if (!path || !*path) {
        const char* hdd = std::getenv("PS3_HDD0_ROOT");   /* <data>/dev_hdd0 (PC) or <files>/PS3/dev_hdd0 (APK) */
        if (hdd && *hdd) {
            std::snprintf(def, sizeof def, "%s/../hv_mods.txt", hdd);
            path = def;
        } else {
            path = "data/hv_mods.txt";
        }
    }
    FILE* f = std::fopen(path, "r");
    if (!f) return;
    char dir[600];
    std::snprintf(dir, sizeof dir, "%s", path);
    { char* sl = std::strrchr(dir, '/'); char* bs = std::strrchr(dir, 92); if (bs > sl) sl = bs; if (sl) sl[1] = 0; else dir[0] = 0; }
    char line[512];
    unsigned nweapons = 0;
    while (s_nswaps < MAX_SWAPS && std::fgets(line, sizeof line, f)) {
        ChrSwap w = {};
        /* "weapon <carrier> <offset hex> <length> <file.hvpatch> <name>": one more weapon in the PC Costume Settings
         * weapon selector. <carrier> is a weapon of the title (its index in the selector's list: weapon_04 = 4) whose
         * file is the entry [offset, offset+length) of the CPK; the selector shows the extra weapons right after it,
         * in the order of these lines, and while one of them is chosen the carrier's file is served from the patch. */
        /* "weaponicons <offset> <64 bytes hex> <name>": where the carrier's picture and name are, so the selector shows
         * each extra weapon's own (<file>.icon next to its patch): 64 bytes of the weapon pictures atlas (csel_wpn_icon,
         * 1024 wide, DXT5: 4096 bytes a row of blocks; a picture is 256x128, 1024 bytes a row of blocks, 32 rows) at
         * <offset> from the start of the carrier's picture, and the carrier's name as the title keeps it in memory. */
        unsigned rel = 0; char sig[160] = {}, nm[48] = {};
        if (std::sscanf(line, "weaponicons %u %159s %47[^\r\n]", &rel, sig, nm) == 3 && std::strlen(sig) == 128) {
            for (int i = 0; i < 64; i++) { unsigned v; std::sscanf(sig + 2 * i, "%2x", &v); s_icon_sig[i] = (uint8_t)v; }
            s_icon_rel = rel; std::snprintf(s_icon_name, sizeof s_icon_name, "%s", nm);
            continue;
        }
        char wfile[400];
        if (std::sscanf(line, "weapon %d %llx %lld %399s %47[^\r\n]", &w.slot, &w.subst_at, &w.subst_len, wfile, w.name) >= 4) {
            char full[1000];
            if (wfile[0] == '/' || wfile[0] == 92 || (wfile[0] && wfile[1] == ':')) std::snprintf(full, sizeof full, "%s", wfile);
            else std::snprintf(full, sizeof full, "%s%s", dir, wfile);
            std::snprintf(w.patch, sizeof w.patch, "%s", full);
            std::snprintf(w.code, sizeof w.code, "weapon %u", ++nweapons);
            w.val = nweapons;
            w.colour = 3;
            char cache[1100];
            std::snprintf(cache, sizeof cache, "%s.built", full);
            size_t n = 0;
            uint8_t* d = read_file(cache, &n);
            if (d && n == (size_t)w.subst_len) w.subst = d;
            else std::free(d);
            s_swaps[s_nswaps++] = w;
            continue;
        }
        /* "subst <code> <colour index> <carousel slot> <offset hex> <length> <file>": for the character at that slot of
         * the character menu (widget +0x8C: Sora 0, Sakuya 1, Haseo 2, Kite 3, BlackRose 4, Tsukasa 5, Ovan 6, Tokio
         * 7), the colour selector gets that one more colour, and while it shows it,
         * reads of the entry's bytes in the CPK [offset, offset+length) are served from <file> instead (same size
         * and format as the entry: make_chunked.py) -- CriFs and the index are not touched. */
        char sp[400];
        if (std::sscanf(line, "subst %15s %u %d %llx %lld %399[^\r\n]", w.code, &w.val, &w.slot, &w.subst_at, &w.subst_len, sp) == 6) {
            char full[1000];   /* relative to the mods file */
            if (sp[0] == '/' || sp[0] == 92 || (sp[0] && sp[1] == ':')) std::snprintf(full, sizeof full, "%s", sp);
            else std::snprintf(full, sizeof full, "%s%s", dir, sp);
            FILE* b = std::fopen(full, "rb");
            char magic[8] = {};
            if (b && std::fread(magic, 1, 8, b) == 8 && std::memcmp(magic, "HVPATCH1", 8) == 0) {
                /* a patch: built from the title's own DATA.CPK at its first read (or taken from an earlier build) */
                std::fclose(b);
                std::snprintf(w.patch, sizeof w.patch, "%s", full);
                char cache[1100];
                std::snprintf(cache, sizeof cache, "%s.built", full);
                size_t n = 0;
                uint8_t* d = read_file(cache, &n);
                if (d && n == (size_t)w.subst_len) w.subst = d;
                else std::free(d);
                w.colour = 2;
                s_swaps[s_nswaps++] = w;
                continue;
            }
            if (b) std::fseek(b, 0, SEEK_SET);
            if (b) {
                w.subst = (uint8_t*)std::malloc((size_t)w.subst_len);
                if (w.subst && std::fread(w.subst, 1, (size_t)w.subst_len, b) == (size_t)w.subst_len) { w.colour = 2; s_swaps[s_nswaps++] = w; }
                std::fclose(b);
            }
            if (!(s_nswaps && s_swaps[s_nswaps - 1].subst)) std::fprintf(stderr, "[mods] cannot read %s\n", sp);
            continue;
        }
        char o[64], r[64];
        unsigned toc = 0x800;
        /* "swapg": the choice is a byte of the title's own battle setup (global address), watched from the menu
         * on so the row already points at the mod when the model is loaded (that happens before the player
         * object -- and hv_chr_path_hook -- exists) */
        if ((std::sscanf(line, "swap %15s %x %u %lld %63s %63s %u", w.code, &w.off, &w.val, &w.rowpos, o, r, &toc) >= 6 ||
             std::sscanf(line, "swapg %15s %x %u %lld %63s %63s %u", w.code, &w.gaddr, &w.val, &w.rowpos, o, r, &toc) >= 6 ||
             (std::sscanf(line, "swapc %15s %u %lld %63s %63s %u", w.code, &w.val, &w.rowpos, o, r, &toc) >= 5 && (w.colour = 1))) &&
            hexbytes(o, w.orig) && hexbytes(r, w.repl)) {
            /* CriFs' own file-info entries hold the same row with the offset made absolute (+ the TOC offset) */
            std::memcpy(w.orig_abs, w.orig, 16); std::memcpy(w.repl_abs, w.repl, 16);
            uint8_t* rows[2] = { w.orig_abs, w.repl_abs };
            for (uint8_t* row : rows) {
                uint64_t v = 0;
                for (int k = 8; k < 16; k++) v = (v << 8) | row[k];
                v += toc;
                for (int k = 15; k >= 8; k--) { row[k] = (uint8_t)v; v >>= 8; }
            }
            s_swaps[s_nswaps++] = w;
        }
    }
    std::fclose(f);
    for (int i = 0; i < s_nswaps; i++) if (s_swaps[i].gaddr) { std::thread(watch_setup).detach(); break; }
    std::fprintf(stderr, "[mods] %d model swap(s) from %s\n", s_nswaps, path);
}

/* Every read into guest memory: the read that brings in a mod's index row tells where the title keeps it. */
uint64_t row_off(const uint8_t* r) { uint64_t v = 0; for (int k = 8; k < 16; k++) v = (v << 8) | r[k]; return v; }
uint32_t row_size(const uint8_t* r) { return ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3]; }

void fs_read_hook(int fd, long long pos, uint32_t buf, size_t n)
{
    (void)fd;
    load_mods();   /* the first game read: main() has set PS3_HDD0_ROOT by now */
    if (s_nswaps <= 0) return;
    for (int i = 0; i < s_nswaps; i++) {
        ChrSwap& w = s_swaps[i];
        if (!w.patch[0] || w.building || __atomic_load_n(&w.subst, __ATOMIC_ACQUIRE)) continue;
        char host[1100];
        const size_t hl = ppu_fs_fd_host_path(fd, host, sizeof host) ? std::strlen(host) : 0;
        if (hl < 8 || strcasecmp(host + hl - 8, "DATA.CPK") != 0) continue;
        w.building = 1;
        std::fprintf(stderr, "[mods] %s: building the mod from %s\n", w.code, w.patch);
        std::thread(build_patch_thread, i, std::string(host)).detach();
    }
    for (int i = 0; i < s_nswaps; i++) {
        const ChrSwap& w = s_swaps[i];
        const uint8_t* sub = __atomic_load_n(&w.subst, __ATOMIC_ACQUIRE);
        if (!sub || !w.on) continue;
        const long long a = pos > w.subst_at ? pos : w.subst_at;
        const long long e = (pos + (long long)n) < (w.subst_at + w.subst_len) ? (pos + (long long)n) : (w.subst_at + w.subst_len);
        if (a >= e) continue;
        std::memcpy(vm_base + buf + (a - pos), sub + (a - w.subst_at), (size_t)(e - a));
        if (s_chrlog) std::fprintf(stderr, "[mods] %s: served +%lld, %lld bytes from the mod\n", w.code, a - w.subst_at, e - a);
    }
    if (s_chrlog && (std::getenv("HV_READLOG") || pos >= 0x80000000ll || n >= (1u << 20)))
        std::fprintf(stderr, "[mods] read fd %d at 0x%llX, %zu bytes -> guest 0x%08X\n", fd, pos, n, buf);
    if (s_chrlog)   /* reads of a mod's entry, original or replacement data */
        for (int i = 0; i < s_nswaps; i++)
            for (int m = 0; m < 2; m++) {
                const uint8_t* r = m ? s_swaps[i].repl_abs : s_swaps[i].orig_abs;
                const long long o = (long long)row_off(r), e = o + row_size(r);
                if (pos < e && pos + (long long)n > o)
                    std::fprintf(stderr, "[mods] read %s %s data: +%lld, %zu bytes\n", s_swaps[i].code, m ? "MOD" : "original", pos - o, n);
            }
    for (int i = 0; i < s_nswaps; i++) {
        ChrSwap& w = s_swaps[i];
        if (w.colour || w.guest || w.rowpos < pos || w.rowpos + 16 > pos + (long long)n) continue;
        const uint32_t at = buf + (uint32_t)(w.rowpos - pos);
        if (std::memcmp(vm_base + at, w.orig, 16) == 0) {
            w.guest = at;
            std::fprintf(stderr, "[mods] %s: index row at guest 0x%08X\n", w.code, at);
        }
    }
}

#ifdef _WIN32
}
#include <windows.h>
namespace {
/* HV_MEMSNAP=<dir>: when <dir>/snap.req appears, dump the guest memory in use to <dir>/snap_<n>.bin (records:
 * u32 guest address, u32 length, bytes) and delete the request -- to find a variable by comparing snapshots. */
DWORD WINAPI memsnap_thread(LPVOID)
{
    const char* dir = std::getenv("HV_MEMSNAP");
    char req[600], out[600];
    std::snprintf(req, sizeof req, "%s/snap.req", dir);
    for (int n = 0;;) {
        Sleep(200);
        FILE* r = std::fopen(req, "rb");
        if (!r) continue;
        std::fclose(r);
        std::snprintf(out, sizeof out, "%s/snap_%d.bin", dir, n++);
        FILE* f = std::fopen(out, "wb");
        MEMORY_BASIC_INFORMATION mi;
        size_t total = 0;
        for (uint8_t* p = vm_base; f && p < vm_base + 0x100000000ull && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t*)mi.BaseAddress + mi.RegionSize) {
            if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
            const uint32_t g = (uint32_t)((uint8_t*)mi.BaseAddress - vm_base), len = (uint32_t)mi.RegionSize;
            std::fwrite(&g, 4, 1, f); std::fwrite(&len, 4, 1, f); std::fwrite(mi.BaseAddress, 1, len, f);
            total += len;
        }
        if (f) std::fclose(f);
        std::remove(req);
        std::fprintf(stderr, "[memsnap] %s: %zu MB\n", out, total >> 20);
    }
    return 0;
}
#endif
/* The "swapg" mods: point the index row at the mod while the watched setup byte has the mod's value. */
void watch_setup()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!vm_base) continue;
        for (int i = 0; i < s_nswaps; i++) {
            ChrSwap& w = s_swaps[i];
            if (!w.gaddr) continue;
            const int mod = vm_base[w.gaddr] == w.val;
            if (mod == w.on) continue;
            const int n = replace_rows(mod ? w.orig : w.repl, mod ? w.repl : w.orig) +
                          replace_rows(mod ? w.orig_abs : w.repl_abs, mod ? w.repl_abs : w.orig_abs);
            w.on = mod;
            std::fprintf(stderr, "[mods] %s: %s from the setup byte at 0x%08X (%d copies of the index row)\n",
                         w.code, mod ? "mod" : "original", w.gaddr, n);
        }
    }
}
struct Init { Init() {
    g_hv_fs_read_hook = fs_read_hook;   /* the mods file is read on the first game read (load_mods) */
#ifdef _WIN32
    if (std::getenv("HV_MEMSNAP")) CreateThread(nullptr, 0, memsnap_thread, nullptr, 0, nullptr);
#endif
} } s_init;
}

#ifdef _WIN32
#include <windows.h>
/* HV_MODSCAN=<FileSize hex>,<ExtractSize hex>: where else the title keeps an index row (CriFs' own tables). */
static void mods_scan()
{
    static int done = 0;
    const char* e = std::getenv("HV_MODSCAN");
    if (done || !e) return;
    done = 1;
    unsigned a = 0, b = 0;
    if (std::sscanf(e, "%x,%x", &a, &b) != 2) return;
    uint8_t pa[4] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a };
    uint8_t pb[4] = { (uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b };
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t* p = vm_base; p < vm_base + 0x100000000ull && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t*)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT) continue;
        uint8_t* r = (uint8_t*)mi.BaseAddress; size_t n = mi.RegionSize;
        for (size_t i = 0; i + 4 <= n; i++) {
            if (std::memcmp(r + i, pa, 4)) continue;
            for (int d = -48; d <= 48; d++) {
                if ((long long)i + d < 0 || i + d + 4 > n || std::memcmp(r + i + d, pb, 4)) continue;
                const uint32_t g = (uint32_t)(r + i - vm_base);
                std::fprintf(stderr, "[modscan] FileSize at 0x%08X, ExtractSize at %+d:", g, d);
                for (int k = -16; k < 32; k++) std::fprintf(stderr, " %02X", r[i + k]);
                std::fprintf(stderr, "\n");
            }
        }
    }
    std::fprintf(stderr, "[modscan] done\n");
}
#else
static void mods_scan() {}
#endif

#ifndef _WIN32
#include <sys/mman.h>
#endif
/* Replace every copy of a 16-byte row in the guest memory actually in use (the raw index and CriFs' tables). */
static int replace_rows(const uint8_t* from, const uint8_t* to)
{
    int n = 0;
    auto scan = [&](uint8_t* r, size_t len) {
        for (size_t i = 0; i + 16 <= len; i++)
            if (r[i] == from[0] && std::memcmp(r + i, from, 16) == 0) { std::memcpy(r + i, to, 16); n++; i += 15; }
    };
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t* p = vm_base; p < vm_base + 0x100000000ull && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t*)mi.BaseAddress + mi.RegionSize)
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            scan((uint8_t*)mi.BaseAddress, mi.RegionSize);
#else
    /* the guest space is one mapping; only the pages the title touched are resident: walk them with mincore */
    const size_t pg = 4096;
    static unsigned char* vec = nullptr;
    const size_t span = 0x100000000ull;
    if (!vec) vec = (unsigned char*)std::malloc(span / pg);
    if (vec && mincore(vm_base, span, vec) == 0)
        for (size_t i = 0; i < span / pg; i++) if (vec[i] & 1) scan(vm_base + i * pg, pg);
#endif
    return n;
}

/* Guest addresses of a byte pattern in the guest memory in use (at most cap). */
static int guest_find(const uint8_t* pat, size_t len, uint32_t* out, int cap)
{
    int n = 0;
    auto scan = [&](uint8_t* r, size_t sz) {
        for (size_t i = 0; i + len <= sz && n < cap; i++)
            if (r[i] == pat[0] && std::memcmp(r + i, pat, len) == 0) out[n++] = (uint32_t)(r + i - vm_base);
    };
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t* p = vm_base; p < vm_base + 0x100000000ull && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t*)mi.BaseAddress + mi.RegionSize)
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            scan((uint8_t*)mi.BaseAddress, mi.RegionSize);
#else
    const size_t pg = 4096;
    static unsigned char* vec = nullptr;
    const size_t span = 0x100000000ull;
    if (!vec) vec = (unsigned char*)std::malloc(span / pg);
    /* a run of resident pages is scanned as one, so a pattern across a page boundary is found too */
    if (vec && mincore(vm_base, span, vec) == 0)
        for (size_t i = 0; i < span / pg; ) {
            if (!(vec[i] & 1)) { i++; continue; }
            size_t j = i;
            while (j < span / pg && (vec[j] & 1)) j++;
            scan(vm_base + i * pg, (j - i) * pg);
            i = j;
        }
#endif
    return n;
}

/* The character menu's colour selector (func_002865F0 draws "%02d/%02d" from the setting object's +0x1C current
 * index and +0x20 highest index). HV_CHRLOG=1 logs the object whenever it changes. */
extern "C" void hv_color_hook(uint32_t setting, uint32_t widget)
{
    load_mods();
    /* "swapc <code> <colour index>": the selector gets one more colour (highest index +0x20 raised to it), and
     * while it shows that colour the code's index row points at the mod -- the menu runs before the battle
     * loads its models, so the model is read from the mod's data. */
    const uint8_t* sl = vm_base + widget + 0x8C;
    const int slot = (int)(((uint32_t)sl[0] << 24) | (sl[1] << 16) | (sl[2] << 8) | sl[3]);
    for (int i = 0; i < s_nswaps; i++) {
        ChrSwap& w = s_swaps[i];
        if (!w.colour || w.colour == 3) continue;   /* weapons: hv_weapon_hook */
        if (w.colour == 2 && !__atomic_load_n(&w.subst, __ATOMIC_ACQUIRE)) continue;   /* a patch still being built */
        if (w.colour == 2 && slot != w.slot && w.guest != setting) continue;   /* another character's selector */
        for (uint32_t f : { 0x20u, 0x28u }) {   /* +0x20 the "/NN" shown, +0x28 the bound the arrows wrap at */
            uint8_t* hi = vm_base + setting + f;
            const uint32_t cur_hi = ((uint32_t)hi[0] << 24) | (hi[1] << 16) | (hi[2] << 8) | hi[3];
            if (cur_hi < w.val) { hi[0] = hi[1] = hi[2] = 0; hi[3] = (uint8_t)w.val; }
        }
        const uint8_t* c = vm_base + setting + 0x1C;
        const int shows = (((uint32_t)c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]) == w.val;
        /* each player has its own selector object: only the one that picked the colour can take it back */
        if (!shows && w.guest != setting) continue;
        const int mod = shows;
        if (mod) w.guest = setting;
        if (mod != w.on) {
            const int n = w.colour == 2 ? 0 : replace_rows(mod ? w.orig : w.repl, mod ? w.repl : w.orig) +
                          replace_rows(mod ? w.orig_abs : w.repl_abs, mod ? w.repl_abs : w.orig_abs);
            w.on = mod;
            std::fprintf(stderr, "[mods] %s: %s (colour %u in the selector, %d copies of the index row)\n", w.code, mod ? "mod" : "original", w.val + 1, n);
        }
    }
    static uint8_t last[0x40];
    if (s_chrlog && std::memcmp(last, vm_base + setting, 0x40)) {
        std::memcpy(last, vm_base + setting, 0x40);
        std::fprintf(stderr, "[color] setting 0x%08X widget 0x%08X:", setting, widget);
        for (int k = 0; k < 0x40; k++) std::fprintf(stderr, "%s%02X", k % 4 ? "" : " ", vm_base[setting + k]);
        std::fprintf(stderr, "\n");
        const uint32_t up = (vm_base[setting + 4] << 24) | (vm_base[setting + 5] << 16) | (vm_base[setting + 6] << 8) | vm_base[setting + 7];
        std::fprintf(stderr, "[color]   widget:");
        for (int k = 0; k < 0x200; k += 4) std::fprintf(stderr, " %08X", (vm_base[widget + k] << 24) | (vm_base[widget + k + 1] << 16) | (vm_base[widget + k + 2] << 8) | vm_base[widget + k + 3]);
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[color]   +4 -> 0x%08X:", up);
        for (int k = 0; k < 0x80; k++) std::fprintf(stderr, "%s%02X", k % 4 ? "" : " ", vm_base[up + k]);
        std::fprintf(stderr, "\n");
    }
}

/* The weapon selector of PC Costume Settings (func_0028F8E8). Its setting object (widget +0x54): +0x1C the weapon
 * chosen, as its number in the title's list of 40 (weapon_00..39: twin blades 0-11, wands 12-18, guns 19-25, swords
 * 26-32, gloves 33-39); +0x20 the highest number; +0x40 a vector (+0x44 begin, +0x48 end) of the numbers that are
 * locked or of another type, which func_0024914C skips. The extra weapons of "weapon" lines take no number: they are
 * steps of the cursor while it stays on their carrier -- moving on from the carrier is put back onto it until the
 * extra weapons are gone through, and the counter the selector draws counts them (hv_weapon_numbers). */
namespace {
uint32_t rd32(uint32_t a) { const uint8_t* p = vm_base + a; return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void wr32(uint32_t a, uint32_t v) { uint8_t* p = vm_base + a; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
int weapon_group(int n) { static const int lo[] = { 0, 12, 19, 26, 33, 40 }; for (int g = 0; g < 5; g++) if (n >= lo[g] && n < lo[g + 1]) return g; return -1; }
struct WeaponSel { uint32_t setting; int prev, step; };   /* step: 0 the carrier itself, k the k-th extra weapon */
WeaponSel s_wsel[4];

int weapon_carrier(int* count)
{
    int c = -1, k = 0;
    for (int i = 0; i < s_nswaps; i++)
        if (s_swaps[i].colour == 3 && (c < 0 || s_swaps[i].slot == c)) { c = s_swaps[i].slot; k++; }
    *count = k;
    return c;
}

int weapon_locked(uint32_t setting, int n)
{
    for (uint32_t p = rd32(setting + 0x44), e = rd32(setting + 0x48); p < e; p += 4) if ((int)rd32(p) == n) return 1;
    return 0;
}

/* a weapon selector seen by hv_weapon_hook: its setting object, or the list its cursor moves over (func_002820AC
 * asks with the setting's parent + 0x74; the parent is at setting +0x04) */
WeaponSel* weapon_known(uint32_t obj)
{
    for (WeaponSel& s : s_wsel) if (s.setting && (s.setting == obj || rd32(s.setting + 4) + 0x74 == obj)) return &s;
    return nullptr;
}

/* the selector's weapons in order (not locked, 0..highest; the carrier always in) */
int weapon_list(uint32_t setting, int carrier, int* out, int cap)
{
    int n = 0;
    const int hi = (int)rd32(setting + 0x20);
    for (int i = 0; i <= hi && n < cap; i++) if (i == carrier || !weapon_locked(setting, i)) out[n++] = i;
    return n;
}

uint32_t s_wshow_owner;   /* the selector whose cursor moved last: the shared picture and name follow it */

/* The picture (in the weapon pictures atlas) and the name (in the title's text) the selector shows for the carrier:
 * an extra weapon's own while one is chosen (step k), the carrier's again at step 0. Both are found in the guest
 * memory the first time ("weaponicons" line) and the originals kept. */
void weapon_show(int step)
{
    static int ncell, nname, shown;
    static uint32_t cell[4], name[8];
    static int cap[8];
    static uint8_t orig_cell[32 * 1024];
    if (s_icon_rel < 0 || step == shown) return;
    if (!ncell) {
        uint32_t m[4];
        const int k = guest_find(s_icon_sig, 64, m, 4);
        for (int i = 0; i < k; i++) cell[ncell++] = m[i] - (uint32_t)s_icon_rel;
        if (!ncell) { if (s_chrlog) std::fprintf(stderr, "[weapon] the weapon pictures are not in memory yet\n"); return; }
        for (int r = 0; r < 32; r++) std::memcpy(orig_cell + r * 1024, vm_base + cell[0] + r * 4096, 1024);
        const size_t L = std::strlen(s_icon_name);
        uint8_t pat[64];
        std::memcpy(pat, s_icon_name, L); pat[L] = 0;
        uint32_t nm[8];
        const int q = guest_find(pat, L + 1, nm, 8);
        for (int i = 0; i < q; i++) {   /* room for a longer name: the zeros after it */
            int z = 0;
            while (z < 32 && vm_base[nm[i] + L + z] == 0) z++;
            name[nname] = nm[i]; cap[nname++] = (int)L + z - 1;
        }
        if (s_chrlog) std::fprintf(stderr, "[weapon] carrier picture found %d time(s) (0x%08X), its name %d time(s)\n", ncell, cell[0], nname);
    }
    const uint8_t* pic = orig_cell;
    const char* text = s_icon_name;
    for (int i = 0; step > 0 && i < s_nswaps; i++) {
        ChrSwap& w = s_swaps[i];
        if (w.colour != 3 || (int)w.val != step) continue;
        if (!w.icon && !w.icon_tried) {   /* <file>.icon next to <file>.hvpatch */
            w.icon_tried = 1;
            char p[1100];
            std::snprintf(p, sizeof p, "%s", w.patch);
            char* dot = std::strrchr(p, '.');
            if (dot) std::strcpy(dot, ".icon");
            size_t n = 0;
            uint8_t* d = read_file(p, &n);
            if (d && n == sizeof orig_cell) w.icon = d; else std::free(d);
        }
        if (w.icon) pic = w.icon;
        if (w.name[0]) text = w.name;
    }
    for (int c = 0; c < ncell; c++)
        for (int r = 0; r < 32; r++) std::memcpy(vm_base + cell[c] + r * 4096, pic + r * 1024, 1024);
    for (int i = 0; i < nname; i++) {
        const int len = (int)std::strlen(text), n = len < cap[i] ? len : cap[i];
        std::memset(vm_base + name[i], 0, (size_t)cap[i] + 1);
        std::memcpy(vm_base + name[i], text, (size_t)n);
    }
    shown = step;
}

WeaponSel* weapon_sel(uint32_t setting)
{
    for (WeaponSel& s : s_wsel) if (s.setting == setting) return &s;
    for (WeaponSel& s : s_wsel) if (!s.setting) { s = { setting, -1, 0 }; return &s; }
    s_wsel[0] = { setting, -1, 0 };
    return &s_wsel[0];
}
}

/* a weapon selector's setting object, for a fighter of the carrier's type (some other weapon of that type is in its
 * list): from then on hv_weapon_available keeps the carrier in the list */
WeaponSel* weapon_register(uint32_t setting, int c)
{
    WeaponSel* s = weapon_known(setting);
    if (!s) {
        const int g = weapon_group(c);
        int same = 0;
        for (int i = 0; i < 40; i++) if (i != c && weapon_group(i) == g && !weapon_locked(setting, i)) same = 1;
        if (!same) return nullptr;
        s = weapon_sel(setting);
    }
    /* the cursor (func_00249FC4) wraps past +0x28, the highest weapon in the list when the menu opened */
    if ((int)rd32(setting + 0x28) < c) {
        wr32(setting + 0x28, (uint32_t)c);
        if (s_chrlog) std::fprintf(stderr, "[weapon] setting 0x%08X: the cursor's bound raised to %d\n", setting, c);
    }
    /* ... and skips the numbers in the locked vector itself: the carrier leaves it */
    const uint32_t b = rd32(setting + 0x44), e = rd32(setting + 0x48);
    for (uint32_t p = b; p < e; p += 4)
        if ((int)rd32(p) == c) {
            wr32(p, rd32(e - 4)); wr32(setting + 0x48, e - 4);
            if (s_chrlog) std::fprintf(stderr, "[weapon] setting 0x%08X: weapon %d taken out of the locked list\n", setting, c);
            break;
        }
    return s;
}

extern "C" void hv_weapon_hook(uint32_t setting, uint32_t widget)
{
    (void)widget;
    load_mods();
    int k = 0;
    const int c = weapon_carrier(&k);
    if (c < 0 || !setting) return;
    WeaponSel* s = weapon_register(setting, c);
    if (!s) return;
    int list[64];
    const int n = weapon_list(setting, c, list, 64);
    int at = -1;
    for (int i = 0; i < n; i++) if (list[i] == c) at = i;
    if (at < 0 || n < 2) return;
    const int cur = (int)rd32(setting + 0x1C);
    if (cur != s->prev) {
        const int after = list[(at + 1) % n], before = list[(at + n - 1) % n];
        if (s->prev == c && cur == after && s->step < k) { s->step++; wr32(setting + 0x1C, (uint32_t)c); }
        else if (s->prev == c && cur == before && s->step > 0) { s->step--; wr32(setting + 0x1C, (uint32_t)c); }
        else if (cur == c) s->step = s->prev == after ? k : 0;   /* came back from the weapon after the extra ones */
        else s->step = 0;
        s->prev = (int)rd32(setting + 0x1C);
        s_wshow_owner = setting;
        if (s_chrlog) std::fprintf(stderr, "[weapon] setting 0x%08X: weapon %d, extra %d of %d\n", setting, s->prev, s->prev == c ? s->step : 0, k);
    }
    if (s_wshow_owner == setting) weapon_show(s->prev == c ? s->step : 0);
    /* the carrier's file is served from the extra weapon a player has on (the last one chosen wins if both differ) */
    for (int i = 0; i < s_nswaps; i++) {
        ChrSwap& w = s_swaps[i];
        if (w.colour != 3) continue;
        int on = 0;
        for (const WeaponSel& q : s_wsel) if (q.setting && q.prev == c && q.step == (int)w.val) on = 1;
        if (on != w.on) {
            w.on = on;
            if (s_chrlog) std::fprintf(stderr, "[weapon] %s (%s): %s\n", w.code, w.name, on ? "on" : "off");
        }
    }
}

/* func_0024914C(setting, n): is weapon n in the selector's list. For a weapon selector hv_weapon_hook has seen, the
 * carrier of the extra weapons is (it may still be locked in the save). -1: the title's own answer. */
extern "C" int hv_weapon_available(uint32_t setting, int n, uint32_t caller)
{
    if (s_nswaps <= 0) return -1;
    int k = 0;
    const int c = weapon_carrier(&k);
    if (c < 0 || n != c) return -1;
    /* the weapon selector's own questions (counting its weapons, moving the cursor, drawing): func_0028F64C,
     * func_00290xxx and func_0028F8E8 ask with its setting object -- known from the first one */
    if (caller == 0x0028F698u || caller == 0x002900F4u || caller == 0x002903F8u || caller == 0x0028F978u)
        return weapon_register(setting, c) ? 1 : -1;
    return weapon_known(setting) ? 1 : -1;
}

/* the "NN/NN" the selector draws: position and count with the extra weapons in */
extern "C" void hv_weapon_numbers(uint32_t setting, uint64_t* pos, uint64_t* count)
{
    int k = 0;
    const int c = weapon_carrier(&k);
    if (c < 0 || !setting) return;
    WeaponSel* s = weapon_known(setting);
    if (!s) return;
    int list[64];
    const int n = weapon_list(setting, c, list, 64);
    int at = -1, now = -1;
    const int cur = (int)rd32(setting + 0x1C);
    for (int i = 0; i < n; i++) { if (list[i] == c) at = i; if (list[i] == cur) now = i; }
    if (at < 0 || now < 0) return;
    *count = (uint64_t)(n + k);
    *pos = (uint64_t)(now + 1 + (now > at ? k : now == at ? s->step : 0));
}

extern "C" void hv_chr_path_hook(uint32_t buf, uint32_t player)
{
    load_mods();
    mods_scan();
    const char* s = (const char*)(vm_base + buf);
    if (s_chrlog)
        std::fprintf(stderr, "[chr] %s player=0x%08X +924=%u +925=%u\n", s, player,
                     vm_base[player + 0x924], vm_base[player + 0x925]);
    const char* at = std::strstr(s, "data/chr/");
    if (!at) return;
    at += 9;
    /* A "subst" colour has no tint of its own (the title's tables stop at its last colour: grey): the mod brings its
     * own textures, so the fighter takes colour 1's tint. */
    for (int i = 0; i < s_nswaps; i++) {
        const ChrSwap& w = s_swaps[i];
        if (w.colour == 2 && std::strncmp(at, w.code, std::strlen(w.code)) == 0 && vm_base[player + 0x924] == w.val) {
            vm_base[player + 0x924] = 0;
            if (s_chrlog) std::fprintf(stderr, "[chr]   %s: colour %u -> tint of colour 1\n", w.code, w.val + 1);
        }
    }
    for (int i = 0; i < s_nswaps; i++) {
        ChrSwap& w = s_swaps[i];
        if (w.gaddr || w.colour || std::strncmp(at, w.code, std::strlen(w.code)) != 0) continue;
        const int mod = vm_base[player + w.off] == w.val;
        int n = 0;
        if (mod != w.on) {
            n = replace_rows(mod ? w.orig : w.repl, mod ? w.repl : w.orig) + replace_rows(mod ? w.orig_abs : w.repl_abs, mod ? w.repl_abs : w.orig_abs);
            w.on = mod;
        }
        if (s_chrlog) std::fprintf(stderr, "[chr]   %s: %s (%d copies of the index row changed)\n", w.code, mod ? "mod" : "original", n);
    }
}

/* ---------------------------------------------------------------------------
 * Android on-screen pad (APK build)
 *
 * PadView (Java) draws the PS3 controls over the game and reports the pressed set here. It feeds an SDL virtual
 * joystick typed as a game controller, so cellPad's SDL2 backend opens it like a real pad and the title reads it
 * through the ordinary cellPadGetData path. A Bluetooth gamepad keeps working next to it.
 * -----------------------------------------------------------------------*/
#ifdef HV_APK
#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <SDL2/SDL.h>

static SDL_Joystick* s_vpad;          /* no longer opened: the touches go straight into cellPad's port 0 */
static volatile int s_vpad_touched;   /* the player pressed something: the intro skipper stops */
/* What the on-screen pad holds down, as SDL_GameControllerButton bits (16/17 = L2/R2); cellPad ORs it into
 * port 0 with a real gamepad's state, so the touch pad is always player 1 and never takes a port of its own. */
volatile unsigned g_hv_touch_buttons;
volatile int      g_hv_touch_present;

/* Intro skipper (HV_AUTOSKIP=0 turns it off): the title opens on its
 * Stereoscopic settings screen after six skippable screens (the fan patch's
 * note, three logos, a disclaimer). Press X on the virtual pad until the
 * Vulkan backend sees that screen in a presented frame (hv_autoskip_done),
 * then stop: one X more there would open the setting. Gives up after 90 s. */
extern "C" volatile int hv_autoskip_watch;
extern "C" volatile int hv_autoskip_done;

static void* hv_autoskip(void*)
{
    hv_autoskip_watch = 1;
    int presses = 0;
    for (int t = 0; t < 900 && !hv_autoskip_done && !s_vpad_touched; t++) {
        usleep(100000);
        if (t < 40 || t % 15) continue;            /* 4 s of boot, then one press every 1.5 s */
        if (hv_autoskip_done || s_vpad_touched) break;
        g_hv_touch_buttons |= 1u << SDL_CONTROLLER_BUTTON_A;
        usleep(150000);
        g_hv_touch_buttons &= ~(1u << SDL_CONTROLLER_BUTTON_A);
        presses++;
    }
    hv_autoskip_watch = 0;
    std::fprintf(stderr, "[vpad] intro skip: %s after %d presses\n",
                 hv_autoskip_done ? "settings screen reached" : s_vpad_touched ? "player took over" : "gave up",
                 presses);
    return nullptr;
}

/* Called once from main.cpp's apk_prepare, on the SDL main thread, before the title runs. */
extern "C" void hv_vpad_init(void)
{
    /* A Bluetooth DualShock 4 ("Wireless Controller") was opened by SDL's HIDAPI driver as "PS4 Controller" and
     * then never reported a button: on Android that driver only reads USB and BLE devices. Without it SDL takes the
     * pad through Android's own input path, with the standard DS4 mapping. HV_HIDAPI=1 keeps the driver. */
    if (!(std::getenv("HV_HIDAPI") && std::getenv("HV_HIDAPI")[0] == '1')) SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[vpad] SDL joystick init failed: %s\n", SDL_GetError());
        return;
    }
    s_vpad = nullptr;
    g_hv_touch_present = 1;
    std::fprintf(stderr, "[vpad] on-screen pad feeds port 0 directly\n");
    const char* skip = std::getenv("HV_AUTOSKIP");
    if (skip && skip[0] == '1') {
        pthread_t th;
        if (pthread_create(&th, nullptr, hv_autoskip, nullptr) == 0) pthread_detach(th);
    }
}

/* PadView.nativePad(int buttons): bit n = SDL_GameControllerButton n; bits 16/17 = L2/R2 (trigger axes). */
extern "C" JNIEXPORT void JNICALL Java_com_hackversus_port_PadView_nativePad(JNIEnv*, jclass, jint buttons)
{
    static int logged = 0;
    if (logged < 12) { logged++; std::fprintf(stderr, "[vpad] buttons 0x%05X%s\n", (unsigned)buttons, s_vpad ? "" : " (no pad)"); }
    if (buttons) s_vpad_touched = 1;
    g_hv_touch_buttons = (unsigned)buttons;
}
#endif

/* ---------------------------------------------------------------------------
 * HV_PROF=<seconds>: a sampling profiler for a device with no simpleperf or
 * debuggerd access. A host thread sends SIGPROF to every thread of the process
 * once a millisecond; the handler records the interrupted PC. When the time is
 * up the hottest addresses go to stderr as "<module> +<offset> <samples>",
 * which llvm-symbolizer resolves against the unstripped library.
 * -----------------------------------------------------------------------*/
#if !defined(_WIN32)
#include <atomic>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

static const int kProfMax = 1 << 20;
static uintptr_t* s_prof_pc;
static std::atomic<int> s_prof_n{0};

/* libc's text range: a sample inside it is charged to the first caller outside, found through the frame-pointer
 * chain (x29) -- "something is in futex/nanosleep" says nothing, who called it does. */
static uintptr_t s_libc_lo, s_libc_hi;

static void on_sigprof(int, siginfo_t*, void* uc)
{
    const mcontext_t& m = ((ucontext_t*)uc)->uc_mcontext;
    uintptr_t pc = (uintptr_t)m.pc;
    if (pc >= s_libc_lo && pc < s_libc_hi) {
        uintptr_t lr = (uintptr_t)m.regs[30], fp = (uintptr_t)m.regs[29];
        pc = lr & 0x7FFFFFFFFFull;
        for (int k = 0; k < 8 && pc >= s_libc_lo && pc < s_libc_hi && fp && !(fp & 7); k++) {
            pc = ((uintptr_t*)fp)[1] & 0x7FFFFFFFFFull;   /* strip the PAC signature */
            fp = ((uintptr_t*)fp)[0];
        }
    }
    /* HV_PROF_UP=<n>: charge the sample n frames up instead -- who called the lock wait. */
    static int up = -1;
    if (up < 0) { const char* e = getenv("HV_PROF_UP"); up = e ? atoi(e) : 0; }
    if (up > 0) {
        uintptr_t fp = (uintptr_t)m.regs[29];
        for (int k = 0; k < up && fp && !(fp & 7); k++) {
            pc = ((uintptr_t*)fp)[1] & 0x7FFFFFFFFFull;
            fp = ((uintptr_t*)fp)[0];
        }
    }
    const int i = s_prof_n.fetch_add(1, std::memory_order_relaxed);
    /* The thread in the top bits (a PC without its PAC signature fits in 39): the report is per thread. */
    if (i < kProfMax) s_prof_pc[i] = (pc & 0x7FFFFFFFFFull) | ((uintptr_t)syscall(SYS_gettid) << 40);
}

static void* prof_thread(void* arg)
{
    const long packed = (long)(intptr_t)arg, secs = packed & 0xFFFF;   /* HV_PROF=[delay:]seconds */
    sleep((unsigned)(packed >> 16));
    const pid_t self = (pid_t)syscall(SYS_gettid), pid = getpid();
    for (long ms = 0; ms < secs * 1000; ms++) {
        if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
                const int tid = atoi(e->d_name);
                if (tid <= 0 || tid == self) continue;
                /* Only threads on a CPU: the parked ones would fill the buffer with futex waits. */
                char path[64], st[256] = {0};
                snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
                if (FILE* f = fopen(path, "r")) { fread(st, 1, sizeof st - 1, f); fclose(f); }
                const char* rp = strrchr(st, ')');
                if (rp && rp[1] == ' ' && rp[2] == 'R') syscall(SYS_tgkill, pid, tid, SIGPROF);
            }
            closedir(d);
        }
        usleep(1000);
    }
    const int n = std::min(s_prof_n.load(), kProfMax);
    std::vector<uintptr_t> v(s_prof_pc, s_prof_pc + n);
    std::sort(v.begin(), v.end());
    std::vector<std::pair<int, uintptr_t>> hot;
    for (size_t i = 0; i < v.size();) {
        size_t j = i; while (j < v.size() && v[j] == v[i]) j++;
        hot.push_back({(int)(j - i), v[i]}); i = j;
    }
    std::sort(hot.rbegin(), hot.rend());
    std::fprintf(stderr, "[prof] %d samples over %lds\n", n, secs);
    /* Per thread: its total, then its hottest addresses, for the busiest eight threads. */
    std::vector<std::pair<int, long>> tids;
    for (auto& h : hot) {
        const long tid = (long)(h.second >> 40);
        auto it = std::find_if(tids.begin(), tids.end(),
                               [&](const std::pair<int, long>& t) { return t.second == tid; });
        if (it == tids.end()) tids.push_back({h.first, tid}); else it->first += h.first;
    }
    std::sort(tids.rbegin(), tids.rend());
    for (size_t t = 0; t < tids.size() && t < 12; t++) {
        std::fprintf(stderr, "[prof] thread %ld: %d samples\n", tids[t].second, tids[t].first);
        int shown = 0;
        for (size_t k = 0; k < hot.size() && shown < 400; k++) {
            if ((long)(hot[k].second >> 40) != tids[t].second) continue;
            shown++;
            const uintptr_t pc = hot[k].second & 0xFFFFFFFFFFull;
            Dl_info di;
            if (dladdr((void*)pc, &di) && di.dli_fname)
                std::fprintf(stderr, "[prof] %s +0x%lx %d\n", di.dli_fname,
                             (unsigned long)(pc - (uintptr_t)di.dli_fbase), hot[k].first);
            else
                std::fprintf(stderr, "[prof] ? 0x%lx %d\n", (unsigned long)pc, hot[k].first);
        }
    }
    std::fflush(stderr);
    return nullptr;
}

/* HV_STACKS_EVERY=<seconds>: every guest thread's stack to the log on a timer -- the app's process cannot be
 * sent SIGUSR2 from adb, so this is how a stall inside the APK gets located. */
static void* stacks_thread(void* arg)
{
    const unsigned secs = (unsigned)(uintptr_t)arg;
    for (unsigned n = 0; n < 6; n++) {
        sleep(secs);
        std::fprintf(stderr, "[stacks] dump %u\n", n);
        ppu_dump_all_guest_stacks("STACKS");
    }
    return nullptr;
}

/* HV_UCLAMP=<0..1024>: utilization clamp (sched_setattr SCHED_FLAG_UTIL_CLAMP_MIN) on
 * every thread of the process, re-applied every 2 s for threads created since.
 * In a fight no thread is ever 100 % busy -- each one waits on another -- so the
 * walt governor settles at 1.2-1.6 GHz on cores that can do 2.5-3.0; a clamp
 * tells it the threads want speed regardless of their measured utilization. */
#include <cerrno>
struct hv_sched_attr {
    uint32_t size; uint32_t sched_policy; uint64_t sched_flags; int32_t sched_nice;
    uint32_t sched_priority; uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};
static void* hv_uclamp_thread(void* arg)
{
    const unsigned clamp = (unsigned)(uintptr_t)arg;
    int applied = 0, failed = 0;
    for (int round = 0;; round++) {
        if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
                const int tid = atoi(e->d_name);
                if (tid <= 0) continue;
                hv_sched_attr a; memset(&a, 0, sizeof a);
                a.size = sizeof a;
                a.sched_flags = 0x08 | 0x10 | 0x20 | 0x40;   /* KEEP_POLICY | KEEP_PARAMS | UTIL_CLAMP_MIN | UTIL_CLAMP_MAX */
                a.sched_util_min = clamp; a.sched_util_max = 1024;
                if (syscall(274 /* sched_setattr */, tid, &a, 0) == 0) applied++; else failed++;
            }
            closedir(d);
        }
        if (round == 0 || round % 30 == 0)
            std::fprintf(stderr, "[uclamp] min=%u: %d threads set, %d failed (errno %d)\n", clamp, applied, failed, errno);
        applied = failed = 0;
        sleep(2);
    }
    return nullptr;
}

/* HV_ADPF=1: an Android performance-hint session (ADPF, libandroid.so API 33) over
 * the process's eight busiest threads, re-chosen every 5 s, with a 33.3 ms target
 * and the real frame time reported from the Vulkan present (hv_adpf_frame). It is
 * the sanctioned way for an app to ask for CPU speed; sched_setattr's uclamp is
 * EPERM for an app on this kernel. */
#include <dlfcn.h>
typedef void* (*hv_fn_get_manager)(void);
typedef void* (*hv_fn_create_session)(void*, const int32_t*, size_t, int64_t);
typedef int   (*hv_fn_report)(void*, int64_t);
typedef void  (*hv_fn_close)(void*);
typedef int   (*hv_fn_set_threads)(void*, const int32_t*, size_t);
static void*        s_adpf_session;
static hv_fn_report s_adpf_report;
static int64_t      s_adpf_last_ns;
static int64_t hv_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec; }
extern "C" void hv_adpf_frame(void)
{
    if (!s_adpf_session || !s_adpf_report) return;
    const int64_t now = hv_now_ns();
    if (s_adpf_last_ns) s_adpf_report(s_adpf_session, now - s_adpf_last_ns);
    s_adpf_last_ns = now;
}
static void* hv_adpf_thread(void* arg)
{
    const int64_t target_ns = (int64_t)(intptr_t)arg;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    hv_fn_get_manager get_manager = lib ? (hv_fn_get_manager)dlsym(lib, "APerformanceHint_getManager") : nullptr;
    hv_fn_create_session create = lib ? (hv_fn_create_session)dlsym(lib, "APerformanceHint_createSession") : nullptr;
    hv_fn_report report = lib ? (hv_fn_report)dlsym(lib, "APerformanceHint_reportActualWorkDuration") : nullptr;
    hv_fn_close close_s = lib ? (hv_fn_close)dlsym(lib, "APerformanceHint_closeSession") : nullptr;
    hv_fn_set_threads set_threads = lib ? (hv_fn_set_threads)dlsym(lib, "APerformanceHint_setThreads") : nullptr;
    void* mgr = get_manager ? get_manager() : nullptr;
    if (!mgr || !create || !report) { std::fprintf(stderr, "[adpf] unavailable (manager %p create %p report %p)\n", mgr, (void*)create, (void*)report); return nullptr; }
    std::fprintf(stderr, "[adpf] manager ready, target %.1f ms, setThreads %s\n", target_ns / 1e6, set_threads ? "yes" : "no");
    static long long prev[4096]; int32_t chosen[8]; int n_prev_chosen = 0; int32_t prev_chosen[8] = {0};
    for (int round = 0;; round++) {
        /* the busiest threads since the last round, by utime+stime delta */
        struct { int tid; long long d; } all[256], top[8]; int n_all = 0, n = 0;
        if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
                const int tid = atoi(e->d_name); if (tid <= 0) continue;
                char path[64], st[512] = {0}; snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
                FILE* f = fopen(path, "r"); if (!f) continue; fread(st, 1, sizeof st - 1, f); fclose(f);
                const char* rp = strrchr(st, ')'); if (!rp) continue;
                long long u = 0, s = 0; int k = 0; const char* p = rp + 2;   /* field 3 onwards */
                for (; *p && k < 11; p++) if (*p == ' ') k++;                /* skip to field 14 (utime) */
                sscanf(p, "%lld %lld", &u, &s);
                const long long now = u + s, delta = now - prev[tid & 4095]; prev[tid & 4095] = now;
                if (round == 0 || delta <= 0 || n_all >= 256) continue;
                all[n_all].tid = tid; all[n_all].d = delta; n_all++;
            }
            closedir(d);
        }
        for (int i = 1; i < n_all; i++)   /* insertion sort, busiest first */
            for (int j = i; j > 0 && all[j].d > all[j - 1].d; j--) { auto t = all[j]; all[j] = all[j - 1]; all[j - 1] = t; }
        for (n = 0; n < 8 && n < n_all; n++) top[n] = all[n];
        if (n) {
            for (int i = 0; i < n; i++) chosen[i] = top[i].tid;
            int same = (n == n_prev_chosen); for (int i = 0; same && i < n; i++) same = chosen[i] == prev_chosen[i];
            if (!same) {
                if (s_adpf_session && set_threads) { set_threads(s_adpf_session, chosen, (size_t)n); }
                else {
                    void* old = s_adpf_session; s_adpf_session = nullptr;
                    if (old && close_s) close_s(old);
                    s_adpf_session = create(mgr, chosen, (size_t)n, target_ns); s_adpf_report = report; s_adpf_last_ns = 0;
                }
                std::fprintf(stderr, "[adpf] session %p over %d threads:", s_adpf_session, n);
                for (int i = 0; i < n; i++) std::fprintf(stderr, " %d(%lld)", chosen[i], top[i].d);
                std::fprintf(stderr, "\n");
                memcpy(prev_chosen, chosen, sizeof chosen); n_prev_chosen = n;
            }
        }
        sleep(round == 0 ? 2 : 5);
    }
    return nullptr;
}

extern "C" void hv_prof_start(void)
{
    if (const char* ad = getenv("HV_ADPF")) {
        pthread_t t;
        const long target_us = atol(ad) > 1 ? atol(ad) : 33333;   /* HV_ADPF=<target us> or 1 */
        if (pthread_create(&t, nullptr, hv_adpf_thread, (void*)(intptr_t)(target_us * 1000)) == 0) pthread_detach(t);
    }
    if (const char* uc = getenv("HV_UCLAMP")) {
        pthread_t t;
        if (pthread_create(&t, nullptr, hv_uclamp_thread, (void*)(uintptr_t)atoi(uc)) == 0) pthread_detach(t);
    }
    if (const char* se = getenv("HV_STACKS_EVERY")) {
        pthread_t t;
        pthread_create(&t, nullptr, stacks_thread, (void*)(uintptr_t)atoi(se));
        pthread_detach(t);
    }
    const char* e = getenv("HV_PROF");
    if (!e || atoi(e) <= 0) return;
    s_prof_pc = (uintptr_t*)calloc(kProfMax, sizeof(uintptr_t));
    {   /* libc's mapped range, from its own symbols' module base and /proc/self/maps */
        Dl_info di;
        if (dladdr((void*)&nanosleep, &di) && di.dli_fbase) {
            s_libc_lo = (uintptr_t)di.dli_fbase;
            s_libc_hi = s_libc_lo;
            if (FILE* f = fopen("/proc/self/maps", "r")) {
                char line[512];
                while (fgets(line, sizeof line, f)) {
                    unsigned long lo, hi;
                    if (strstr(line, "/libc.so") && sscanf(line, "%lx-%lx", &lo, &hi) == 2 && hi > s_libc_hi)
                        s_libc_hi = hi;
                }
                fclose(f);
            }
        }
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigprof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    pthread_t t;
    const char* colon = strchr(e, ':');
    const long delay = colon ? atol(e) : 0, secs = colon ? atol(colon + 1) : atol(e);
    pthread_create(&t, nullptr, prof_thread, (void*)(intptr_t)((delay << 16) | (secs & 0xFFFF)));
    pthread_detach(t);
    std::fprintf(stderr, "[prof] sampling every 1 ms for %ss\n", e);
}
#endif

/* ---------------------------------------------------------------------------
 * HV_PROF=[delay:]seconds on Windows (the PC build): every millisecond each other thread of the process is
 * suspended and its RIP read. At the end, per thread (with its name), the hottest addresses go to stderr as
 * "[prof] <tid> <name> +<rva> <samples>"; llvm-symbolizer resolves the RVAs against the unstripped .exe.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
static DWORD WINAPI hv_prof_thread_win(LPVOID arg)
{
    const long packed = (long)(intptr_t)arg, secs = packed & 0xFFFF, delay = packed >> 16;
    Sleep((DWORD)delay * 1000);
    /* HV_PROF_GO=<file>: start only once that file exists (a script creates it when the fight is on screen). */
    if (const char* go = getenv("HV_PROF_GO"))
        while (GetFileAttributesA(go) == INVALID_FILE_ATTRIBUTES) Sleep(100);
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::map<DWORD, std::map<uintptr_t, unsigned>> hist;
    std::map<DWORD, unsigned> total;
    std::fprintf(stderr, "[prof] sampling every 1 ms for %lds\n", secs);
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)secs * 1000;
    std::vector<std::pair<DWORD, HANDLE>> th;
    th.reserve(512);
    struct Samp { DWORD tid; uintptr_t pc; };
    const size_t kMax = 4u << 20;
    Samp* samp = (Samp*)malloc(kMax * sizeof(Samp));
    size_t n_samp = 0;
    ULONGLONG refresh = 0;
    while (GetTickCount64() < end) {
        if (GetTickCount64() >= refresh) {   /* the thread list, once a second */
            for (auto& t : th) CloseHandle(t.second);
            th.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te; te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (h) th.push_back({te.th32ThreadID, h});
            }
            CloseHandle(snap);
            refresh = GetTickCount64() + 1000;
        }
        /* Nothing may allocate while a thread is suspended: it can be holding the heap lock. */
        for (auto& t : th) {
            if (n_samp >= kMax) break;
            if (SuspendThread(t.second) == (DWORD)-1) continue;
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(t.second, &c);
            ResumeThread(t.second);
            if (ok) samp[n_samp++] = { t.first, (uintptr_t)c.Rip };
        }
        Sleep(1);
    }
    for (size_t i = 0; i < n_samp; i++) { hist[samp[i].tid][samp[i].pc]++; total[samp[i].tid]++; }
    free(samp);
    for (auto& t : hist) {
        if (total[t.first] < 50) continue;
        unsigned in_exe = 0;
        for (auto& a : t.second) if (a.first >= base && a.first < base + 0x10000000) in_exe += a.second;
        std::string name = "?";
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, t.first);
        PWSTR wn = nullptr;
        if (h && SUCCEEDED(GetThreadDescription(h, &wn)) && wn) {
            char buf[128]; WideCharToMultiByte(CP_UTF8, 0, wn, -1, buf, sizeof buf, NULL, NULL); name = buf; LocalFree(wn);
        }
        if (h) CloseHandle(h);
        for (auto& c : name) if (c == ' ') c = '_';
        std::vector<std::pair<unsigned, uintptr_t>> v;
        for (auto& a : t.second) v.push_back({a.second, a.first});
        std::sort(v.rbegin(), v.rend());
        std::fprintf(stderr, "[prof] thread %lu %s: %u samples, %u%% running in the exe\n", (unsigned long)t.first,
                     name.c_str(), total[t.first], 100u * in_exe / total[t.first]);
        for (size_t i = 0; i < v.size() && i < 25; i++) {
            const uintptr_t pc = v[i].second;
            if (pc >= base && pc < base + 0x10000000) std::fprintf(stderr, "[prof] %lu %s exe+0x%llx %u\n", (unsigned long)t.first, name.c_str(), (unsigned long long)(pc - base), v[i].first);
            else std::fprintf(stderr, "[prof] %lu %s abs 0x%llx %u\n", (unsigned long)t.first, name.c_str(), (unsigned long long)pc, v[i].first);
        }
    }
    for (auto& t : th) CloseHandle(t.second);
    return 0;
}
extern "C" void hv_prof_start(void)
{
    const char* e = getenv("HV_PROF");
    if (!e || atoi(e) <= 0) return;
    const char* colon = strchr(e, ':');
    const long delay = colon ? atol(e) : 0, secs = colon ? atol(colon + 1) : atol(e);
    CreateThread(NULL, 0, hv_prof_thread_win, (LPVOID)(intptr_t)((delay << 16) | (secs & 0xFFFF)), 0, NULL);
}
#endif
