// The MCPX audio processing unit (0xfe800000, IRQ 5): what DirectSound drives. Its front end takes
// methods (0x20000 on) and keeps the voices (128 bytes each, an array in guest memory at
// NV_PAPU_VPVADDR) and their lists; the voice processor mixes every active voice, each 32-sample frame
// at 48 kHz, into 32 mix bins. The GP and EP DSPs (Motorola 56300s running DirectSound's effects and
// encoder microcode) run on xemu's DSP56300 interpreter (src/third_party/xemu-dsp, GPL-2), driven as
// xemu drives them with its DSP option off: 1000 cycles of the GP each frame, of the EP each 8th, their
// scratch and FIFO DMA through the APU's scatter-gather lists, the EP's main output silent; the sound
// heard is the voices' own mix, at each voice's loudest bin volume (xemu's voice-processor monitor).
// DirectSound waits on the DSPs' microcode at start-up (Halo spins on it). Register, method and voice
// layouts as xemu documents them (hw/xbox/mcpx/apu); the GP/EP glue follows xemu's gp_ep.c.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "core.h"
#include "devices.h"

extern "C" {
#include "third_party/xemu-dsp/dsp.h"
}

namespace xb {

void MixApuSound(const s16* samples, size_t frames); // audio.cpp

namespace {

constexpr u32 kBase = 0xfe800000, kSize = 0x80000, kIrq = 5;
constexpr int kVoices = 256, kVoices3D = 64, kFrame = 32, kBins = 32;

// registers (byte offsets)
constexpr u32 ISTS = 0x1000, IEN = 0x1004, FECTL = 0x1100, FECV = 0x1110, FEAV = 0x1118, FENADDR = 0x115c,
              FEDECMETH = 0x1300, FEDECPARAM = 0x1304, FEMEMADDR = 0x1324, FEMEMDATA = 0x1334,
              FETFORCE1 = 0x1504, SECTL = 0x2000, XGSCNT = 0x200c, VPVADDR = 0x202c, VPSGEADDR = 0x2030,
              VPSSLADDR = 0x2034;
constexpr u32 kListTop[3] = {0x2054, 0x2060, 0x206c}, kListCurrent[3] = {0x2058, 0x2064, 0x2070},
              kListNext[3] = {0x205c, 0x2068, 0x2074};
// voice structure offsets
constexpr u32 V_VBIN = 0x00, V_FMT = 0x04, V_ENV0 = 0x08, V_ENVA = 0x0c, V_ENV1 = 0x10, V_ENVF = 0x14, V_MISC = 0x18,
              V_HRTF = 0x1c, V_PSL_START = 0x20, V_PSH_SAMPLE = 0x24, V_ECNT = 0x34, V_STATE = 0x54,
              V_OFFSET = 0x58, V_NEXT = 0x5c, V_VOLA = 0x60, V_VOLB = 0x64, V_VOLC = 0x68, V_LFO_ENV = 0x6c,
              V_FCA = 0x74, V_FCB = 0x78, V_PITCH_LINK = 0x7c;
// state bits and envelope phases
constexpr u32 ST_PAUSED = 1u << 18, ST_NEW = 1u << 20, ST_ACTIVE = 1u << 21;
enum Phase { kOff, kDelay, kAttack, kHold, kDecay, kSustain, kRelease, kForceRelease };

std::recursive_mutex g_m;
u32 g_regs[0x20000 / 4] = {};
u32 g_gp_regs[0x10000 / 4] = {}, g_ep_regs[0x10000 / 4] = {}; // the DSPs' memories and registers, as written
u64 g_frames = 0;
bool g_irq_level = false;
DSPState* g_gp = nullptr;
DSPState* g_ep = nullptr;
u32 g_ep_frame_div = 0;

u32& Reg(u32 offset) { return g_regs[offset / 4]; }

void UpdateIrq()
{
    u32& ists = Reg(ISTS);
    if (Reg(FECTL) & 0xe0 && (Reg(FECTL) & 0xe0) == 0xe0) // trapped
        ists |= 1u << 4;
    const u32 ien = Reg(IEN);
    const bool level = (ien & 1) && (ists & ~1u & ien);
    if (level)
        ists |= 1;
    else
        ists &= ~1u;
    if (level != g_irq_level)
        SetIrq(kIrq, level);
    g_irq_level = level;
}

// ---- voices in guest memory

u32 VoiceAddress(u32 v) { return PhysToGuest(Reg(VPVADDR) + v * 0x80); }
u32 Get(u32 v, u32 offset) { const u32 a = VoiceAddress(v); return a ? Read<u32>(a + offset) : 0; }
void Put(u32 v, u32 offset, u32 value) { if (const u32 a = VoiceAddress(v)) Write<u32>(a + offset, value); }
u32 Field(u32 v, u32 offset, u32 mask) { return (Get(v, offset) & mask) >> __builtin_ctz(mask); }
void SetField(u32 v, u32 offset, u32 mask, u32 value)
{
    Put(v, offset, (Get(v, offset) & ~mask) | ((value << __builtin_ctz(mask)) & mask));
}

// per voice, on the host: its streaming lists (SSL A and B: base page and count), its resampler
struct VoiceHost
{
    u32 ssl_base[2] = {}, ssl_count[2] = {};
    int ssl_index = 0, ssl_seg = 0;
    double phase = 1.0;            // the resampler's position between prev and cur (in source samples)
    float prev[2] = {}, cur[2] = {};
    float source[64][2] = {};
    int source_at = 0, source_count = 0;
    bool ended = false;
    bool locked = false;
};
VoiceHost g_voice[kVoices];
u32 g_ssl_base_page = 0, g_inbuf_sge = 0, g_outbuf_sge = 0;
u8 g_hrtf_submix[4] = {}, g_submix_headroom[kBins] = {}, g_hrtf_headroom = 0;

void Notify(u32 v, int notifier)
{
    // notifiers: 16 bytes each, from FENADDR, two before the voices', four a voice; the status last
    const u32 at = PhysToGuest(Reg(FENADDR) + 16 * (2 + v * 4 + notifier));
    if (at)
    {
        Write<u8>(at + 15, 1); // done
        Write<u8>(at + 14, 1);
    }
    Reg(ISTS) |= (1u << 6) | (1u << 5); // FEVINTSTS, FENINTSTS
}

void VoiceOff(u32 v)
{
    SetField(v, V_STATE, ST_ACTIVE, 0);
    const bool stream = Get(v, V_FMT) & (1u << 24);
    Notify(v, stream ? std::min(g_voice[v].ssl_index, 1) : 0);
}

void SetEnvelopeStart(u32 v, u32 phase, u32 count_mask, u32 state_mask, u32 env0, u32 enva)
{
    SetField(v, V_STATE, state_mask, phase);
    if (phase == kDelay)
        SetField(v, V_ECNT, count_mask, Field(v, env0, 0xfff000) * 16);
    else if (phase == kAttack)
        SetField(v, V_ECNT, count_mask, 0);
    else if (phase == kHold)
        SetField(v, V_ECNT, count_mask, Field(v, enva, 0xfff000) * 16);
}

// ---- the front end's methods

void Method(u32 method, u32 argument)
{
    Reg(FEDECMETH) = method;
    Reg(FEDECPARAM) = argument;
    const u32 current = Reg(FECV) & 0xffff;
    auto set_current = [&](u32 offset, u32 mask, u32 value) {
        if (current < kVoices)
            SetField(current, offset, mask, value);
    };
    if (method >= 0x400 && method < 0x440) // HRIR coefficients: HRTF isn't done (the mix is stereo)
        return;
    if (method >= 0x600 && method < 0x800) // SSL segments: offset and length pairs for the current SSL
    {
        if (const u32 at = PhysToGuest(Reg(VPSSLADDR) + g_ssl_base_page * 8 + (method - 0x600)))
            Write<u32>(at, argument);
        return;
    }
    if (method >= 0x200 && method < 0x280) // submix headroom
    {
        g_submix_headroom[(method - 0x200) / 4] = u8(argument & 7);
        return;
    }
    if (method >= 0x1000 && method < 0x1020) // output buffers (the DSPs'): nothing to do here
        return;
    switch (method)
    {
    case 0x2fc: // VOICE_LOCK
        if (current < kVoices)
            g_voice[current].locked = argument & 1;
        return;
    case 0x120: Reg(FEAV) = argument; return; // SET_ANTECEDENT_VOICE
    case 0x124:                                 // VOICE_ON
    {
        const u32 v = argument & 0xffff;
        if (v >= kVoices)
            return;
        const u32 list = (Reg(FEAV) >> 16) & 3;
        if (list)
        {
            SetField(v, V_PITCH_LINK, 0xffff, Reg(kListTop[list - 1]) & 0xffff);
            Reg(kListTop[list - 1]) = v;
        }
        else
        {
            const u32 antecedent = Reg(FEAV) & 0xffff;
            if (antecedent < kVoices)
            {
                SetField(v, V_PITCH_LINK, 0xffff, Field(antecedent, V_PITCH_LINK, 0xffff));
                SetField(antecedent, V_PITCH_LINK, 0xffff, v);
            }
        }
        SetField(v, V_OFFSET, 0xffffff, 0);
        VoiceHost& h = g_voice[v];
        h.ssl_index = h.ssl_seg = 0;
        h.phase = 1.0, h.source_at = h.source_count = 0, h.ended = false;
        h.prev[0] = h.prev[1] = h.cur[0] = h.cur[1] = 0;
        SetEnvelopeStart(v, argument >> 28, 0xffff, 0xfu << 28, V_ENV0, V_ENVA);       // amplitude
        SetEnvelopeStart(v, (argument >> 24) & 0xf, 0xffff0000, 0xfu << 24, V_ENV1, V_ENVF); // filter/pitch
        SetField(v, V_STATE, ST_ACTIVE, 1);
        return;
    }
    case 0x12c: // VOICE_RELEASE: both envelopes to their release
    {
        const u32 v = argument & 0xffff;
        if (v >= kVoices)
            return;
        SetField(v, V_ECNT, 0xffff, Field(v, V_LFO_ENV, 0xfff) * 16);
        SetField(v, V_STATE, 0xfu << 28, kRelease);
        SetField(v, V_ECNT, 0xffff0000, Field(v, V_MISC, 0xfff) * 16);
        SetField(v, V_STATE, 0xfu << 24, kRelease);
        return;
    }
    case 0x128: if ((argument & 0xffff) < kVoices) VoiceOff(argument & 0xffff); return; // VOICE_OFF
    case 0x140:                                                                          // VOICE_PAUSE
        if ((argument & 0xffff) < kVoices)
            SetField(argument & 0xffff, V_STATE, ST_PAUSED, (argument >> 18) & 1);
        return;
    case 0x160: return;                         // SET_CURRENT_HRTF_ENTRY
    case 0x2f8: Reg(FECV) = argument; return;   // SET_CURRENT_VOICE
    case 0x300: set_current(V_VBIN, 0xffffffff, argument); return;
    case 0x304: set_current(V_FMT, 0xffffffff, argument); return;
    case 0x308: set_current(V_ENV0, 0xffffffff, argument); return;
    case 0x30c: set_current(V_ENVA, 0xffffffff, argument); return;
    case 0x310: set_current(V_ENV1, 0xffffffff, argument); return;
    case 0x314: set_current(V_ENVF, 0xffffffff, argument); return;
    case 0x318: set_current(V_MISC, 0xffffffff, argument); return;
    case 0x31c: set_current(V_HRTF, 0xffff, argument & 0xffff); return;
    case 0x360: set_current(V_VOLA, 0xffffffff, argument); return;
    case 0x364: set_current(V_VOLB, 0xffffffff, argument); return;
    case 0x368: set_current(V_VOLC, 0xffffffff, argument); return;
    case 0x36c: set_current(V_LFO_ENV, 0xffffffff, argument); return;
    case 0x374: set_current(V_FCA, 0xffffffff, argument); return;
    case 0x378: set_current(V_FCB, 0xffffffff, argument); return;
    case 0x37c: set_current(V_PITCH_LINK, 0xffff0000, argument >> 16); return; // the pitch step
    case 0x3a0: set_current(V_PSL_START, 0xffffff, argument); return;           // buffer base
    case 0x3a4: set_current(V_PSH_SAMPLE, 0xffffff, argument); return;          // loop start
    case 0x3d8: set_current(V_OFFSET, 0xffffff, argument); return;              // current offset
    case 0x3dc: set_current(V_NEXT, 0xffffff, argument); return;                // end offset
    case 0x320: // SET_VOICE_SSL_A / B: base page and count
    case 0x35c:
        if (current < kVoices)
        {
            const int s = method == 0x320 ? 0 : 1;
            g_voice[current].ssl_base[s] = argument >> 8;
            g_voice[current].ssl_count[s] = argument & 0xff;
        }
        return;
    case 0x190: g_ssl_base_page = argument; return; // SET_CURRENT_SSL
    case 0x804: g_inbuf_sge = argument; return;      // SET_CURRENT_INBUF_SGE
    case 0x808:                                      // ... its page
        if (const u32 at = PhysToGuest(Reg(VPSGEADDR) + g_inbuf_sge * 8))
            Write<u32>(at, argument & 0xfffff000);
        return;
    case 0x1800: g_outbuf_sge = argument; return;
    case 0x1808:
        if (const u32 at = PhysToGuest(Reg(VPSGEADDR) + g_outbuf_sge * 8))
            Write<u32>(at, argument & 0xfffff000);
        return;
    case 0x2c0: // SET_HRTF_SUBMIXES
        for (int i = 0; i < 4; i++)
            g_hrtf_submix[i] = u8((argument >> (8 * i)) & 0x1f);
        return;
    case 0x280: g_hrtf_headroom = u8(argument & 7); return;
    case 0x8000: // SE2FE_IDLE_VOICE: the processor met an inactive voice in a list
        if (Reg(FETFORCE1) & (1u << 15))
        {
            Reg(FECTL) = (Reg(FECTL) & ~0xfe0u) | 0xe0 | 0xf00; // trapped, as requested
            UpdateIrq();
        }
        return;
    }
    static bool reported[0x800] = {};
    if (method / 4 < 0x800 && !reported[method / 4])
    {
        reported[method / 4] = true;
        Log("apu: method %#x = %#x isn't done", method, argument);
    }
}

// ---- envelopes: a step a frame; returns the level (0..1)

float StepEnvelope(u32 v, bool amplitude)
{
    const u32 state_shift = amplitude ? 28 : 24, count_mask = amplitude ? 0xffff : 0xffff0000;
    const u32 env0 = amplitude ? V_ENV0 : V_ENV1, enva = amplitude ? V_ENVA : V_ENVF;
    const u32 level_reg = amplitude ? V_OFFSET : V_NEXT, level_mask = 0xff000000;
    u32 phase = (Get(v, V_STATE) >> state_shift) & 0xf;
    u32 count = Field(v, V_ECNT, count_mask);
    auto next = [&](u32 p, u32 c) {
        SetField(v, V_STATE, 0xfu << state_shift, p);
        SetField(v, V_ECNT, count_mask, c);
    };
    switch (phase)
    {
    case kOff:
        SetField(v, level_reg, level_mask, 0xff);
        return 1.f;
    case kDelay:
        SetField(v, level_reg, level_mask, 0);
        if (count == 0)
            next(kAttack, 0);
        else
            SetField(v, V_ECNT, count_mask, count - 1);
        return 0.f;
    case kAttack:
    {
        const u32 rate = Field(v, env0, 0xfff) * 16;
        const float value = rate == 0 || count >= rate ? 255.f : float(count) * 255.f / float(rate);
        SetField(v, level_reg, level_mask, u32(value));
        if (count >= rate)
            next(kHold, Field(v, enva, 0xfff000) * 16);
        else
            SetField(v, V_ECNT, count_mask, count + 1);
        return value / 255.f;
    }
    case kHold:
        SetField(v, level_reg, level_mask, 0xff);
        if (count == 0)
            next(kDecay, Field(v, enva, 0xfff) * 16);
        else
            SetField(v, V_ECNT, count_mask, count - 1);
        return 1.f;
    case kDecay:
    {
        const u32 rate = Field(v, enva, 0xfff), sustain = Field(v, enva, 0xff000000);
        // an exponential fall from full to the sustain level over the decay time
        const float value = rate == 0 ? 0.f : 255.f * std::pow(0.99988799f, float(rate * 16 - count) * 4096.f / float(rate));
        if (value <= float(sustain) + 0.2f || value > 255.f || count == 0)
        {
            SetField(v, V_STATE, 0xfu << state_shift, kSustain);
            return float(sustain) / 255.f;
        }
        SetField(v, V_ECNT, count_mask, count - 1);
        SetField(v, level_reg, level_mask, u32(value));
        return value / 255.f;
    }
    case kSustain:
    {
        const u32 sustain = Field(v, enva, 0xff000000);
        SetField(v, V_ECNT, count_mask, 0);
        SetField(v, level_reg, level_mask, sustain);
        return float(sustain) / 255.f;
    }
    case kRelease:
    {
        const u32 rate = amplitude ? Field(v, V_LFO_ENV, 0xfff) : Field(v, V_MISC, 0xfff);
        if (rate == 0 || count == 0)
        {
            SetField(v, V_STATE, 0xfu << state_shift, kForceRelease);
            return 0.f;
        }
        // from the level held to silence (-60 dB) over the release time
        const float pos = std::clamp(1.f - float(count) / float(rate * 16), 0.f, 1.f);
        const float value = std::exp(-6.91f * pos) * float(Field(v, level_reg, level_mask));
        SetField(v, V_ECNT, count_mask, count - 1);
        return value / 255.f;
    }
    default: // forced release: the amplitude envelope's end is the voice's
        if (amplitude)
            VoiceOff(v);
        return 0.f;
    }
}

// ---- samples

float S16(u32 a) { return float(Read<int16_t>(a)) / 32768.f; }

// IMA ADPCM, as the Xbox stores it: per channel a 4-byte header (the first sample, the step index),
// then 4-byte words of eight 4-bit codes, channels interleaved word by word; 36 bytes (65 samples) a
// block per channel.
void DecodeAdpcm(const u8* in, int channels, int16_t out[65][2])
{
    static const int16_t kSteps[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767};
    static const int kIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    int predictor[2], index[2];
    for (int c = 0; c < channels; c++)
    {
        predictor[c] = int16_t(in[c * 4] | in[c * 4 + 1] << 8);
        index[c] = std::clamp(int(in[c * 4 + 2]), 0, 88);
        out[0][c] = int16_t(predictor[c]);
    }
    const u8* p = in + 4 * channels;
    for (int word = 0; word < 8; word++)
        for (int c = 0; c < channels; c++)
        {
            for (int n = 0; n < 8; n++)
            {
                const int code = (p[n / 2] >> ((n & 1) * 4)) & 0xf;
                const int step = kSteps[index[c]];
                int delta = step >> 3;
                if (code & 1) delta += step >> 2;
                if (code & 2) delta += step >> 1;
                if (code & 4) delta += step;
                if (code & 8) delta = -delta;
                predictor[c] = std::clamp(predictor[c] + delta, -32768, 32767);
                index[c] = std::clamp(index[c] + kIndex[code & 7], 0, 88);
                out[1 + word * 8 + n][c] = int16_t(predictor[c]);
            }
            p += 4;
        }
}

// The address of byte `offset` of a buffer voice's data: through the SGE page table.
u32 SgeAddress(u32 offset)
{
    const u32 entry = PhysToGuest(Reg(VPSGEADDR) + (offset / 4096) * 8);
    return entry ? PhysToGuest(Read<u32>(entry) + offset % 4096) : 0;
}

// Up to `n` source samples of voice v (stereo; mono doubled); -1 when it has ended or is paused.
int GetSamples(u32 v, float out[][2], int n)
{
    const u32 fmt = Get(v, V_FMT);
    const bool stereo = fmt & (1u << 27), stream = fmt & (1u << 24), loop = fmt & (1u << 25);
    const bool persist = fmt & (1u << 23);
    const int channels = stereo ? 2 : 1;
    u32 sample_size = (fmt >> 28) & 3, container = (fmt >> 30) & 3;
    const u32 samples_per_block = 1 + ((fmt >> 16) & 0x1f);
    SetField(v, V_STATE, ST_NEW, 0);
    if (Get(v, V_STATE) & ST_PAUSED)
        return -1;
    VoiceHost& h = g_voice[v];
    u32 cbo = Field(v, V_OFFSET, 0xffffff), ebo = Field(v, V_NEXT, 0xffffff);
    const u32 lbo = Field(v, V_PSH_SAMPLE, 0xffffff), ba = Field(v, V_PSL_START, 0xffffff);
    u32 segment = 0;
    if (stream)
    {
        if (!persist && ((Get(v, V_STATE) >> 28) & 0xf) < kRelease)
        {
            VoiceOff(v);
            return -1;
        }
        const u32 count = h.ssl_count[h.ssl_index];
        if (count == 0) // the stream ran out
        {
            SetField(v, V_OFFSET, 0xffffff, 0);
            h.ssl_seg = 0;
            if (!persist)
            {
                h.ssl_index = 0;
                VoiceOff(v);
            }
            else
                Notify(v, h.ssl_index);
            return -1;
        }
        const u32 entry = PhysToGuest(Reg(VPSSLADDR) + (h.ssl_base[h.ssl_index] + u32(h.ssl_seg)) * 8);
        if (!entry)
            return -1;
        segment = Read<u32>(entry);
        const u32 length = Read<u32>(entry + 4);
        container = (length >> 16) & 3;
        if (container == 2)
            sample_size = 2;
        ebo = (length & 0xffff) ? (length & 0xffff) - 1 : 0;
    }
    const bool adpcm = container == 2;
    const u32 container_bytes[4] = {1, 2, 0, 4};
    const u32 block_bytes = (adpcm ? 36 : container_bytes[container]) * samples_per_block;
    int got = 0;
    int16_t decoded[65][2];
    int decoded_block = -1;
    for (; got < n && cbo <= ebo; got++, cbo++)
    {
        if (adpcm)
        {
            const int block = int(cbo / 64), at = int(cbo % 64);
            if (block != decoded_block)
            {
                u8 raw[72];
                const u32 linear = u32(block) * block_bytes;
                for (u32 i = 0; i < 36u * u32(channels); i++)
                {
                    const u32 a = stream ? PhysToGuest(segment + linear + i) : SgeAddress(ba + linear + i);
                    raw[i] = a ? Read<u8>(a) : 0;
                }
                DecodeAdpcm(raw, channels, decoded);
                decoded_block = block;
            }
            out[got][0] = decoded[at][0] / 32768.f;
            out[got][1] = stereo ? decoded[at][1] / 32768.f : out[got][0];
            continue;
        }
        u32 a = stream ? PhysToGuest(segment + cbo * block_bytes) : SgeAddress(ba + cbo * block_bytes);
        for (int c = 0; c < channels; c++, a += container_bytes[container])
        {
            float value = 0;
            if (a)
                switch (sample_size)
                {
                case 0: value = (float(Read<u8>(a)) - 128.f) / 128.f; break;
                case 1: value = S16(a); break;
                case 2: value = float(int32_t(Read<u32>(a) << 8) >> 8) / 8388608.f; break;
                default: value = float(int32_t(Read<u32>(a))) / 2147483648.f; break;
                }
            out[got][c] = value;
        }
        if (!stereo)
            out[got][1] = out[got][0];
    }
    if (cbo > ebo || (cbo == ebo && got < n))
    {
        if (stream)
        {
            h.ssl_seg++;
            cbo = 0;
            if (u32(h.ssl_seg) >= h.ssl_count[h.ssl_index])
            {
                Notify(v, h.ssl_index);
                h.ssl_index ^= 1;
                h.ssl_seg = 0;
            }
        }
        else if (loop)
            cbo = lbo;
        else
        {
            cbo = ebo;
            VoiceOff(v);
        }
    }
    SetField(v, V_OFFSET, 0xffffff, cbo);
    return got;
}

// The next source sample of v (silence once it has ended).
void NextSource(u32 v, float s[2])
{
    VoiceHost& h = g_voice[v];
    if (h.source_at >= h.source_count)
    {
        h.source_at = h.source_count = 0;
        if (!h.ended && (Get(v, V_STATE) & ST_ACTIVE))
        {
            const int got = GetSamples(v, h.source, 64);
            if (got < 0)
                h.ended = !(Get(v, V_STATE) & ST_ACTIVE);
            else
                h.source_count = got;
        }
    }
    if (h.source_at < h.source_count)
    {
        s[0] = h.source[h.source_at][0], s[1] = h.source[h.source_at][1];
        h.source_at++;
    }
    else
        s[0] = s[1] = 0;
}

// ---- a frame

float Attenuate(u32 volume) // 12 bits of attenuation, 1/64 dB each; 0xfff: silent
{
    volume &= 0xfff;
    return volume == 0xfff ? 0.f : std::pow(10.f, float(volume) / (64.f * -20.f));
}

float g_bins[kBins][kFrame];

// mixes voice v into the bins and, with `gain_out`, the gain to hear it at
bool ProcessVoice(u32 v, float samples[kFrame][2], float& heard_gain, int skip_bin)
{
    const u32 fmt = Get(v, V_FMT);
    if (Get(v, V_STATE) & ST_PAUSED)
        return false;
    const bool stereo = fmt & (1u << 27), multipass = fmt & (1u << 21);
    const float ef = StepEnvelope(v, false);
    const int16_t pitch = int16_t(Field(v, V_PITCH_LINK, 0xffff0000));
    const int8_t pitch_scale = int8_t(Field(v, V_ENV0, 0xff000000));
    const double step = std::pow(2.0, (double(pitch) + double(pitch_scale) * 32.0 * ef) / 4096.0);
    const float ea = StepEnvelope(v, true);
    if (multipass) // its input is a bin the voices before it filled
    {
        const u32 bin = (fmt >> 16) & 0x1f;
        for (int i = 0; i < kFrame; i++)
            samples[i][0] = samples[i][1] = g_bins[bin][i];
        if (fmt & (1u << 26)) // clear the bin after
            std::fill(std::begin(g_bins[bin]), std::end(g_bins[bin]), 0.f);
    }
    else
    {
        VoiceHost& h = g_voice[v];
        for (int i = 0; i < kFrame; i++)
        {
            while (h.phase >= 1.0)
            {
                h.prev[0] = h.cur[0], h.prev[1] = h.cur[1];
                NextSource(v, h.cur);
                h.phase -= 1.0;
            }
            const float f = float(h.phase);
            samples[i][0] = h.prev[0] + (h.cur[0] - h.prev[0]) * f;
            samples[i][1] = h.prev[1] + (h.cur[1] - h.prev[1]) * f;
            h.phase += step;
        }
    }
    if (!(Get(v, V_STATE) & ST_ACTIVE))
        return false;
    // eight bins and their volumes (the last two split over the three volume words)
    const u32 vbin = Get(v, V_VBIN);
    int bin[8] = {int(vbin & 0x1f), int((vbin >> 5) & 0x1f), int((vbin >> 10) & 0x1f), int((vbin >> 16) & 0x1f),
                  int((vbin >> 21) & 0x1f), int((vbin >> 26) & 0x1f), int(fmt & 0x1f), int((fmt >> 5) & 0x1f)};
    if (v < kVoices3D)
        for (int i = 0; i < 4; i++)
            bin[i] = g_hrtf_submix[i];
    const u32 a = Get(v, V_VOLA), b = Get(v, V_VOLB), c = Get(v, V_VOLC);
    const u32 volume[8] = {(a >> 4) & 0xfff, a >> 20, (b >> 4) & 0xfff, b >> 20, (c >> 4) & 0xfff, c >> 20,
                           (a & 0xf) | (b & 0xf) << 4 | (c & 0xf) << 8,
                           ((a >> 16) & 0xf) | ((b >> 16) & 0xf) << 4 | ((c >> 16) & 0xf) << 8};
    heard_gain = 0;
    for (int i = 0; i < 8; i++)
    {
        const bool hrtf = v < kVoices3D && i < 4;
        const float headroom = float(1 << (hrtf ? g_hrtf_headroom : g_submix_headroom[bin[i]]));
        const float g = ea * Attenuate(volume[i]) / headroom;
        for (int s = 0; s < kFrame; s++)
            g_bins[bin[i]][s] += g * samples[s][i % (stereo ? 2 : 1)];
        if (bin[i] != skip_bin)
            heard_gain = std::max(heard_gain, Attenuate(volume[i]) / float(1 << g_submix_headroom[bin[i]]));
    }
    heard_gain *= ea;
    return true;
}

// The bin a voice of the multipass list feeds: the multipass voice after it, if one follows.
int FeedsBin(u32 v)
{
    bool first = true;
    for (int guard = 0; v < kVoices && guard < kVoices; guard++)
    {
        const u32 fmt = Get(v, V_FMT);
        if (fmt & (1u << 21))
            return first ? -1 : int((fmt >> 16) & 0x1f);
        v = Field(v, V_PITCH_LINK, 0xffff);
        first = false;
    }
    return -1;
}

void Frame(float out[kFrame][2])
{
    for (auto& b : g_bins)
        std::fill(std::begin(b), std::end(b), 0.f);
    for (int s = 0; s < kFrame; s++)
        out[s][0] = out[s][1] = 0;
    if (!Reg(VPVADDR))
        return;
    for (int list = 0; list < 3; list++)
    {
        Reg(kListCurrent[list]) = Reg(kListTop[list]);
        for (int guard = 0; (Reg(kListCurrent[list]) & 0xffff) != 0xffff && guard < kVoices; guard++)
        {
            const u32 v = Reg(kListCurrent[list]) & 0xffff;
            if (v >= kVoices)
                break;
            Reg(kListNext[list]) = Field(v, V_PITCH_LINK, 0xffff);
            if (!(Get(v, V_STATE) & ST_ACTIVE))
                Method(0x8000, v); // SE2FE_IDLE_VOICE
            else if (!g_voice[v].locked)
            {
                float samples[kFrame][2] = {};
                float gain = 0;
                const int skip = list == 2 ? FeedsBin(v) : -1;
                if (ProcessVoice(v, samples, gain, skip) && skip < 0)
                    for (int s = 0; s < kFrame; s++)
                        out[s][0] += gain * samples[s][0], out[s][1] += gain * samples[s][1];
            }
            Reg(kListCurrent[list]) = Reg(kListNext[list]);
        }
    }
}

void DspFrame(const float out[kFrame][2]); // below

void Thread()
{
    // 15 frames (480 samples, 10 ms) at a time
    constexpr int kFrames = 15;
    std::vector<s16> pcm(kFrames * kFrame * 2);
    auto next = std::chrono::steady_clock::now();
    while (!g_quit)
    {
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
        bool sound = false;
        {
            std::lock_guard lock(g_m);
            const u32 fectl = Reg(FECTL);
            const bool running = ((Reg(SECTL) >> 3) & 3) != 0 && (fectl & 0xe0) != 0xe0 && (fectl & 0xe0) != 0x80;
            UpdateIrq();
            if (!running)
                continue;
            for (int f = 0; f < kFrames; f++)
            {
                float out[kFrame][2];
                Frame(out);
                DspFrame(out);
                g_frames++;
                for (int s = 0; s < kFrame; s++)
                    for (int c = 0; c < 2; c++)
                    {
                        const float x = std::clamp(out[s][c], -1.f, 1.f);
                        pcm[size_t((f * kFrame + s) * 2 + c)] = s16(std::lrint(x * 32767.f));
                        sound |= x != 0;
                    }
            }
            UpdateIrq();
        }
        if (sound)
            MixApuSound(pcm.data(), kFrames * kFrame);
    }
}

// ---- the GP and EP DSPs (xemu's gp_ep.c, adapted): scratch memory and FIFOs reach guest memory
// through scatter-gather lists of 4 KiB pages (8-byte entries: the page's physical address, control)

constexpr u32 GPSADDR = 0x2040, GPFADDR = 0x2044, EPSADDR = 0x2048, EPFADDR = 0x204c, GPSMAXSGE = 0x20d4,
              GPFMAXSGE = 0x20d8, EPSMAXSGE = 0x20dc, EPFMAXSGE = 0x20e0, GPOFBASE0 = 0x3024, GPIFBASE0 = 0x3064,
              EPOFBASE0 = 0x4024, EPIFBASE0 = 0x4064; // a FIFO's base, end and cur at +0, +4, +8; FIFOs 0x10 apart
constexpr u32 kDspRst = 0xfffc, kRstBits = 3; // GPRST/EPRST: GPRST | GPDSPRST running

void ScatterGather(u32 sge_base, u32 max_sge, u8* ptr, u32 addr, size_t len, bool to_guest)
{
    u32 entry = addr / 4096, in_page = addr % 4096, chunk = 4096 - in_page;
    while (len > 0)
    {
        if (entry > max_sge)
            return;
        const u32 at = PhysToGuest(sge_base + entry * 8);
        const u32 page = at && GuestMapped(at, 4) ? Read<u32>(at) : 0;
        chunk = u32(std::min<size_t>(chunk, len));
        const u32 guest = PhysToGuest(page + in_page);
        if (guest && GuestMapped(guest, chunk))
        {
            if (to_guest)
                std::memcpy(Ptr<u8>(guest), ptr, chunk);
            else
                std::memcpy(ptr, Ptr<u8>(guest), chunk);
        }
        else if (!to_guest)
            std::memset(ptr, 0, chunk);
        ptr += chunk;
        len -= chunk;
        entry++;
        in_page = 0;
        chunk = 4096;
    }
}

u32 CircularScatterGather(u32 sge_base, u32 max_sge, u8* ptr, u32 base, u32 end, u32 cur, size_t len, bool to_guest)
{
    if (end <= base)
        return cur;
    while (len > 0)
    {
        const u32 chunk = u32(std::min<size_t>(end - cur, len));
        ScatterGather(sge_base, max_sge, ptr, cur, chunk, to_guest);
        ptr += chunk;
        len -= chunk;
        cur += chunk;
        if (cur >= end)
            cur = base;
    }
    return cur;
}

void FifoRw(u32 out_base, u32 in_base, u32 count_out, u32 count_in, u32 sge, u32 max_sge, u8* ptr, unsigned index,
            size_t len, bool to_guest)
{
    if (index >= (to_guest ? count_out : count_in))
        return;
    const u32 r = (to_guest ? out_base : in_base) + 0x10 * index;
    const u32 base = Reg(r) & 0xffffff, end = Reg(r + 4) & 0xffffff;
    u32 cur = Reg(r + 8) & 0xffffff;
    if (end <= base)
        return;
    if (cur >= end)
        cur = base + (cur - base) % (end - base);
    if (cur < base)
        cur = base;
    cur = CircularScatterGather(Reg(sge), Reg(max_sge), ptr, base, end, cur, len, to_guest);
    Reg(r + 8) = (Reg(r + 8) & ~0xffffffu) | cur;
}

extern "C" void GpScratchRw(void*, uint8_t* ptr, uint32_t addr, size_t len, bool to_guest)
{
    ScatterGather(Reg(GPSADDR), Reg(GPSMAXSGE), ptr, addr, len, to_guest);
}
extern "C" void EpScratchRw(void*, uint8_t* ptr, uint32_t addr, size_t len, bool to_guest)
{
    ScatterGather(Reg(EPSADDR), Reg(EPSMAXSGE), ptr, addr, len, to_guest);
}
extern "C" void GpFifoRw(void*, uint8_t* ptr, unsigned int index, size_t len, bool to_guest)
{
    FifoRw(GPOFBASE0, GPIFBASE0, 4, 2, GPFADDR, GPFMAXSGE, ptr, index, len, to_guest);
}
extern "C" void EpFifoRw(void*, uint8_t* ptr, unsigned int index, size_t len, bool to_guest)
{
    // the main output (FIFO 0, to the AC'97's buffers) silent: the voices' mix is what's heard
    static std::vector<u8> silence;
    if (to_guest && index == 0)
    {
        if (silence.size() < len)
            silence.assign(len, 0);
        ptr = silence.data();
    }
    FifoRw(EPOFBASE0, EPIFBASE0, 4, 2, EPFADDR, EPFMAXSGE, ptr, index, len, to_guest);
}

// GP and EP register windows (offsets within their 64 KiB): X memory at 0 (GP 0x1000 words, EP
// 0xc00), the GP's mix buffer at 0x5000 (X:0x1400, 0x400 words), Y at 0x6000 (GP 0x800, EP 0x100),
// P at 0xa000 (0x1000), the reset register at 0xfffc; anything else kept as written.
bool DspWindow(bool gp, u32 offset, char& space, u32& address)
{
    const u32 w = offset / 4;
    if (offset < (gp ? 0x1000u : 0xc00u) * 4)
        return space = 'X', address = w, true;
    if (gp && offset >= 0x5000 && offset < 0x5000 + 0x400 * 4)
        return space = 'X', address = 0x1400 + (offset - 0x5000) / 4, true;
    if (offset >= 0x6000 && offset < 0x6000 + (gp ? 0x800u : 0x100u) * 4)
        return space = 'Y', address = (offset - 0x6000) / 4, true;
    if (offset >= 0xa000 && offset < 0xa000 + 0x1000 * 4)
        return space = 'P', address = (offset - 0xa000) / 4, true;
    return false;
}

u32 DspRead(bool gp, u32 offset)
{
    char space;
    u32 address;
    if (DspWindow(gp, offset, space, address))
        return dsp_read_memory(gp ? g_gp : g_ep, space, address);
    return (gp ? g_gp_regs : g_ep_regs)[offset / 4];
}

void DspWrite(bool gp, u32 offset, u32 value)
{
    char space;
    u32 address;
    if (DspWindow(gp, offset, space, address))
    {
        dsp_write_memory(gp ? g_gp : g_ep, space, address, value & 0xffffff);
        return;
    }
    u32& reg = (gp ? g_gp_regs : g_ep_regs)[offset / 4];
    if (offset == kDspRst)
    {
        DSPState* d = gp ? g_gp : g_ep;
        const bool was = (reg & kRstBits) == kRstBits, now = (value & kRstBits) == kRstBits;
        if (!now)
            dsp_reset(d);
        else if (!was)
        {
            dsp_bootstrap(d);
            static int told = 0;
            if (told++ < 8)
                Log("apu: the %s DSP starts (its microcode bootstrapped)", gp ? "GP" : "EP");
        }
        if (!gp)
            g_ep_frame_div = 0;
    }
    reg = value;
}

float FloatClamp(float v) { return std::clamp(v, -1.f, 1.f); }

// One frame of the DSPs: the voices' front left and right into the GP's mix buffer (bins 0 and 1),
// then 1000 cycles of the GP, and of the EP every 8th frame (xemu's, with its DSP option off).
void DspFrame(const float out[kFrame][2])
{
    for (int c = 0; c < 2; c++)
        for (int s = 0; s < kFrame; s++)
        {
            const double v = double(FloatClamp(out[s][c])) * (8.0 * 0x100000);
            const s32 i = v >= double(0x7fffff) ? 0x7fffff : v <= -8.0 * 0x100000 ? -0x800000 : s32(v);
            dsp_write_memory(g_gp, 'X', u32(0x1400 + c * kFrame + s), u32(i) & 0xffffff);
        }
    auto run = [](DSPState* d) {
        dsp_start_frame(d);
        dsp_set_halt_requested(d, false);
        dsp_set_cycle_count(d, 0);
        dsp_run(d, 1000);
    };
    if ((g_gp_regs[kDspRst / 4] & kRstBits) == kRstBits)
        run(g_gp);
    if ((g_ep_regs[kDspRst / 4] & kRstBits) == kRstBits && g_ep_frame_div % 8 == 0)
        run(g_ep);
    g_ep_frame_div++;
}

// ---- registers

u32 ReadReg(u32 offset, u32)
{
    std::lock_guard lock(g_m);
    if (offset >= 0x20000 && offset < 0x30000) // the front end's method window
        return offset - 0x20000 == 0x10 ? 0x80 : 0; // NV1BA0_PIO_FREE: room for methods
    if (offset >= 0x30000 && offset < 0x40000)
        return DspRead(true, (offset - 0x30000) & ~3u);
    if (offset >= 0x50000 && offset < 0x60000)
        return DspRead(false, (offset - 0x50000) & ~3u);
    if (offset == XGSCNT) // the global sample count
        return u32(g_frames * kFrame);
    return offset < 0x20000 ? Reg(offset & ~3u) : 0;
}

void WriteReg(u32 offset, u32, u32 value)
{
    std::lock_guard lock(g_m);
    if (offset >= 0x20000 && offset < 0x30000)
    {
        Method(offset - 0x20000, value);
        UpdateIrq();
        return;
    }
    if (offset >= 0x30000 && offset < 0x40000)
    {
        DspWrite(true, (offset - 0x30000) & ~3u, value);
        return;
    }
    if (offset >= 0x50000 && offset < 0x60000)
    {
        DspWrite(false, (offset - 0x50000) & ~3u, value);
        return;
    }
    if (offset >= 0x20000)
        return;
    switch (offset)
    {
    case ISTS: Reg(ISTS) &= ~value; UpdateIrq(); return; // write 1 to clear
    case FEMEMDATA: // the notify write: the value to FEMEMADDR
        if (const u32 at = PhysToGuest(Reg(FEMEMADDR)))
            Write<u32>(at, value);
        Reg(FEMEMDATA) = value;
        return;
    }
    Reg(offset & ~3u) = value;
    if (offset == IEN || offset == FECTL)
        UpdateIrq();
}

} // namespace

void InitApu()
{
    for (int l = 0; l < 3; l++)
        g_regs[kListTop[l] / 4] = 0xffff;
    if (!g_gp)
    {
        g_gp = dsp_init(nullptr, GpScratchRw, GpFifoRw, true);
        g_ep = dsp_init(nullptr, EpScratchRw, EpFifoRw, false);
    }
    AddDevice({kBase, kSize, "apu", ReadReg, WriteReg});
    std::thread(Thread).detach();
}

} // namespace xb
