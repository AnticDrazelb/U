// Shared by the test games: drawing into a libnx framebuffer with the CPU (rectangles and a 5x7
// font) and sound through audout (square-wave beeps and a looped tune, on a thread of their own).
// Include after defining W and H, the framebuffer's size.

#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

static u32* g_fb;
static u32 g_stride; // in pixels

static inline u32 rgb(u32 r, u32 g, u32 b) {
    return RGBA8_MAXALPHA(r, g, b);
}

static void fill(int x, int y, int w, int h, u32 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        u32* row = g_fb + (size_t)(y + j) * g_stride + x;
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
}

// A 5x7 font: digits, capitals and a few signs, one byte per row (low 5 bits).
static const u8* glyph(char c) {
    static const u8 digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},   {14, 17, 1, 2, 4, 8, 31},
        {31, 2, 4, 2, 1, 17, 14},     {2, 6, 10, 18, 31, 2, 2},  {31, 16, 30, 1, 1, 17, 14},
        {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},    {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 2, 12}};
    static const u8 letters[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
        {28, 18, 17, 17, 17, 18, 28}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
        {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
        {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
        {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
        {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
        {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
        {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
        {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31}};
    static const u8 colon[7] = {0, 12, 12, 0, 12, 12, 0};
    static const u8 plus[7] = {0, 4, 4, 31, 4, 4, 0};
    static const u8 bang[7] = {4, 4, 4, 4, 4, 0, 4};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c == ':') return colon;
    if (c == '+') return plus;
    if (c == '!') return bang;
    return NULL;
}

static void text(int x, int y, int scale, u32 color, const char* s) {
    for (; *s; s++, x += 6 * scale) {
        const u8* g = glyph(*s);
        if (!g) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (16 >> col))
                    fill(x + col * scale, y + row * scale, scale, scale, color);
    }
}

static void text_centered(int y, int scale, u32 color, const char* s) {
    text((W - (int)strlen(s) * 6 * scale) / 2, y, scale, color, s);
}

// ---- sound: short square-wave beeps on a thread of their own

#define SAMPLE_RATE 48000
#define AUDIO_FRAMES 1440 // 30 ms
static AudioOutBuffer g_audio_buf[2];
static s16* g_audio_mem[2];
static volatile int g_beep_freq, g_beep_left; // Hz, samples still to play
static bool g_audio_ok;

// Background music: notes of a square wave, quieter than the beeps, looped.
typedef struct {
    u16 freq;   // Hz, 0 for a rest
    u8 eighths; // length
} Note;
static const Note* g_music;
static int g_music_len, g_music_eighth; // notes, samples per eighth
static volatile bool g_music_on;
static volatile bool g_running = true;
static Thread g_audio_thread;

static void beep(int freq, int ms) {
    g_beep_freq = freq;
    g_beep_left = SAMPLE_RATE * ms / 1000;
}

static inline void music_play(const Note* notes, int count, int eighth_ms) {
    g_music_eighth = SAMPLE_RATE * eighth_ms / 1000;
    g_music_len = count;
    g_music = notes;
    g_music_on = true;
}

static void fill_audio(s16* out) {
    static u32 phase, music_phase;
    static int note = -1, note_left, note_length; // the first note is note 0
    for (int i = 0; i < AUDIO_FRAMES; i++) {
        int v = 0;
        if (g_beep_left > 0) {
            phase += (u32)g_beep_freq * 2;
            v = (phase / SAMPLE_RATE) & 1 ? 2400 : -2400;
            g_beep_left--;
        }
        if (g_music && g_music_on) {
            if (note_left <= 0) {
                note = (note + 1) % g_music_len;
                note_length = note_left = g_music[note].eighths * g_music_eighth;
            }
            // the last eighth of each note silent, so that repeated notes are heard as two
            if (g_music[note].freq && note_left > note_length / 8) {
                music_phase += (u32)g_music[note].freq * 2;
                v += (music_phase / SAMPLE_RATE) & 1 ? 900 : -900;
            }
            note_left--;
        }
        out[i * 2] = out[i * 2 + 1] = (s16)v;
    }
}

static void audio_thread(void* arg) {
    (void)arg;
    for (int i = 0; i < 2; i++) {
        fill_audio(g_audio_mem[i]);
        audoutAppendAudioOutBuffer(&g_audio_buf[i]);
    }
    while (g_running) {
        AudioOutBuffer* released = NULL;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ull)) || !released)
            continue;
        fill_audio((s16*)released->buffer);
        audoutAppendAudioOutBuffer(released);
    }
}

static void audio_init(void) {
    const size_t size = (AUDIO_FRAMES * 2 * sizeof(s16) + 0xFFF) & ~0xFFF;
    if (R_FAILED(audoutInitialize()))
        return;
    if (R_FAILED(audoutStartAudioOut())) {
        audoutExit();
        return;
    }
    for (int i = 0; i < 2; i++) {
        g_audio_mem[i] = aligned_alloc(0x1000, size);
        memset(g_audio_mem[i], 0, size);
        g_audio_buf[i] = (AudioOutBuffer){.next = NULL, .buffer = g_audio_mem[i], .buffer_size = size,
                                          .data_size = AUDIO_FRAMES * 2 * sizeof(s16), .data_offset = 0};
    }
    g_audio_ok = R_SUCCEEDED(threadCreate(&g_audio_thread, audio_thread, NULL, NULL, 0x4000, 0x2B, -2)) &&
                 R_SUCCEEDED(threadStart(&g_audio_thread));
}

static void audio_exit(void) {
    g_running = false;
    if (g_audio_ok) {
        threadWaitForExit(&g_audio_thread);
        threadClose(&g_audio_thread);
    }
    audoutStopAudioOut();
    audoutExit();
}
