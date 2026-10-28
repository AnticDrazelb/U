// Blocks: falling blocks, a small Switch homebrew game for testing Eden on the PS4. Drawn with the
// CPU into a libnx framebuffer (no GPU), the first controller, beeps and a tune through audout.
//
//   left/right (D-pad or stick): move    down: drop faster    up or X: drop
//   A: rotate clockwise    B: rotate the other way    -: music on/off    +: quit
//
// The tune is Korobeiniki, a Russian folk song of the 19th century. The best score is kept on the
// SD card (sdmc:/blocks-best.txt).

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

// A fixed heap: without one, libnx asks for all free memory, which Eden then clears.
size_t __nx_heap_size = 64 * 1024 * 1024;

#define W 640
#define H 360
#define COLS 10
#define ROWS 20
#define CELL 16
#define BOARD_X ((W - COLS * CELL) / 2)
#define BOARD_Y 20

#include "nxgame.h"

typedef struct {
    int x, y;
} Cell;

// The seven pieces in their rotation boxes (n x n), and their colours.
static const struct {
    int n;
    Cell cells[4];
    u8 r, g, b;
} kPieces[7] = {
    {4, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, 0, 200, 230},   // I
    {2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, 240, 210, 40},  // O
    {3, {{1, 0}, {0, 1}, {1, 1}, {2, 1}}, 170, 80, 220},  // T
    {3, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}, 80, 210, 90},   // S
    {3, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}, 230, 70, 70},   // Z
    {3, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}, 60, 110, 230},  // J
    {3, {{2, 0}, {0, 1}, {1, 1}, {2, 1}}, 240, 140, 40},  // L
};

typedef struct {
    int type, rot, x, y;
} Piece;

static u8 g_board[ROWS][COLS]; // 0: empty, else the piece type + 1

static void piece_cells(const Piece* p, Cell out[4]) {
    const int n = kPieces[p->type].n;
    for (int i = 0; i < 4; i++) {
        int x = kPieces[p->type].cells[i].x, y = kPieces[p->type].cells[i].y;
        for (int r = 0; r < p->rot; r++) { // clockwise, a quarter turn at a time
            const int t = x;
            x = n - 1 - y;
            y = t;
        }
        out[i] = (Cell){p->x + x, p->y + y};
    }
}

static bool fits(const Piece* p) {
    Cell c[4];
    piece_cells(p, c);
    for (int i = 0; i < 4; i++) {
        if (c[i].x < 0 || c[i].x >= COLS || c[i].y >= ROWS)
            return false;
        if (c[i].y >= 0 && g_board[c[i].y][c[i].x])
            return false;
    }
    return true;
}

// The next piece: each of the seven once in a shuffled bag, then a new bag.
static u32 g_seed;
static int next_type(void) {
    static int bag[7], left;
    if (left == 0) {
        for (int i = 0; i < 7; i++)
            bag[i] = i;
        for (int i = 6; i > 0; i--) {
            g_seed = g_seed * 1103515245u + 12345u;
            const int j = (int)((g_seed >> 16) % (u32)(i + 1));
            const int t = bag[i];
            bag[i] = bag[j];
            bag[j] = t;
        }
        left = 7;
    }
    return bag[--left];
}

static void draw_cell(int x, int y, u8 r, u8 g, u8 b, bool ghost) {
    if (ghost) {
        const u32 c = rgb(r / 3, g / 3, b / 3);
        fill(x, y, CELL, 2, c);
        fill(x, y + CELL - 2, CELL, 2, c);
        fill(x, y, 2, CELL, c);
        fill(x + CELL - 2, y, 2, CELL, c);
        return;
    }
    fill(x, y, CELL, CELL, rgb(r * 2 / 3, g * 2 / 3, b * 2 / 3));
    fill(x + 1, y + 1, CELL - 3, CELL - 3, rgb(r, g, b));
    fill(x + 3, y + 3, 4, 2, rgb(255, 255, 255));
}

static const Note kTune[] = {
    {659, 2}, {494, 1}, {523, 1}, {587, 2}, {523, 1}, {494, 1}, {440, 2}, {440, 1}, {523, 1}, {659, 2},
    {587, 1}, {523, 1}, {494, 3}, {523, 1}, {587, 2}, {659, 2}, {523, 2}, {440, 2}, {440, 4},
    {0, 1},   {587, 2}, {698, 1}, {880, 2}, {784, 1}, {698, 1}, {659, 3}, {523, 1}, {659, 2}, {587, 1},
    {523, 1}, {494, 2}, {494, 1}, {523, 1}, {587, 2}, {659, 2}, {523, 2}, {440, 2}, {440, 2}, {0, 2},
};

// The best score, on the SD card (Eden's: its data folder's sdmc).
#define BEST_PATH "sdmc:/blocks-best.txt"
static int load_best(void) {
    int best = 0;
    FILE* f = fopen(BEST_PATH, "r");
    if (f) {
        if (fscanf(f, "%d", &best) != 1)
            best = 0;
        fclose(f);
    }
    return best;
}
static void save_best(int best) {
    FILE* f = fopen(BEST_PATH, "w");
    if (!f) {
        fprintf(stderr, "blocks: can't write %s\n", BEST_PATH);
        return;
    }
    fprintf(f, "%d\n", best);
    fclose(f);
    fprintf(stderr, "blocks: best score %d saved\n", best);
}

typedef enum { Playing, Over } State;

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
    music_play(kTune, sizeof(kTune) / sizeof(kTune[0]), 180);
    const int best_at_start = load_best();
    fprintf(stderr, "blocks: started, audio %s, best score %d\n", g_audio_ok ? "on" : "off", best_at_start);

    const double tick_freq = (double)armGetSystemTickFreq();
    g_seed = (u32)armGetSystemTick();
    State state = Playing;
    int score = 0, lines = 0, level = 0, next = next_type(), best = best_at_start;
    Piece cur = {0};
    double fall_timer = 0, repeat_timer = 0;
    int repeat_dir = 0;
    u64 last = armGetSystemTick(), fps_tick = last, fps_frames = 0;
    int fps = 0;

    memset(g_board, 0, sizeof(g_board));
#define SPAWN()                                                                                    \
    do {                                                                                           \
        cur = (Piece){next, 0, (COLS - kPieces[next].n) / 2, 0};                                   \
        next = next_type();                                                                        \
        if (!fits(&cur)) {                                                                         \
            state = Over;                                                                          \
            beep(110, 600);                                                                        \
            fprintf(stderr, "blocks: game over, score %d, lines %d\n", score, lines);              \
            if (score > best)                                                                      \
                save_best(best = score);                                                           \
        }                                                                                          \
    } while (0)
    SPAWN();

    while (appletMainLoop()) {
        const u64 now = armGetSystemTick();
        const double dt = (double)(now - last) / tick_freq;
        last = now;

        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const u64 held = padGetButtons(&pad);
        if (down & HidNpadButton_Plus)
            break;
        if (down & HidNpadButton_Minus)
            g_music_on = !g_music_on;
        const HidAnalogStickState stick = padGetStickPos(&pad, 0);
        const bool left = (held & HidNpadButton_AnyLeft) || stick.x < -JOYSTICK_MAX / 2;
        const bool right = (held & HidNpadButton_AnyRight) || stick.x > JOYSTICK_MAX / 2;
        const bool soft = (held & HidNpadButton_AnyDown) || stick.y < -JOYSTICK_MAX / 2;

        if (state == Over) {
            if (down & HidNpadButton_A) {
                memset(g_board, 0, sizeof(g_board));
                score = lines = level = 0;
                state = Playing;
                SPAWN();
            }
        } else {
            // sideways: a step on the press, then repeated while held
            const int dir = left ? -1 : right ? 1 : 0;
            int step = 0;
            if (dir != repeat_dir) {
                repeat_dir = dir;
                repeat_timer = 0.17;
                step = dir;
            } else if (dir) {
                repeat_timer -= dt;
                if (repeat_timer <= 0) {
                    repeat_timer += 0.05;
                    step = dir;
                }
            }
            if (step) {
                Piece moved = cur;
                moved.x += step;
                if (fits(&moved))
                    cur = moved;
            }
            // rotation, with a nudge sideways when it touches a wall or another block
            if (down & (HidNpadButton_A | HidNpadButton_B)) {
                Piece turned = cur;
                turned.rot = (cur.rot + ((down & HidNpadButton_A) ? 1 : 3)) % 4;
                static const int kicks[] = {0, -1, 1, -2, 2};
                for (int k = 0; k < 5; k++) {
                    Piece kicked = turned;
                    kicked.x += kicks[k];
                    if (fits(&kicked)) {
                        cur = kicked;
                        beep(880, 15);
                        break;
                    }
                }
            }
            // falling: a drop to the bottom, or a row at a time
            bool lock = false;
            if (down & (HidNpadButton_AnyUp | HidNpadButton_X)) {
                Piece dropped = cur;
                while (dropped.y++, fits(&dropped))
                    cur.y = dropped.y;
                score += 2;
                lock = true;
            } else {
                double interval = 0.8 - level * 0.07;
                if (interval < 0.08)
                    interval = 0.08;
                if (soft)
                    interval = 0.04;
                fall_timer += dt;
                while (fall_timer >= interval && !lock) {
                    fall_timer -= interval;
                    Piece moved = cur;
                    moved.y++;
                    if (fits(&moved)) {
                        cur = moved;
                        if (soft)
                            score++;
                    } else {
                        lock = true;
                    }
                }
            }
            if (lock) {
                fall_timer = 0;
                Cell c[4];
                piece_cells(&cur, c);
                for (int i = 0; i < 4; i++)
                    if (c[i].y >= 0)
                        g_board[c[i].y][c[i].x] = (u8)(cur.type + 1);
                // full rows go, the ones above come down
                int cleared = 0;
                for (int y = ROWS - 1; y >= 0; y--) {
                    bool full = true;
                    for (int x = 0; x < COLS; x++)
                        full = full && g_board[y][x];
                    if (!full)
                        continue;
                    memmove(&g_board[1], &g_board[0], (size_t)y * COLS);
                    memset(g_board[0], 0, COLS);
                    cleared++;
                    y++;
                }
                static const int kPoints[5] = {0, 100, 300, 500, 800};
                score += kPoints[cleared] * (level + 1);
                lines += cleared;
                level = lines / 10;
                if (cleared == 4)
                    beep(1320, 250);
                else if (cleared)
                    beep(660 + 220 * cleared, 120);
                else
                    beep(160, 30);
                SPAWN();
            }
        }

        // drawing
        g_fb = (u32*)framebufferBegin(&fb, &g_stride);
        g_stride /= sizeof(u32);
        fill(0, 0, W, H, rgb(16, 20, 28));
        fill(BOARD_X - 4, BOARD_Y - 4, COLS * CELL + 8, ROWS * CELL + 8, rgb(70, 90, 120));
        fill(BOARD_X, BOARD_Y, COLS * CELL, ROWS * CELL, rgb(8, 10, 16));
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++)
                if (g_board[y][x]) {
                    const int t = g_board[y][x] - 1;
                    draw_cell(BOARD_X + x * CELL, BOARD_Y + y * CELL, kPieces[t].r, kPieces[t].g, kPieces[t].b,
                              false);
                }
        if (state == Playing) {
            Piece ghost = cur;
            Piece below = cur;
            while (below.y++, fits(&below))
                ghost.y = below.y;
            Cell c[4];
            piece_cells(&ghost, c);
            for (int i = 0; i < 4; i++)
                if (c[i].y >= 0)
                    draw_cell(BOARD_X + c[i].x * CELL, BOARD_Y + c[i].y * CELL, kPieces[cur.type].r,
                              kPieces[cur.type].g, kPieces[cur.type].b, true);
            piece_cells(&cur, c);
            for (int i = 0; i < 4; i++)
                if (c[i].y >= 0)
                    draw_cell(BOARD_X + c[i].x * CELL, BOARD_Y + c[i].y * CELL, kPieces[cur.type].r,
                              kPieces[cur.type].g, kPieces[cur.type].b, false);
        }

        // the side panels: next piece on the right, score on the left
        const int panel_r = BOARD_X + COLS * CELL + 24, panel_l = 24;
        text(panel_r, BOARD_Y, 2, rgb(150, 170, 200), "NEXT");
        {
            const Piece preview = {next, 0, 0, 0};
            Cell c[4];
            piece_cells(&preview, c);
            for (int i = 0; i < 4; i++)
                draw_cell(panel_r + c[i].x * CELL, BOARD_Y + 28 + c[i].y * CELL, kPieces[next].r, kPieces[next].g,
                          kPieces[next].b, false);
        }
        char line[48];
        text(panel_l, BOARD_Y, 2, rgb(150, 170, 200), "SCORE");
        snprintf(line, sizeof(line), "%d", score);
        text(panel_l, BOARD_Y + 20, 3, rgb(240, 240, 240), line);
        text(panel_l, BOARD_Y + 64, 2, rgb(150, 170, 200), "LINES");
        snprintf(line, sizeof(line), "%d", lines);
        text(panel_l, BOARD_Y + 84, 3, rgb(240, 240, 240), line);
        text(panel_l, BOARD_Y + 128, 2, rgb(150, 170, 200), "LEVEL");
        snprintf(line, sizeof(line), "%d", level);
        text(panel_l, BOARD_Y + 148, 3, rgb(240, 240, 240), line);
        text(panel_l, BOARD_Y + 192, 2, rgb(150, 170, 200), "BEST");
        snprintf(line, sizeof(line), "%d", best > score ? best : score);
        text(panel_l, BOARD_Y + 212, 3, rgb(240, 240, 240), line);
        snprintf(line, sizeof(line), "%d FPS", fps);
        text(panel_l, H - 24, 2, rgb(90, 100, 120), line);
        text(panel_r, H - 64, 1, rgb(110, 120, 140), "A B: ROTATE");
        text(panel_r, H - 52, 1, rgb(110, 120, 140), "UP X: DROP");
        text(panel_r, H - 40, 1, rgb(110, 120, 140), "MINUS: MUSIC");
        text(panel_r, H - 28, 1, rgb(110, 120, 140), "PLUS: QUIT");
        if (state == Over) {
            fill(BOARD_X - 30, H / 2 - 34, COLS * CELL + 60, 68, rgb(16, 20, 28));
            text_centered(H / 2 - 24, 3, rgb(255, 107, 107), "GAME OVER");
            text_centered(H / 2 + 10, 2, rgb(242, 204, 96), "PRESS A");
        }
        framebufferEnd(&fb);

        fps_frames++;
        if (now - fps_tick >= armGetSystemTickFreq()) {
            fps = (int)fps_frames;
            fps_frames = 0;
            fps_tick = now;
        }
    }

    if (score > best)
        save_best(score);
    audio_exit();
    framebufferClose(&fb);
    fprintf(stderr, "blocks: quit, score %d, lines %d\n", score, lines);
    return 0;
}
