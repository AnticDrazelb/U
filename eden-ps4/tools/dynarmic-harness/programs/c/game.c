// A frame of game-like work compiled to 32-bit ARM, as a Switch game's main thread runs it:
// entities with a state machine and bounds checks, a broad-phase pair test, a sort, string hashing,
// a tree walk, small copies. image mode: r0 = frames; the result in r0.
#include <stdint.h>
#include <stddef.h>

#define N 96
typedef struct { float x, y, z, vx, vy, vz; int state, timer, flags, id; } Entity;
typedef struct Node { struct Node* l; struct Node* r; int key; int value; } Node;

static Entity ents[N];
static int order[N];
static Node nodes[64];
static char names[N][16];
static uint32_t seed = 12345;

void* memcpy(void* d, const void* s, size_t n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, size_t n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)c; return d; }

static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

__attribute__((noinline)) static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
__attribute__((noinline)) static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

__attribute__((noinline)) static void update(Entity* e) {
    switch (e->state) {
    case 0: if (++e->timer > 20) { e->state = 1; e->timer = 0; } break;
    case 1: e->vx += 0.1f; if (e->x > 50.0f) e->state = 2; break;
    case 2: e->vx -= 0.2f; if (e->vx < -1.0f) { e->state = 3; e->flags |= 4; } break;
    default: if (e->flags & 4) { e->flags &= ~4; e->state = 0; } else e->state = 1; break;
    }
    e->x += e->vx; e->y += e->vy; e->z += e->vz;
    e->x = clampf(e->x, -100.0f, 100.0f);
    e->timer = clampi(e->timer, 0, 1000);
    if (e->y < 0.0f) { e->y = 0.0f; e->vy = -e->vy * 0.5f; }
}

__attribute__((noinline)) static int overlap(const Entity* a, const Entity* b) {
    float dx = a->x - b->x, dz = a->z - b->z;
    if (dx < -2.0f || dx > 2.0f) return 0;
    if (dz < -2.0f || dz > 2.0f) return 0;
    return (a->flags & b->flags & 1) == 0;
}

__attribute__((noinline)) static void sort(int* a, int n) {
    for (int i = 1; i < n; i++) {
        int v = a[i], j = i - 1;
        while (j >= 0 && ents[a[j]].x > ents[v].x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

__attribute__((noinline)) static uint32_t hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

__attribute__((noinline)) static int find(const Node* n, int key) {
    while (n) {
        if (key == n->key) return n->value;
        n = key < n->key ? n->l : n->r;
    }
    return -1;
}

static Node* insert(Node* root, Node* n) {
    if (!root) return n;
    Node* p = root;
    for (;;) {
        Node** next = n->key < p->key ? &p->l : &p->r;
        if (!*next) { *next = n; return root; }
        p = *next;
    }
}

int run(int frames) {
    Node* root = 0;
    for (int i = 0; i < N; i++) {
        ents[i] = (Entity){ (float)(rnd() % 200) - 100.0f, (float)(rnd() % 10), (float)(rnd() % 200) - 100.0f,
                            0.0f, 0.5f, 0.0f, (int)(rnd() % 4), 0, (int)(rnd() % 8), i };
        order[i] = i;
        for (int k = 0; k < 15; k++) names[i][k] = (char)('a' + (rnd() % 26));
        names[i][15] = 0;
    }
    for (int i = 0; i < 64; i++) {
        nodes[i] = (Node){ 0, 0, (int)(rnd() % 1000), i };
        root = insert(root, &nodes[i]);
    }
    int result = 0;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < N; i++) update(&ents[i]);
        sort(order, N);
        for (int i = 0; i < N; i++)
            for (int j = i + 1; j < N && j < i + 8; j++)
                result += overlap(&ents[order[i]], &ents[order[j]]);
        for (int i = 0; i < 16; i++) result ^= (int)hash(names[(f + i) % N]);
        for (int i = 0; i < 32; i++) result += find(root, (int)(rnd() % 1000));
        Entity tmp;
        memcpy(&tmp, &ents[f % N], sizeof tmp);
        memcpy(&ents[f % N], &ents[(f + 1) % N], sizeof tmp);
        memcpy(&ents[(f + 1) % N], &tmp, sizeof tmp);
    }
    return result;
}
