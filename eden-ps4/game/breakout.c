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

#include "nxgame.h"

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
