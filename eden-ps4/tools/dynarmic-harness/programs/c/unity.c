// (04.27) A frame of work shaped like Unity's IL2CPP output, for the A64 harness (image mode: x19 =
// frames, the result in x0): objects with a class pointer and a vtable, virtual calls through it
// (BLR), a null check before each field access and a bounds check before each array element (to
// throw helpers that never run: CBZ and CMP; B.HS to cold code), write-barrier calls on reference
// stores, Vector3 math in floats, a List<T>-style iteration, a dictionary lookup, an Interlocked
// counter. Built by build64.sh as unity64.bin.
#include <stdint.h>
#include <stddef.h>

void* memcpy(void* d, const void* s, size_t n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, size_t n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)c; return d; }

typedef struct Object Object;
typedef struct Klass {
    const char* name;
    void (*update)(Object* self, float dt);
    int (*get_priority)(Object* self);
} Klass;
struct Object { const Klass* klass; void* monitor; };

typedef struct { float x, y, z; } Vector3;
typedef struct { Object obj; uint32_t max_length; uint32_t pad; Object* items[1]; } ObjArray;
typedef struct { Object obj; uint32_t max_length; uint32_t pad; float items[1]; } FloatArray;

typedef struct {
    Object obj;
    Vector3 position, velocity;
    float health;
    int32_t state, ticks;
    Object* target;     // a reference: stored through the write barrier
    FloatArray* weights;
} Component;

typedef struct { Object obj; ObjArray* items; int32_t size; int32_t version; } List;

static volatile int64_t g_counter;
static uint32_t g_wb_count;
static uint32_t seed = 4242;

__attribute__((noinline, noreturn)) static void ThrowNullReference(void) { for (;;) __asm__ volatile("brk #1"); }
__attribute__((noinline, noreturn)) static void ThrowIndexOutOfRange(void) { for (;;) __asm__ volatile("brk #2"); }
__attribute__((noinline)) static void il2cpp_gc_wbarrier_set_field(Object* obj, Object** field, Object* value) {
    *field = value;
    g_wb_count += (obj != 0);
}

#define NULL_CHECK(p) do { if (__builtin_expect((p) == 0, 0)) ThrowNullReference(); } while (0)
#define BOUNDS_CHECK(a, i) do { if (__builtin_expect((uint32_t)(i) >= (a)->max_length, 0)) ThrowIndexOutOfRange(); } while (0)

static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

__attribute__((noinline)) static float Vector3_Magnitude(Vector3 v) {
    float s = v.x * v.x + v.y * v.y + v.z * v.z;
    // (a few Newton steps: no libm)
    float r = s > 1.0f ? s * 0.5f : 1.0f;
    for (int i = 0; i < 4; i++) r = 0.5f * (r + s / r);
    return r;
}

__attribute__((noinline)) static Vector3 Vector3_MoveTowards(Vector3 a, Vector3 b, float step) {
    Vector3 d = {b.x - a.x, b.y - a.y, b.z - a.z};
    float m = Vector3_Magnitude(d);
    if (m <= step || m == 0.0f) return b;
    float k = step / m;
    Vector3 r = {a.x + d.x * k, a.y + d.y * k, a.z + d.z * k};
    return r;
}

static void Enemy_Update(Object* self_obj, float dt) {
    NULL_CHECK(self_obj);
    Component* self = (Component*)self_obj;
    Object* target = self->target;
    if (target != 0) {
        NULL_CHECK(target);
        Component* t = (Component*)target;
        self->position = Vector3_MoveTowards(self->position, t->position, 2.0f * dt);
        if (t->health <= 0.0f)
            il2cpp_gc_wbarrier_set_field(self_obj, &self->target, 0);
    }
    FloatArray* w = self->weights;
    NULL_CHECK(w);
    float acc = 0.0f;
    for (int32_t i = 0; i < 4; i++) {
        BOUNDS_CHECK(w, i);
        acc += w->items[i] * (float)(i + 1);
    }
    self->health -= acc * 0.001f;
    self->ticks++;
    if (self->health < 0.0f) { self->health = 100.0f; self->state = (self->state + 1) & 3; }
}

static void Pickup_Update(Object* self_obj, float dt) {
    NULL_CHECK(self_obj);
    Component* self = (Component*)self_obj;
    self->position.y += self->velocity.y * dt;
    if (self->position.y > 2.0f || self->position.y < 0.0f) self->velocity.y = -self->velocity.y;
    self->ticks++;
}

static int Enemy_Priority(Object* self) { NULL_CHECK(self); return ((Component*)self)->state + 10; }
static int Pickup_Priority(Object* self) { NULL_CHECK(self); return 1; }

static const Klass EnemyKlass = {"Enemy", Enemy_Update, Enemy_Priority};
static const Klass PickupKlass = {"Pickup", Pickup_Update, Pickup_Priority};

#define NCOMP 128
static Component comps[NCOMP];
static struct { Object obj; uint32_t max_length; uint32_t pad; Object* items[NCOMP]; } comp_array;
static struct { Object obj; uint32_t max_length; uint32_t pad; float items[4]; } weights[NCOMP];
static List list;

// a Dictionary<int, Object>-style lookup: buckets of entries with hash and next
#define NBUCKETS 64
typedef struct { int32_t hash, next, key; Object* value; } Entry;
static struct { Object obj; uint32_t max_length; uint32_t pad; int32_t items[NBUCKETS]; } buckets;
static struct { Object obj; uint32_t max_length; uint32_t pad; Entry items[NCOMP]; } entries;

__attribute__((noinline)) static Object* Dictionary_TryGetValue(int32_t key) {
    NULL_CHECK(&buckets);
    int32_t h = key * 0x9E3779B1;
    h &= 0x7FFFFFFF;
    int32_t b = h % NBUCKETS;
    BOUNDS_CHECK(&buckets, b);
    for (int32_t i = buckets.items[b] - 1; i >= 0;) {
        BOUNDS_CHECK(&entries, i);
        Entry* e = &entries.items[i];
        if (e->hash == h && e->key == key) return e->value;
        i = e->next;
    }
    return 0;
}

static void setup(void) {
    comp_array.max_length = NCOMP;
    for (int i = 0; i < NCOMP; i++) {
        Component* c = &comps[i];
        c->obj.klass = (i % 3) ? &EnemyKlass : &PickupKlass;
        c->position = (Vector3){(float)(rnd() % 100), 0.5f, (float)(rnd() % 100)};
        c->velocity = (Vector3){0.0f, 1.0f, 0.0f};
        c->health = 50.0f + (float)(rnd() % 50);
        weights[i].max_length = 4;
        for (int k = 0; k < 4; k++) weights[i].items[k] = (float)(rnd() % 10) * 0.25f;
        c->weights = (FloatArray*)&weights[i];
        comp_array.items[i] = &c->obj;
    }
    for (int i = 0; i < NCOMP; i++)
        comps[i].target = (i % 3) ? &comps[(i * 7 + 1) % NCOMP].obj : 0;
    list.items = (ObjArray*)&comp_array;
    list.size = NCOMP;
    buckets.max_length = NBUCKETS;
    entries.max_length = NCOMP;
    for (int i = 0; i < NCOMP; i++) {
        int32_t key = i * 13 + 7;
        int32_t h = (key * 0x9E3779B1) & 0x7FFFFFFF;
        int32_t b = h % NBUCKETS;
        entries.items[i] = (Entry){h, buckets.items[b] - 1, key, &comps[i].obj};
        buckets.items[b] = i + 1;
    }
}

__attribute__((noinline)) static int64_t frame(float dt) {
    // foreach (var c in list) c.Update(dt)  (List<T> enumerator: the version, the bounds)
    NULL_CHECK(&list);
    int32_t version = list.version;
    int64_t prio = 0;
    for (int32_t i = 0; i < list.size; i++) {
        if (version != list.version) ThrowIndexOutOfRange();
        ObjArray* items = list.items;
        NULL_CHECK(items);
        BOUNDS_CHECK(items, i);
        Object* c = items->items[i];
        NULL_CHECK(c);
        c->klass->update(c, dt);
        prio += c->klass->get_priority(c);
    }
    // lookups by key
    for (int32_t k = 0; k < 64; k++) {
        Object* o = Dictionary_TryGetValue((int32_t)(rnd() % NCOMP) * 13 + 7);
        if (o) prio += ((Component*)o)->ticks & 7;
    }
    __atomic_fetch_add(&g_counter, prio, __ATOMIC_SEQ_CST);
    return prio;
}

int64_t run(int64_t frames) {
    setup();
    int64_t sum = 0;
    for (int64_t f = 0; f < frames; f++)
        sum += frame(1.0f / 30.0f);
    return sum ^ (int64_t)g_wb_count ^ g_counter;
}
