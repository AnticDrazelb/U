// The MCPX APU driven as DirectSound drives it: the voice array, SGE pages and notifiers set up,
// voices configured through the front end's methods. Heard (xbox-core --wav, then check.py):
//   voice 100: a 1 kHz sine (amplitude 0.5), 16-bit, looped, full volume
//   voice 101: the same buffer an octave down (pitch -4096: 500 Hz) at -6 dB (amplitude 0.25)
//   voice 103: a 2 kHz sine (amplitude 0.25) in IMA ADPCM, encoded here, looped
//   voice 104: a 3 kHz sine (amplitude 0.25) streamed: two segments in each of SSL A and B
// Checked here (DbgPrint PASS/FAIL): the global sample count runs at 48 kHz; voice 102, a short
// buffer that doesn't loop, ends and writes its notifier.
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define APU(offset) (*(volatile DWORD *)(0xFE800000 + (offset)))
#define METHOD(m, v) (APU(0x20000 + (m)) = (v))

static int failures = 0;
static void check(const char *what, int ok)
{
    DbgPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    debugPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static void *contiguous(DWORD size)
{
    void *p = MmAllocateContiguousMemoryEx(size, 0, 0x3ffffff, 4096, PAGE_READWRITE);
    memset(p, 0, size);
    return p;
}

// IMA ADPCM, as the Xbox stores it (36-byte blocks: a header, then 8 words of 8 codes; 64 samples
// heard a block): the encoder's side of the APU's decoder
static const int steps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int index_change[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static void encode_adpcm(const short *in, int blocks, unsigned char *out)
{
    int index = 0;
    for (int b = 0; b < blocks; b++, out += 36)
    {
        const short *x = in + b * 64;
        int predictor = x[0];
        out[0] = predictor & 0xff, out[1] = (predictor >> 8) & 0xff, out[2] = index, out[3] = 0;
        for (int n = 0; n < 64; n++)
        {
            const int target = x[n + 1];
            const int step = steps[index];
            int diff = target - predictor, code = 0;
            if (diff < 0) code = 8, diff = -diff;
            int delta = step >> 3;
            if (diff >= step) code |= 4, diff -= step, delta += step;
            if (diff >= step >> 1) code |= 2, diff -= step >> 1, delta += step >> 1;
            if (diff >= step >> 2) code |= 1, delta += step >> 2;
            predictor += code & 8 ? -delta : delta;
            if (predictor > 32767) predictor = 32767;
            if (predictor < -32768) predictor = -32768;
            index += index_change[code & 7];
            if (index < 0) index = 0;
            if (index > 88) index = 88;
            unsigned char *byte = out + 4 + n / 2;
            *byte = n & 1 ? (*byte | code << 4) : code;
        }
    }
}

static void sine_table(short *out, int n, float hz, float amplitude)
{
    for (int i = 0; i < n; i++)
    {
        float s = 32768.0f * amplitude * sinf(2.0f * 3.14159265f * hz * i / 48000.0f);
        out[i] = (short)(s < 0 ? s - 0.5f : s + 0.5f);
    }
}

static void voice(int v, int pitch, int volume, DWORD end, int loop)
{
    METHOD(0x2f8, v);                                   // SET_CURRENT_VOICE
    METHOD(0x300, 0 | 1 << 5 | 31 << 10 | 31 << 16 | 31 << 21 | 31 << 26); // bins 0, 1 (the rest 31)
    METHOD(0x304, 1u << 28 | 1u << 30 | (loop ? 1u << 25 : 0) | 31 | 31 << 5); // S16 in B16, mono
    // volumes 0 and 1 `volume`, the others silent (0xfff)
    METHOD(0x360, (DWORD)volume << 4 | (DWORD)volume << 20 | 0x000f000f);
    METHOD(0x364, 0xffffffff);
    METHOD(0x368, 0xffffffff);
    METHOD(0x37c, (DWORD)(pitch & 0xffff) << 16);       // TAR_PITCH
    METHOD(0x3a0, 0);                                   // buffer base
    METHOD(0x3a4, 0);                                   // loop start
    METHOD(0x3dc, end);                                 // end
    METHOD(0x3d8, 0);                                   // current
    METHOD(0x120, 1 << 16);                             // antecedent: the 2D list's top
    METHOD(0x124, v);                                   // VOICE_ON, envelopes off
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    // a second of a 1 kHz sine, amplitude 0.5, in 4 KiB pages listed by the SGE table
    const int samples = 48000;
    short *sine = contiguous(samples * 2);
    for (int i = 0; i < samples; i++)
        { float s = 16384.0f * sinf(2.0f * 3.14159265f * 1000.0f * i / 48000.0f); sine[i] = (short)(s < 0 ? s - 0.5f : s + 0.5f); }
    DWORD *sge = contiguous(4096);
    for (int page = 0; page < (samples * 2 + 4095) / 4096; page++)
        sge[page * 2] = MmGetPhysicalAddress((char *)sine + page * 4096);
    void *voices = contiguous(256 * 0x80);
    unsigned char *notifiers = contiguous(16 * (2 + 256 * 4));

    APU(0x202c) = MmGetPhysicalAddress(voices);    // VPVADDR
    APU(0x2030) = MmGetPhysicalAddress(sge);       // VPSGEADDR
    APU(0x115c) = MmGetPhysicalAddress(notifiers); // FENADDR
    APU(0x2054) = APU(0x2060) = APU(0x206c) = 0xffff; // the voice lists, empty
    APU(0x2000) = 1 << 3;                          // SECTL: the sample counter running

    DWORD start = APU(0x200c);
    Sleep(100);
    DWORD counted = APU(0x200c) - start;
    DbgPrint("  100 ms: %u samples counted\n", (unsigned)counted);
    check("the global sample count runs at 48 kHz", counted > 3800 && counted < 6000);

    voice(100, 0, 0, samples - 1, 1);          // 1 kHz, full volume
    voice(101, -4096, 6 * 64, samples - 1, 1); // 500 Hz, -6 dB
    voice(102, 0, 0xfff, 4799, 0);             // a tenth of a second, silent, once

    // voice 103: ADPCM, a second of 2 kHz (750 blocks), its pages after the sine's in the SGE table
    short *pcm = contiguous((48000 + 1) * 2);
    sine_table(pcm, 48001, 2000.0f, 0.25f);
    unsigned char *adpcm = contiguous(750 * 36);
    encode_adpcm(pcm, 750, adpcm);
    const int first_page = (samples * 2 + 4095) / 4096;
    for (int page = 0; page < (750 * 36 + 4095) / 4096; page++)
        sge[(first_page + page) * 2] = MmGetPhysicalAddress(adpcm + page * 4096);
    voice(103, 0, 0, 750 * 64 - 1, 1);
    METHOD(0x304, 1u << 28 | 2u << 30 | 1u << 25 | 31 | 31 << 5); // ADPCM container, looped
    METHOD(0x3a0, first_page * 4096);                             // its base
    METHOD(0x3d8, 0);

    // voice 104: a stream; 3 kHz in a tenth of a second (4800 samples), two segments in each SSL
    short *tone = contiguous(4800 * 2);
    sine_table(tone, 4800, 3000.0f, 0.25f);
    DWORD *ssl = contiguous(4096);
    APU(0x2034) = MmGetPhysicalAddress(ssl);    // VPSSLADDR
    METHOD(0x190, 0x40);                        // SET_CURRENT_SSL: entries from 0x40
    for (int i = 0; i < 4; i++)
    {
        METHOD(0x600 + i * 8, MmGetPhysicalAddress(tone)); // segment offset
        METHOD(0x604 + i * 8, 4800 | 1 << 16);              // length, B16, mono
    }
    METHOD(0x2f8, 104);
    METHOD(0x300, 0 | 1 << 5 | 31 << 10 | 31 << 16 | 31 << 21 | 31 << 26);
    METHOD(0x304, 1u << 28 | 1u << 30 | 1u << 24 | 1u << 23 | 31 | 31 << 5); // S16, stream, persist
    METHOD(0x360, 0x000f000f);
    METHOD(0x364, 0xffffffff);
    METHOD(0x368, 0xffffffff);
    METHOD(0x37c, 0);
    METHOD(0x320, 0x40 << 8 | 2);               // SSL A: entries 0x40, 0x41
    METHOD(0x35c, 0x42 << 8 | 2);               // SSL B: entries 0x42, 0x43
    METHOD(0x120, 1 << 16);
    METHOD(0x124, 104);

    Sleep(400);
    // voice 102's notifier: the first of its four, 16 bytes each after two
    const unsigned char *n = notifiers + 16 * (2 + 102 * 4);
    check("a finished voice writes its notifier", n[15] == 1);
    // voice 104's SSL A notifier (its first): done once A has played
    const unsigned char *a = notifiers + 16 * (2 + 104 * 4);
    check("a stream's SSL A notifies when it has played", a[15] == 1);

    DbgPrint("APU test: %d failures\n", failures);
    debugPrint("APU test: %d failures\n", failures);
    for (;;)
        Sleep(1000);
}
