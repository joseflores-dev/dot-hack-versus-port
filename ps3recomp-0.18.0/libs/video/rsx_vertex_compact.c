#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define RSX_FETCH_NEON 1
#endif

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_dsp_vertex_default(rsx, attr, fetch->default_value);
        if (attr == 3 &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (rsx_vertex_freq_plain) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        if (rsx_vertex_freq_plain(fetch->desc.frequency,
                                  (plan->divider_mask >> attr) & 1u)) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_element_index(
                        refs[i].vertex_id, refs[i].base_index, 0, 0);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix = rsx_dsp_resolve_vertex_offset(
            plan->base_offset, fetch->desc.offset);
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int rsx_vertex_fetch_attr_value(
    const rsx_vertex_fetch_plan* plan, u32 attr,
    const rsx_vertex_ref* ref, float* value)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    if (!fetch->desc.type || !fetch->desc.size) {
        memcpy(value, fetch->default_value, 16);
        return 1;
    }

    const u32 element = rsx_vertex_element_index(
        ref->vertex_id, ref->base_index, fetch->desc.frequency,
        (plan->divider_mask >> attr) & 1u);
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base +
            (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        /* An inline draw has no vertex array to fall back to: the array
         * offsets still hold whatever the last ordinary draw bound, and
         * reading them would feed this draw another mesh's vertices.
         * An attribute the stream does not supply gets its default,
         * which is what the hardware feeds. */
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 offset =
            rsx_dsp_resolve_vertex_offset(plan->base_offset,
                                          fetch->desc.offset) +
            element * fetch->stride;
        source = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, offset,
            fetch->elem_size);
    }

    if (source && rsx_vertex_decode_element(
            fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0)
        return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++)
        if (!rsx_vertex_fetch_attr_value(plan, plan->layout.attrs[slot], ref,
                                   (float*)(out + slot * 16u)))
            return 0;
    return 1;
}

/* The stable-range loop for one attribute and one vertex type: `decode`
 * reads `src` into `v` (pre-set to 0,0,0,1) for components c < n. Elements
 * outside the stable range take the one-vertex path. */
#define RSX_FETCH_LOOP(decode)                                                  \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                          \
            if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;      \
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode; }                                 \
    }

/* Same walk as RSX_FETCH_LOOP, with one statement filling the whole float4
 * `v` from `src` (the NEON paths below). */
#define RSX_FETCH_LOOPV(stmt)                                                   \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                          \
            if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;      \
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        stmt;                                                                   \
    }

#ifdef RSX_FETCH_NEON
/* Four big-endian halves to floats, exactly as rsx_compact_be_f16: a zero
 * exponent gives a signed zero (no denormals), 31 gives inf/NaN with the
 * mantissa shifted up, anything else rebiases. Integer ops only, so the bits
 * match the scalar form. */
static inline uint32x4_t rsx_neon_be_f16x4(uint16x4_t be)
{
    const uint32x4_t h = vmovl_u16(vreinterpret_u16_u8(vrev16_u8(vreinterpret_u8_u16(be))));
    const uint32x4_t sign = vshlq_n_u32(vshrq_n_u32(h, 15), 31);
    const uint32x4_t e = vandq_u32(vshrq_n_u32(h, 10), vdupq_n_u32(0x1F));
    const uint32x4_t man13 = vshlq_n_u32(vandq_u32(h, vdupq_n_u32(0x3FF)), 13);
    const uint32x4_t normal = vorrq_u32(sign, vaddq_u32(vshlq_n_u32(vandq_u32(h, vdupq_n_u32(0x7FFF)), 13),
                                                        vdupq_n_u32(112u << 23)));
    const uint32x4_t special = vorrq_u32(vorrq_u32(sign, vdupq_n_u32(0x7F800000u)), man13);
    uint32x4_t r = vbslq_u32(vceqq_u32(e, vdupq_n_u32(31)), special, normal);
    return vbslq_u32(vceqq_u32(e, vdupq_n_u32(0)), sign, r);
}
#endif

#ifdef RSX_FETCH_NEON
/* The first n (1..4) bytes / big-endian halves of an attribute as one word,
 * reading only what the attribute owns (the last vertex of a tightly packed
 * array may end the mapping). */
static inline u32 rsx_fetch_bytes(const u8* src, u32 n)
{
    u32 w = 0;
    if (n >= 4) { memcpy(&w, src, 4); return w; }
    for (u32 c = 0; c < n; c++) w |= (u32)src[c] << (8 * c);
    return w;
}
static inline u64 rsx_fetch_halves(const u8* src, u32 n)
{
    u64 w = 0;
    if (n >= 4) { memcpy(&w, src, 8); return w; }
    memcpy(&w, src, 2 * n);
    return w;
}
#endif

int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count))
        return 0;
    const u32 stride = plan->layout.stride;
#ifdef RSX_FETCH_NEON
    static int neon = -1;   /* HV_NO_FETCH_NEON=1: the per-component loops only */
    if (neon < 0) neon = getenv("HV_NO_FETCH_NEON") == NULL;
#endif
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        const u8* base = fetch->stable_base;
        const u32 type = fetch->desc.type;
        if (!type || !fetch->desc.size || !base ||
            type < RSX_VTX_TYPE_SNORM16 || type > RSX_VTX_TYPE_UINT8) {
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            continue;
        }
        const u32 first = fetch->stable_first;
        const u32 span = fetch->stable_last - first;
        const u32 fstride = fetch->stride;
        const u32 modulo = (plan->divider_mask >> attr) & 1u;
        const int plain = rsx_vertex_freq_plain(fetch->desc.frequency, modulo);
        const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
#ifdef RSX_FETCH_NEON
            /* The common float4/float3 attributes, a whole vertex at a time. */
            if (neon && n == 4) {
                RSX_FETCH_LOOPV(vst1q_u8((u8*)v, vrev32q_u8(vld1q_u8(src))))
                break;
            }
            if (neon && n == 3) {
                /* Lanes 0-2 from the vertex, lane 3 = BE 1.0f (0x3F800000 after the
                 * rev32), built in registers: the stack temporary and its two
                 * memcpy were the hottest lines of a fight's RSX thread. */
                RSX_FETCH_LOOPV({ u32 w2; memcpy(&w2, src + 8, 4);
                                  const uint32x2_t lo = vreinterpret_u32_u8(vld1_u8(src));
                                  const uint32x2_t hi = vset_lane_u32(w2, vdup_n_u32(0x0000803Fu), 0);
                                  vst1q_u8((u8*)v, vrev32q_u8(vreinterpretq_u8_u32(vcombine_u32(lo, hi)))); })
                break;
            }
#endif
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f32(src + c * 4))
            break;
        case RSX_VTX_TYPE_HALF:
#ifdef RSX_FETCH_NEON
            if (neon && n == 4) {
                RSX_FETCH_LOOPV(vst1q_u32((u32*)v, rsx_neon_be_f16x4(vreinterpret_u16_u8(vld1_u8(src)))))
                break;
            }
            if (neon && n == 2) {   /* z = 0, w = 1: lane 1 of the pair = (BE 1.0h << 16) | 0 */
                RSX_FETCH_LOOPV({ u32 w; memcpy(&w, src, 4);
                                  const uint32x2_t t = vset_lane_u32(w, vdup_n_u32(0x003C0000u), 0);
                                  vst1q_u32((u32*)v, rsx_neon_be_f16x4(vreinterpret_u16_u32(t))); })
                break;
            }
            if (neon && n == 3) {   /* w = 1 */
                RSX_FETCH_LOOPV({ u32 w; u16 h2; memcpy(&w, src, 4); memcpy(&h2, src + 4, 2);
                                  const uint32x2_t t = vset_lane_u32(w, vdup_n_u32(0x003C0000u | h2), 0);
                                  vst1q_u32((u32*)v, rsx_neon_be_f16x4(vreinterpret_u16_u32(t))); })
                break;
            }
            if (neon && n == 1) {   /* y = z = 0, w = 1 */
                RSX_FETCH_LOOPV({ u16 h0; memcpy(&h0, src, 2);
                                  const uint32x2_t t = vset_lane_u32(h0, vdup_n_u32(0x003C0000u), 0);
                                  vst1q_u32((u32*)v, rsx_neon_be_f16x4(vreinterpret_u16_u32(t))); })
                break;
            }
#endif
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f16(src + c * 2))
            break;
#ifdef RSX_FETCH_NEON
        /* Byte and 16-bit integer attributes, one vertex at a time: the same
         * int->float conversion and the same IEEE division as the per-component
         * loops below (bit-identical), lanes past `n` set to (0, 0, 0, 1). */
#define RSX_FETCH_NEON_INT(load, scale)                                             \
        if (neon) {                                                                 \
            const uint32x4_t keep = { n > 0 ? ~0u : 0u, n > 1 ? ~0u : 0u,          \
                                      n > 2 ? ~0u : 0u, n > 3 ? ~0u : 0u };         \
            const float32x4_t dflt = { 0.0f, 0.0f, 0.0f, 1.0f };                    \
            RSX_FETCH_LOOPV({ float32x4_t f = load;                                 \
                              if (scale != 1.0f) f = vdivq_f32(f, vdupq_n_f32(scale)); \
                              vst1q_f32(v, vbslq_f32(keep, f, dflt)); })            \
            break;                                                                  \
        }
        case RSX_VTX_TYPE_UNORM8:
            RSX_FETCH_NEON_INT(vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(
                                   vdup_n_u32(rsx_fetch_bytes(src, n))))))), 255.0f)
            RSX_FETCH_LOOP(v[c] = src[c] / 255.0f)
            break;
        case RSX_VTX_TYPE_UINT8:
            RSX_FETCH_NEON_INT(vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(
                                   vdup_n_u32(rsx_fetch_bytes(src, n))))))), 1.0f)
            RSX_FETCH_LOOP(v[c] = (float)src[c])
            break;
        case RSX_VTX_TYPE_SNORM16:
            RSX_FETCH_NEON_INT(vcvtq_f32_s32(vmovl_s16(vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_u64(
                                   vdup_n_u64(rsx_fetch_halves(src, n))))))), 32767.0f)
            RSX_FETCH_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
            break;
        case RSX_VTX_TYPE_SINT16:
            RSX_FETCH_NEON_INT(vcvtq_f32_s32(vmovl_s16(vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_u64(
                                   vdup_n_u64(rsx_fetch_halves(src, n))))))), 1.0f)
            RSX_FETCH_LOOP(v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
            break;
#undef RSX_FETCH_NEON_INT
#else
        case RSX_VTX_TYPE_UNORM8:
            RSX_FETCH_LOOP(v[c] = src[c] / 255.0f)
            break;
        case RSX_VTX_TYPE_UINT8:
            RSX_FETCH_LOOP(v[c] = (float)src[c])
            break;
        case RSX_VTX_TYPE_SNORM16:
            RSX_FETCH_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
            break;
        case RSX_VTX_TYPE_SINT16:
            RSX_FETCH_LOOP(v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
            break;
#endif
        default:   /* RSX_VTX_TYPE_CMP32: one packed word, whatever the size */
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            break;
        }
    }
    return 1;
}
#undef RSX_FETCH_LOOP
#undef RSX_FETCH_LOOPV

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    /* Dense fast path: one base and vertex ids spanning a range not much wider
     * than the draw -- an ordinary indexed mesh. Unique vertex k is then
     * min + k (every id in between is inside the referenced range), and no
     * hashing is needed; it was ~14% of the FIFO thread in a .hack//Versus
     * fight. The order of the unique set differs from first-occurrence order,
     * which nothing depends on: callers go through occurrence_to_unique. */
    static int fast_off = -1;
    if (fast_off < 0) fast_off = getenv("HV_NO_REMAP_FAST") != NULL;
    if (!fast_off) {
        const u32 base = refs[0].base_index;
        u32 lo = refs[0].vertex_id, hi = lo, same = 1;
        for (u32 i = 1; i < count; i++) {
            const u32 v = refs[i].vertex_id;
            same &= refs[i].base_index == base;
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        const u64 span = (u64)hi - lo + 1u;
        /* span < count: real duplicates, so the caller marks the draw remapped and
         * goes through occurrence_to_unique. With span == count it takes the refs
         * in their new (sorted) order as the occurrence order -- crossed vertices
         * (the title menu lost the left half of its text). */
        if (same && span < (u64)count) {
            for (u32 i = 0; i < count; i++)
                remap->occurrence_to_unique[i] = refs[i].vertex_id - lo;
            for (u32 k = 0; k < (u32)span; k++) {
                refs[k].base_index = base;
                refs[k].vertex_id = lo + k;
            }
            *unique_count = (u32)span;
            return 1;
        }
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
