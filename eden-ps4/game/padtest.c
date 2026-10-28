// A controller test for Eden for PS4: shows, live, which of the Switch controller's buttons are
// held (with the PS4 button the launcher maps to each) and where the sticks are. Text through
// libnx's console, which draws into a framebuffer with the CPU. Hold PLUS (OPTIONS) and MINUS (the
// touchpad's click) together to quit.

#include <stdio.h>
#include <switch.h>

// A heap of 64 MiB, not libnx's default of all the memory there is (about 3.2 GiB, which the
// emulated kernel would have to clear first).
size_t __nx_heap_size = 64 * 1024 * 1024;

static const struct {
    u64 bit;
    const char* name;
    const char* ps4;
} kButtons[] = {
    {HidNpadButton_A, "A", "CROSS"},           {HidNpadButton_B, "B", "CIRCLE"},
    {HidNpadButton_X, "X", "TRIANGLE"},        {HidNpadButton_Y, "Y", "SQUARE"},
    {HidNpadButton_L, "L", "L1"},              {HidNpadButton_R, "R", "R1"},
    {HidNpadButton_ZL, "ZL", "L2"},            {HidNpadButton_ZR, "ZR", "R2"},
    {HidNpadButton_Plus, "PLUS", "OPTIONS"},   {HidNpadButton_Minus, "MINUS", "TOUCHPAD"},
    {HidNpadButton_StickL, "L STICK", "L3"},   {HidNpadButton_StickR, "R STICK", "R3"},
    {HidNpadButton_Up, "UP", "UP"},            {HidNpadButton_Down, "DOWN", "DOWN"},
    {HidNpadButton_Left, "LEFT", "LEFT"},      {HidNpadButton_Right, "RIGHT", "RIGHT"},
};

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    u64 frames = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 held = padGetButtons(&pad);
        if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus))
            break;
        const HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);

        // \x1b[H: back to the top left, \x1b[K: clear to the end of the line
        printf("\x1b[H");
        printf("Controller test (Eden for PS4)\x1b[K\n");
        printf("Hold OPTIONS and the touchpad's click together to quit.\x1b[K\n\n");
        printf("  Switch      PS4\x1b[K\n");
        for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
            const int on = (held & kButtons[i].bit) != 0;
            printf("%s %-10s  %-10s %s\x1b[0m\x1b[K\n", on ? "\x1b[32m>" : " ", kButtons[i].name, kButtons[i].ps4,
                   on ? "held" : "");
        }
        printf("\x1b[K\n");
        printf("  left stick   %6d %6d\x1b[K\n", l.x, l.y);
        printf("  right stick  %6d %6d\x1b[K\n", r.x, r.y);
        printf("\x1b[K\n  frame %llu\x1b[K\n", (unsigned long long)++frames);
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
