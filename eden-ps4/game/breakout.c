// Breakout: a small Switch homebrew game for testing Eden on the PS4. It draws with the CPU into
// a libnx framebuffer (no GPU), reads the first controller, and beeps through audout.
//
//   left stick or D-pad: move the paddle    A: launch the ball    +: quit
//
// The framebuffer is 640x360 (the screen scales it up): a quarter of the pixels of 720p, as every
// pixel is drawn by guest code that the PS4 runs at a fraction of a Switch's speed.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

// A fixed heap: without one, libnx asks for all free memory, which Eden then clears page by page.
size_t __nx_heap_size = 64 * 1024 * 1024;

#define W 640
#define H 360
#define BRICK_COLS 10
#define BRICK_ROWS 6
#define BRICK_W 56
#define BRICK_H 14
#define BRICK_GAP 4
#define BRICKS_TOP 52
#define BRICKS_LEFT ((W - BRICK_COLS * (BRICK_W + BRICK_GAP) + BRICK_GAP) / 2)
#define PADDLE_W 84
#define PADDLE_H 8
#define PADDLE_Y (H - 28)
#define BALL 6

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
static volatile bool g_running = true;
static Thread g_audio_thread;

static void beep(int freq, int ms) {
    g_beep_freq = freq;
    g_beep_left = SAMPLE_RATE * ms / 1000;
}

static void fill_audio(s16* out) {
    static u32 phase;
    for (int i = 0; i < AUDIO_FRAMES; i++) {
        s16 v = 0;
        if (g_beep_left > 0) {
            phase += (u32)g_beep_freq * 2;
            v = (phase / SAMPLE_RATE) & 1 ? 2400 : -2400;
            g_beep_left--;
        }
        out[i * 2] = out[i * 2 + 1] = v;
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

// ---- the game

typedef enum { Ready, Playing, Won, Lost } State;

static bool g_bricks[BRICK_ROWS][BRICK_COLS];
static const u32 g_row_colors[BRICK_ROWS][3] = {
    {231, 76, 60}, {230, 126, 34}, {241, 196, 15}, {46, 204, 113}, {52, 152, 219}, {155, 89, 182}};

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    consoleDebugInit(debugDevice_SVC); // stderr goes to svcOutputDebugString (the launcher's log)
    NWindow* win = nwindowGetDefault();
    Framebuffer fb;
    framebufferCreate(&fb, win, W, H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    audio_init();
    fprintf(stderr, "breakout: started, audio %s\n", g_audio_ok ? "on" : "off");

    State state = Ready;
    int score = 0, lives = 3, bricks_left = 0;
    float paddle = (W - PADDLE_W) / 2.0f, bx = 0, by = 0, vx = 0, vy = 0;
    u64 frames = 0, last_tick = armGetSystemTick(), fps_frames = 0;
    int fps = 0;

    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++)
            g_bricks[r][c] = true;
    bricks_left = BRICK_ROWS * BRICK_COLS;

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const u64 held = padGetButtons(&pad);
        if (down & HidNpadButton_Plus)
            break;

        // paddle: stick or D-pad
        const HidAnalogStickState stick = padGetStickPos(&pad, 0);
        float move = (float)stick.x / JOYSTICK_MAX * 9.0f;
        if (held & HidNpadButton_AnyLeft) move = -7.0f;
        if (held & HidNpadButton_AnyRight) move = 7.0f;
        paddle += move;
        if (paddle < 0) paddle = 0;
        if (paddle > W - PADDLE_W) paddle = W - PADDLE_W;

        if (state != Playing && (down & HidNpadButton_A)) {
            if (state != Ready) { // a new game
                for (int r = 0; r < BRICK_ROWS; r++)
                    for (int c = 0; c < BRICK_COLS; c++)
                        g_bricks[r][c] = true;
                bricks_left = BRICK_ROWS * BRICK_COLS;
                score = 0;
                lives = 3;
            }
            state = Playing;
            bx = paddle + PADDLE_W / 2.0f - BALL / 2.0f;
            by = PADDLE_Y - BALL - 1;
            vx = 3.2f;
            vy = -4.2f;
            beep(660, 60);
            fprintf(stderr, "breakout: ball launched\n");
        }

        if (state == Playing) {
            bx += vx;
            by += vy;
            if (bx < 0) { bx = 0; vx = -vx; beep(330, 20); }
            if (bx > W - BALL) { bx = W - BALL; vx = -vx; beep(330, 20); }
            if (by < 24) { by = 24; vy = -vy; beep(330, 20); }
            // paddle: the bounce angle depends on where the ball hits it
            if (vy > 0 && by + BALL >= PADDLE_Y && by + BALL <= PADDLE_Y + PADDLE_H + 6 &&
                bx + BALL >= paddle && bx <= paddle + PADDLE_W) {
                const float hit = (bx + BALL / 2.0f - paddle) / PADDLE_W - 0.5f; // -0.5..0.5
                const float speed = sqrtf(vx * vx + vy * vy) * 1.01f;
                vx = speed * hit * 1.6f;
                vy = -sqrtf(speed * speed - vx * vx);
                by = PADDLE_Y - BALL;
                beep(440, 30);
            }
            // bricks
            const float cx = bx + BALL / 2.0f - BRICKS_LEFT, cy = by + BALL / 2.0f - BRICKS_TOP;
            const int col = (int)(cx / (BRICK_W + BRICK_GAP));
            const int row = (int)(cy / (BRICK_H + BRICK_GAP));
            if (cx >= 0 && cy >= 0 && row >= 0 && row < BRICK_ROWS && col >= 0 &&
                col < BRICK_COLS && g_bricks[row][col]) {
                g_bricks[row][col] = false;
                bricks_left--;
                score += 10 * (BRICK_ROWS - row);
                vy = -vy;
                beep(880 + 110 * (BRICK_ROWS - row), 40);
                if (bricks_left == 0) {
                    state = Won;
                    beep(1320, 300);
                    fprintf(stderr, "breakout: all bricks cleared, score %d\n", score);
                }
            }
            if (by > H) {
                lives--;
                beep(160, 250);
                state = lives > 0 ? Ready : Lost;
                if (state == Lost)
                    fprintf(stderr, "breakout: game over, score %d\n", score);
            }
        }
        if (state == Ready) { // the ball waits on the paddle
            bx = paddle + PADDLE_W / 2.0f - BALL / 2.0f;
            by = PADDLE_Y - BALL - 1;
        }

        // draw
        g_fb = (u32*)framebufferBegin(&fb, &g_stride);
        g_stride /= sizeof(u32);
        fill(0, 0, W, H, rgb(16, 20, 28));
        fill(0, 22, W, 2, rgb(58, 142, 230));
        char line[64];
        snprintf(line, sizeof(line), "SCORE %d", score);
        text(8, 6, 2, rgb(230, 230, 230), line);
        snprintf(line, sizeof(line), "LIVES %d", lives);
        text(W - 8 - (int)strlen(line) * 12, 6, 2, rgb(230, 230, 230), line);
        snprintf(line, sizeof(line), "%d FPS", fps);
        text((W - (int)strlen(line) * 12) / 2, 6, 2, rgb(120, 130, 150), line);
        for (int r = 0; r < BRICK_ROWS; r++)
            for (int c = 0; c < BRICK_COLS; c++)
                if (g_bricks[r][c])
                    fill(BRICKS_LEFT + c * (BRICK_W + BRICK_GAP), BRICKS_TOP + r * (BRICK_H + BRICK_GAP),
                         BRICK_W, BRICK_H, rgb(g_row_colors[r][0], g_row_colors[r][1], g_row_colors[r][2]));
        fill((int)paddle, PADDLE_Y, PADDLE_W, PADDLE_H, rgb(236, 240, 241));
        if (state == Playing || state == Ready)
            fill((int)bx, (int)by, BALL, BALL, rgb(255, 255, 255));
        if (state == Ready)
            text_centered(H / 2 + 20, 2, rgb(242, 204, 96), "PRESS A");
        if (state == Won)
            text_centered(H / 2, 3, rgb(126, 231, 135), "YOU WIN! PRESS A");
        if (state == Lost)
            text_centered(H / 2, 3, rgb(255, 107, 107), "GAME OVER PRESS A");
        framebufferEnd(&fb);

        frames++;
        fps_frames++;
        const u64 now = armGetSystemTick();
        if (now - last_tick >= armGetSystemTickFreq()) {
            fps = (int)fps_frames;
            fps_frames = 0;
            last_tick = now;
            if (frames % 600 < 60)
                fprintf(stderr, "breakout: %d fps\n", fps);
        }
    }

    audio_exit();
    framebufferClose(&fb);
    fprintf(stderr, "breakout: quit\n");
    return 0;
}
