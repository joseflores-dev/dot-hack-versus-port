/*
 * ps3recomp - HLSL -> SPIR-V through glslang, for the Vulkan backend
 *
 * See rsx_shader_spirv.h for the binding contract. The front end is driven
 * exactly as rsx_shader_msl.cpp drives it (C++ API for setEntryPoint, Vulkan
 * rules, auto-mapped bindings and locations, then mapIO, legalized SPIR-V),
 * with three differences, all because the output goes to a Vulkan driver
 * rather than to spirv-cross:
 *
 *   - Vulkan 1.0 client, SPIR-V 1.0: the backend creates a 1.0 instance and
 *     promises to run on 1.0-level drivers. (The MSL path targets 1.3 because
 *     that is spirv-cross's floor, not a driver's.)
 *   - Texture and sampler registers are shifted apart (setShiftBinding),
 *     because Vulkan has one binding namespace per set.
 *   - The vertex stage flips Y (setInvertY): D3D clip space is +y up.
 *
 * Translations are cached. glslang takes 20-150 ms per shader, on the thread
 * that walks the FIFO, and the engine asks for the same HLSL again for every
 * pipeline that shares a program (Twisted Metal: 236 translations in its
 * first minute, 74 distinct). The cache key is the stage plus the whole HLSL
 * text, so a hit returns exactly the words that text translated to. With
 * PS3RECOMP_SPIRV_CACHE=<existing directory> the cache is also kept on disk,
 * one file per shader, and the next run starts warm. A file whose key,
 * glslang version or translator build differs from the request is a miss,
 * and is rewritten.
 */

#include "rsx_shader_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS3RECOMP_HAVE_SPIRV_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void put_log(char* log, u32 log_size, const char* what, const char* detail)
{
    if (!log || log_size == 0) return;
    snprintf(log, log_size, "%s%s%s", what ? what : "",
             (what && detail && *detail) ? ": " : "", detail ? detail : "");
}

bool glslang_ready()
{
    static bool inited = false;
    if (!inited) {
        if (!glslang::InitializeProcess()) return false;
        inited = true;
    }
    return true;
}

/* The decompilers declare each register file as an array (float4 r[48]; float4 h[48]; float4 o[16]; float4 v[16])
 * and clear it with an [unroll] loop. glslang is built without its optimizer here, so the SPIR-V keeps those as
 * function-scope arrays; the Adreno compiler then put them in private memory instead of registers -- 1.5 KB per
 * pixel, and a full-screen quad cost the GPU ~150 ms. Every access but the clearing loop uses a constant index,
 * so rewrite them as one variable per register (r[5] -> r_5); unused ones then simply vanish.
 * HV_NO_SCALARIZE=1 compiles the text unchanged. */
static bool is_ident(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; }

static std::string scalarize_register_files(const std::string& in)
{
    static const char kFiles[] = "rhov";
    int size[4] = { 0, 0, 0, 0 };
    std::string out;
    out.reserve(in.size() * 2);
    size_t i = 0;
    while (i < in.size()) {
        /* "float4 X[N];" for a register file: declare X_0..X_{N-1} instead. */
        if (in.compare(i, 7, "float4 ") == 0 && (i == 0 || !is_ident(in[i - 1])) && i + 9 < in.size()) {
            const char* f = strchr(kFiles, in[i + 7]);
            if (f && *f && in[i + 8] == '[') {
                size_t j = i + 9; int n = 0;
                while (j < in.size() && in[j] >= '0' && in[j] <= '9') n = n * 10 + (in[j++] - '0');
                if (n > 0 && j + 1 < in.size() && in[j] == ']' && in[j + 1] == ';') {
                    size[f - kFiles] = n;
                    out += "float4 ";
                    for (int k = 0; k < n; k++) { out += in[i + 7]; out += '_'; out += std::to_string(k); out += k + 1 < n ? ", " : ";"; }
                    i = j + 2;
                    continue;
                }
            }
        }
        /* "[unroll] for (int _i...; _i < N; _i++) { X[_i] = E; ... }": spell the clears out. */
        if (in.compare(i, 8, "[unroll]") == 0) {
            const size_t eol = in.find('\n', i);
            const std::string line = in.substr(i, eol == std::string::npos ? std::string::npos : eol - i);
            const size_t fo = line.find("for"), lt = line.find('<'), cl = line.find(')');
            if (fo != std::string::npos && lt != std::string::npos && cl != std::string::npos) {
                std::string var;
                for (size_t k = line.find("int", fo) + 3; k < line.size() && line[k] != '='; k++)
                    if (line[k] != ' ') var += line[k];
                const int n = atoi(line.c_str() + lt + 1);
                std::string body = line.substr(cl + 1);
                for (char& ch : body) if (ch == '{' || ch == '}') ch = ' ';
                std::string expanded;
                bool ok = n > 0 && !var.empty();
                size_t b = 0;
                while (ok && b < body.size()) {
                    size_t e = body.find(';', b);
                    if (e == std::string::npos) break;
                    std::string st = body.substr(b, e - b);
                    b = e + 1;
                    size_t a = st.find_first_not_of(' ');
                    if (a == std::string::npos) continue;
                    st = st.substr(a);
                    const std::string idx = "[" + var + "]";
                    const size_t br = st.find(idx), eq = st.find('=');
                    if (br != 1 || !strchr(kFiles, st[0]) || eq == std::string::npos) { ok = false; break; }
                    const std::string rhs = st.substr(eq + 1);
                    for (int k = 0; k < n; k++) { expanded += st[0]; expanded += '_'; expanded += std::to_string(k); expanded += " =" + rhs + "; "; }
                }
                if (ok && !expanded.empty()) {
                    out += expanded;
                    i += line.size();
                    continue;
                }
            }
        }
        /* "X[k]" with a constant index -> "X_k". */
        if (strchr(kFiles, in[i]) && in[i] && (i == 0 || (!is_ident(in[i - 1]) && in[i - 1] != '.')) &&
            i + 2 < in.size() && in[i + 1] == '[' && in[i + 2] >= '0' && in[i + 2] <= '9') {
            size_t j = i + 2; int k = 0;
            while (j < in.size() && in[j] >= '0' && in[j] <= '9') k = k * 10 + (in[j++] - '0');
            if (j < in.size() && in[j] == ']' && size[strchr(kFiles, in[i]) - kFiles] > k) {
                out += in[i]; out += '_'; out += std::to_string(k);
                i = j + 1;
                continue;
            }
        }
        out += in[i++];
    }
    return out;
}

int translate(const char* hlsl, int stage, std::vector<unsigned int>& spirv,
              char* log, u32 log_size)
{
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }

    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX)  ? EShLangVertex
                           : (stage == RSX_SHADER_STAGE_COMPUTE) ? EShLangCompute
                                                                 : EShLangFragment;
    const EShMessages msgs =
        (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    glslang::TShader shader(lang);
    static const bool keep = getenv("HV_NO_SCALARIZE") != nullptr;
    std::string text = keep ? std::string(hlsl) : scalarize_register_files(hlsl);
    /* HV_FP_IDCOLOR=1: every fragment program paints a flat colour of its own (from a hash of its text), so one
     * screenshot shows which program draws which pixels. */
    /* HV_SKIP_FP=<hash>: that fragment program discards every pixel, so the draws it makes vanish -- find the
     * program behind an artifact by elimination. The hash is the one HV_FP_IDCOLOR logs. */
    /* HV_SHADER_DUMP also gets the text glslang really compiles, as x_<hash>.hlsl with the hash HV_SKIP_FP and
     * HV_FP_IDCOLOR use, so an artifact found by elimination maps straight to its source. */
    if (const char* dd = getenv("HV_SHADER_DUMP")) {
        unsigned h = 2166136261u;
        for (unsigned char ch : text) { h ^= ch; h *= 16777619u; }
        char name[512];
        snprintf(name, sizeof name, "%s/x_%s_%08X.hlsl", dd, lang == EShLangFragment ? "fs" : "vs", h);
        if (FILE* f = fopen(name, "w")) { fputs(text.c_str(), f); fclose(f); }
    }
    /* HV_NAN_MAGENTA=1: an output that is NaN paints magenta instead of the decompiler's black, to tell a NaN
     * apart from a genuine zero. */
    if (getenv("HV_NAN_MAGENTA") && lang == EShLangFragment)
        for (size_t at = 0; (at = text.find(") ? _o", at)) != std::string::npos; ) {
            const size_t z = text.find(": (float4)0;", at);
            if (z == std::string::npos || z - at > 16) { at++; continue; }
            text.replace(z, 12, ": float4(1.0, 0.0, 1.0, 1.0);");
            at = z;
        }
    /* HV_TEXCONST=<hash>:<unit>[:<unit>...]: in that fragment program, samples of those texture units read a flat
     * (0.5, 0.5, 1, 1) instead -- tells a texture decode problem from a maths one. */
    if (const char* tc = getenv("HV_TEXCONST"))
        if (lang == EShLangFragment) {
            unsigned h = 2166136261u;
            for (unsigned char ch : text) { h ^= ch; h *= 16777619u; }
            if (h == (unsigned)strtoul(tc, nullptr, 16))
                for (const char* u = strchr(tc, ':'); u; u = strchr(u + 1, ':')) {
                    const std::string call = "rsx_tex[" + std::to_string(atoi(u + 1)) + "].Sample(";
                    for (size_t at; (at = text.find(call)) != std::string::npos; ) {
                        size_t depth = 0, end = at + call.size() - 1;
                        for (; end < text.size(); end++) {
                            if (text[end] == '(') depth++;
                            else if (text[end] == ')' && --depth == 0) break;
                        }
                        text.replace(at, end + 1 - at, "float4(0.5, 0.5, 1.0, 1.0)");
                    }
                    fprintf(stderr, "[texconst] fp %08X unit %d flat\n", h, atoi(u + 1));
                }
        }
    /* Vertex programs: everything that reaches the position is `precise` (SPIR-V NoContraction). Titles draw a
     * depth-only prepass with one vertex program and the colour passes with another, then test LEQUAL; the RSX
     * computes both positions bit-identically, but a mobile compiler fuses the multiply-adds of the two programs
     * differently, the colour pass came out a hair deeper on some triangles, failed the test whole (depth is
     * planar per triangle) and left them black -- the mosaic over .hack//Versus's characters.
     * HV_NO_PRECISE=1 compiles the vertex text unchanged. */
    static const bool no_precise = getenv("HV_NO_PRECISE") != nullptr;
    if (!no_precise && lang == EShLangVertex) {
        auto prefix_all = [&text](const std::string& what, const std::string& with) {
            for (size_t at = 0; (at = text.find(what, at)) != std::string::npos; ) {
                if (at == 0 || !is_ident(text[at - 1])) { text.insert(at, with); at += with.size(); }
                at += what.size();
            }
        };
        prefix_all("float4 r_0,", "precise ");
        prefix_all("float4 o_0,", "precise ");
        prefix_all("float4 _p =", "precise ");
        prefix_all("float4 _v =", "precise ");
        prefix_all("float4 m = vp_mul", "precise ");
        const std::string mul1 = "return (a == 0.0 || b == 0.0) ? 0.0 : a * b;";
        const size_t m1 = text.find(mul1);
        if (m1 != std::string::npos)
            text.replace(m1, mul1.size(), "precise float pr = a * b; return (a == 0.0 || b == 0.0) ? 0.0 : pr;");
    }
    /* HV_FS_SHOW=<hash>:<expr>: that fragment program paints float4(<expr>, 1) -- shows what a varying or an
     * intermediate actually holds across a mesh (a step at triangle edges means bad per-vertex data). */
    if (const char* fs = getenv("HV_FS_SHOW"))
        if (lang == EShLangFragment) {
            unsigned h = 2166136261u;
            for (unsigned char ch : text) { h ^= ch; h *= 16777619u; }
            const char* colon = strchr(fs, ':');
            const size_t ret = text.rfind("return _out;");
            if (colon && ret != std::string::npos && h == (unsigned)strtoul(fs, nullptr, 16)) {
                text.insert(ret, std::string("_out.t0 = float4(") + (colon + 1) + ", 1.0); ");
                fprintf(stderr, "[fsshow] fp %08X shows %s\n", h, colon + 1);
            }
        }
    /* The decompiler scales some inputs by _rsx_inv_w(SV_Position) = FragCoord.w. Vulkan already interpolates
     * them perspective-correctly, so the factor only adds error -- and on Adreno FragCoord.w read this way came
     * out unusable on some triangles: .hack//Versus normalizes a view vector built from such inputs, and every
     * character turned into a mosaic of black triangles. Use the inputs as interpolated. HV_KEEP_INVW=1 keeps
     * the factor. */
    if (!getenv("HV_KEEP_INVW") && lang == EShLangFragment) {
        const std::string d = "#define _rsx_inv_w(p) ((p).w)";
        for (size_t at; (at = text.find(d)) != std::string::npos; )
            text.replace(at, d.size(), "#define _rsx_inv_w(p) (1.0)");
    }
    static const char* skip = getenv("HV_SKIP_FP");
    if (skip && lang == EShLangFragment) {
        unsigned h = 2166136261u;
        for (unsigned char ch : text) { h ^= ch; h *= 16777619u; }
        const size_t body = text.find('{', text.find("main("));
        if (h == (unsigned)strtoul(skip, nullptr, 16) && body != std::string::npos) {
            text.insert(body + 1, " clip(-1.0); ");
            fprintf(stderr, "[skipfp] fp %08X discards\n", h);
        }
    }
    static const bool idcolor = getenv("HV_FP_IDCOLOR") != nullptr;
    if (idcolor && lang == EShLangFragment) {
        const size_t ret = text.rfind("return _out;");
        if (ret != std::string::npos) {
            unsigned h = 2166136261u;
            for (unsigned char ch : text) { h ^= ch; h *= 16777619u; }
            char col[160];
            snprintf(col, sizeof col, "_out.t0 = lerp(_out.t0, float4(%.3f, %.3f, %.3f, 1.0), 0.5); ",
                     ((h >> 0) & 255) / 255.0, ((h >> 8) & 255) / 255.0, ((h >> 16) & 255) / 255.0);
            text.insert(ret, col);
            fprintf(stderr, "[idcolor] fp %08X -> (%u,%u,%u)\n", h, h & 255, (h >> 8) & 255, (h >> 16) & 255);
        }
    }
    const char* strings[1] = { text.c_str() };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    /* SPIR-V FragCoord.w is 1/w (FXC's SV_POSITION.w is w): tells the FP
     * decompiler's _rsx_inv_w helper which one it gets. */
    shader.setPreamble("#define RSX_FRAGCOORD_W_IS_RCP 1\n");
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResTexture, RSX_SPIRV_TEXTURE_BINDING);
    shader.setShiftBinding(glslang::EResSampler, RSX_SPIRV_SAMPLER_BINDING);
    if (lang == EShLangVertex) shader.setInvertY(true);

    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        return -1;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return -1;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return -1;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* run the HLSL legalization passes */
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return -1;
    }
    return 0;
}

/* ---- translation cache ------------------------------------------------- */

const u32 kCacheFormat = 1;
const u32 kCacheMagic = 0x56505352u;   /* "RSPV" */
/* This file's compile time stands for translate()'s options and the binding
 * contract in rsx_shader_spirv.h: when either changes, this file is rebuilt,
 * and the files an older build wrote become misses. */
const char kTranslatorBuild[24] = __DATE__ " " __TIME__;

struct Cache {
    std::mutex lock;
    std::unordered_map<std::string, std::vector<unsigned int>> map;
    std::string dir;
    bool dir_read = false;
    RsxSpirvCacheStats stats = {};
};

Cache& cache()
{
    static Cache c;
    return c;
}

const std::string& cache_dir(Cache& c)
{
    if (!c.dir_read) {
        const char* e = getenv("PS3RECOMP_SPIRV_CACHE");
        c.dir = (e && *e) ? e : "";
        c.dir_read = true;
    }
    return c.dir;
}

/* The key is the stage, then the HLSL text. */
std::string cache_key(const char* hlsl, int stage)
{
    std::string key(1, (char)('0' + stage));
    key += hlsl;
    return key;
}

std::string cache_path(const std::string& dir, const std::string& key)
{
    unsigned long long h = 0xcbf29ce484222325ull;       /* FNV-1a 64 */
    for (unsigned char ch : key) { h ^= ch; h *= 0x100000001b3ull; }
    char name[40];
    snprintf(name, sizeof name, "/%016llx.spv", h);
    return dir + name;
}

/* Header: magic, format, glslang major/minor/patch, translator build (six
 * words), key bytes, word count; then the key and the words. */
const int kHeaderWords = 13;

void cache_header(u32 hdr[kHeaderWords], const std::string& key, size_t nwords)
{
    const glslang::Version v = glslang::GetVersion();
    hdr[0] = kCacheMagic; hdr[1] = kCacheFormat;
    hdr[2] = (u32)v.major; hdr[3] = (u32)v.minor; hdr[4] = (u32)v.patch;
    memcpy(&hdr[5], kTranslatorBuild, sizeof kTranslatorBuild);
    hdr[11] = (u32)key.size(); hdr[12] = (u32)nwords;
}

bool cache_load(const std::string& path, const std::string& key,
                std::vector<unsigned int>& spirv)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u32 want[kHeaderWords], hdr[kHeaderWords];
    cache_header(want, key, 0);
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 &&
              memcmp(hdr, want, (kHeaderWords - 1) * sizeof(u32)) == 0 &&
              hdr[kHeaderWords - 1] != 0 && hdr[kHeaderWords - 1] <= (1u << 24);
    if (ok) {
        std::string stored(key.size(), '\0');
        ok = fread(&stored[0], 1, stored.size(), f) == stored.size() && stored == key;
    }
    if (ok) {
        spirv.resize(hdr[kHeaderWords - 1]);
        ok = fread(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    }
    fclose(f);
    if (!ok) spirv.clear();
    return ok;
}

/* Written under a temporary name and renamed, so a reader never sees half a file. */
void cache_store(const std::string& path, const std::string& key,
                 const std::vector<unsigned int>& spirv)
{
    char suffix[48];
    snprintf(suffix, sizeof suffix, ".%zx.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::string tmp = path + suffix;
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    u32 hdr[kHeaderWords];
    cache_header(hdr, key, spirv.size());
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(key.data(), 1, key.size(), f) == key.size() &&
              fwrite(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if (ok) remove(path.c_str());      /* rename does not replace on Windows */
#endif
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
}

} // namespace

extern "C" int rsx_hlsl_to_spirv_available(void) { return 1; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    if (stage != RSX_SHADER_STAGE_VERTEX && stage != RSX_SHADER_STAGE_FRAGMENT &&
        stage != RSX_SHADER_STAGE_COMPUTE) {
        put_log(log, log_size, "unknown shader stage", nullptr);
        return -1;
    }

    /* One lock around the whole lookup and translation: glslang's process
     * state is not documented as thread-safe either. */
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    const std::string key = cache_key(hlsl, stage);
    auto it = c.map.find(key);
    /* HV_SHADER_DUMP=<dir>: every distinct program's HLSL, once, as <dir>/<vs|fs>_<fnv>.hlsl. */
    if (it == c.map.end())
        if (const char* dd = getenv("HV_SHADER_DUMP")) {
            const std::string p = cache_path(dd, key);
            const size_t slash = p.find_last_of('/');
            const std::string name = std::string(dd) + "/" + (stage == RSX_SHADER_STAGE_VERTEX ? "vs_" : "fs_") +
                                     p.substr(slash == std::string::npos ? 0 : slash + 1) + ".hlsl";
            if (FILE* f = fopen(name.c_str(), "w")) { fputs(hlsl, f); fclose(f); }
        }
    if (it != c.map.end()) {
        c.stats.memory_hits++;
    } else {
        std::vector<unsigned int> spirv;
        const std::string& dir = cache_dir(c);
        const std::string path = dir.empty() ? std::string() : cache_path(dir, key);
        if (!dir.empty() && cache_load(path, key, spirv)) {
            c.stats.disk_hits++;
        } else {
            if (translate(hlsl, stage, spirv, log, log_size)) return -1;
            c.stats.translations++;
            if (!dir.empty()) cache_store(path, key, spirv);
        }
        it = c.map.emplace(key, std::move(spirv)).first;
    }
    const std::vector<unsigned int>& spirv = it->second;
    if (spirv.size() > max_words) {
        char n[64];
        snprintf(n, sizeof n, "%zu words, room for %u", spirv.size(), max_words);
        put_log(log, log_size, "SPIR-V output too large", n);
        return -1;
    }
    memcpy(words, spirv.data(), spirv.size() * sizeof(u32));
    *out_words = (u32)spirv.size();
    return 0;
}

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    if (out) *out = c.stats;
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    c.map.clear();
    c.dir_read = false;
    c.stats = {};
}

#else /* !PS3RECOMP_HAVE_SPIRV_TRANSLATION */

extern "C" int rsx_hlsl_to_spirv_available(void) { return 0; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    (void)hlsl; (void)stage; (void)words; (void)max_words;
    if (out_words) *out_words = 0;
    if (log && log_size)
        snprintf(log, log_size, "HLSL -> SPIR-V translation not built (glslang not found)");
    return -1;
}

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    if (out) memset(out, 0, sizeof *out);
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void) {}

#endif
