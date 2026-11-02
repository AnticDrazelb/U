# big.py <functions> <seed> > big.c: game-like C with a large code footprint (04.07: how much of the
# translated code's instruction cache misses the rarely run code costs). A frame calls every
# function once through a table, as a game's update and render code does.
# (04.07: python3 big.py 1500 7 > big1500.c, built with build.sh's flags as game.c is)
import random, sys
n, seed = int(sys.argv[1]), int(sys.argv[2])
rng = random.Random(seed)
print("typedef struct { int a, b, c, d; float x, y, z, w; int arr[8]; unsigned char flags[4]; } Obj;")
print("static Obj objs[64]; static int g[512]; static float fg[128];")
def stmt(d):
    k = rng.random(); v = rng.randrange(1, 9)
    o = f"objs[(s + {rng.randrange(64)}) & 63]"
    if k < 0.18 and d < 2:
        return f"for (int i = 0; i < {rng.randrange(2, 6)}; i++) {{ {stmt(d+1)} {stmt(d+1)} }}"
    if k < 0.34 and d < 2:
        return f"if (({o}.a & {v}) != 0) {{ {stmt(d+1)} }} else {{ {stmt(d+1)} }}"
    if k < 0.45:
        return f"{o}.x = {o}.x * {rng.random():.3f}f + {o}.y; if ({o}.x > {rng.randrange(1,50)}.0f) s += {v};"
    if k < 0.55:
        return f"s += {o}.arr[(s >> {rng.randrange(1,5)}) & 7] ^ {o}.b;"
    if k < 0.65:
        return f"{o}.flags[{rng.randrange(4)}] = (unsigned char)(s + {v}); s ^= {o}.flags[{rng.randrange(4)}];"
    if k < 0.75:
        return f"g[(s + {rng.randrange(512)}) & 511] += s >> {rng.randrange(1,8)};"
    if k < 0.85:
        return f"switch (s & 3) {{ case 0: s += {v}; break; case 1: s -= g[{rng.randrange(512)}]; break; case 2: s = s * {v} + 1; break; default: s ^= {o}.c; }}"
    if k < 0.93:
        return f"fg[{rng.randrange(128)}] += (float)(s & 255) * {rng.random():.3f}f; s += (int)fg[{rng.randrange(128)}];"
    return f"{o}.d = {o}.d < s ? {o}.d + {v} : s - {v};"
for i in range(n):
    body = " ".join(stmt(0) for _ in range(rng.randrange(3, 8)))
    print(f"__attribute__((noinline)) static int f{i}(int s) {{ {body} return s; }}")
print("typedef int (*Fn)(int); static Fn const table[] = {" + ", ".join(f"f{i}" for i in range(n)) + "};")
print(f"int run(int frames) {{ int s = 1; for (int f = 0; f < frames; f++) for (int i = 0; i < {n}; i++) s = table[i](s); return s; }}")
