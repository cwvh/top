// Randomized tests for top: a separate unity build (everything but main.c)
// with the debug checks on: UBSan traps, guard pages behind every allocation,
// poisoned arenas.
//   $ make test
//   $ top-test.exe [-s SEED] [-n ITERS] [-i START]
//
// Iteration i of seed s is self-contained: it derives its own generator from
// (s, i) and builds its own Ui, so "-s S -i I -n 1" replays exactly it.
#include "base.c"
#include "win32.c"
#include "sample.c"
#include "ui.c"

#ifndef DEBUG
#error "test.c needs -DDEBUG (guard pages and poisoning)"
#endif

// These do not fit in an enum, or are better kept typed.
#define I64_MAX 0x7fffffffffffffffLL
#define I64_MIN (-I64_MAX - 1)
#define U32_MAX 0xffffffffu
enum { I32_MAX = 0x7fffffff, I32_MIN = -I32_MAX - 1 };


// Platform hooks

// Output.handle value meaning "append to the capture buffer". Real handles
// are multiples of 4, so this never collides with one.
#define CAPTURE ((uptr)1)

typedef struct {
    u8 *buf;
    iz  len;
    iz  size;
} Capture;
static Capture capt;

static b32 os_write(Output *o) {
    if (o->handle == CAPTURE) {
        if (o->len > capt.size - capt.len) {
            return 0;
        }
        memcpy(capt.buf + capt.len, o->buf, (uz)o->len);
        capt.len += o->len;
        return 1;
    }
    for (iz off = 0; off < o->len;) {
        u32 n     = 0;
        u32 count = (u32)min(o->len - off, (iz)1<<30);
        if (!WriteFile(o->handle, o->buf+off, count, &n, 0) || !n) {
            return 0;
        }
        off += n;
    }
    return 1;
}

static void os_protect(void *p, iz size, b32 readwrite) {
    u32 old = 0;
    b32 ok  = VirtualProtect(p, (uz)size, readwrite ? PAGE_READWRITE : PAGE_NOACCESS, &old);
    assert(ok);
}

static void poison(Arena a) {
    os_protect(a.beg, a.end - a.beg, 1);
    memset(a.beg, 0xcc, (uz)(a.end - a.beg));
}

static u64 rand64(u64 *s);

// SAFETY: this is a stub. It never opens or terminates a process; it only
// records what it was asked to kill and answers randomly.
typedef struct {
    u64 rng;
    u32 last;
    i32 count;
} Kill;
static Kill killed;

static b32 os_kill(u32 pid) {
    killed.last = pid;
    killed.count++;
    return (b32)(rand64(&killed.rng) >> 63);
}


// Random numbers: 64-bit LCG with an xorshift-multiply output permutation.

static u64 rand64(u64 *s) {
    *s = *s*0x3243f6a8885a308dULL + 1;
    u64 r = *s;
    r ^= r >> 32;
    r *= 0xdaba0b6eb09322e3ULL;
    r ^= r >> 32;
    r *= 0xdaba0b6eb09322e3ULL;
    r ^= r >> 32;
    return r;
}

// Uniform-ish in [0, n)
static u64 rnd(u64 *s, u64 n) {
    return rand64(s) % n;
}

// Uniform-ish in [lo, hi]
static i64 rrange(u64 *s, i64 lo, i64 hi) {
    return lo + (i64)rnd(s, (u64)(hi - lo + 1));
}

static b32 chance(u64 *s, u64 percent) {
    return rnd(s, 100) < percent;
}

static f64 randf(u64 *s) {
    // 53 bits fit an i64, and tcc has no helper to convert from a u64.
    return (f64)(i64)(rand64(s) >> 11) * (1.0 / 9007199254740992.0);
}

// Index chosen with the given relative weights.
static i32 weighted(u64 *s, i32 const *w, i32 n) {
    i32 total = 0;
    for (i32 i = 0; i < n; i++) {
        total += w[i];
    }
    i32 x = (i32)rnd(s, (u64)total);
    for (i32 i = 0; i < n; i++) {
        if (x < w[i]) {
            return i;
        }
        x -= w[i];
    }
    return n - 1;
}
#define WEIGHTED(r, ...) \
    weighted(r, (i32 const[]){__VA_ARGS__}, \
             (i32)(sizeof((i32 const[]){__VA_ARGS__})/sizeof(i32)))


// Reporting

typedef struct {
    u64         seed;
    i64         iter;       // -1 during the startup tests
    char const *phase;
    i32         ncpu;
    iz          nprocs;
    i32         width, height;
    i32         keys[1024]; // ring of recent keys
    i64         nkeys;
    u8         *frame;      // last captured frame, dumped on failure
    iz          framelen;
    Output     *out;
    b32         dead;
} Ctx;
static Ctx ctx;

static Str cstr(char const *s) {
    iz n = 0;
    for (; s[n]; n++) {}
    return (Str){(u8 *)s, n};
}

static void printhex(Output *o, u64 v, i32 digits) {
    u8  t[16];
    i32 n = 0;
    do {
        t[n++] = (u8)"0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    for (i32 i = n; i < digits; i++) {
        printc(o, '0');
    }
    while (n) {
        printc(o, t[--n]);
    }
}

static void printkey(Output *o, i32 key) {
    if (key >= KEY_UP) {
        static char const *const names[] = {
            "UP", "DOWN", "LEFT", "RIGHT", "PGUP", "PGDN", "HOME", "END", "DELETE",
        };
        print(o, S("KEY_"));
        if (key - KEY_UP < countof(names)) {
            print(o, cstr(names[key - KEY_UP]));
        } else {
            printhex(o, (u64)key, 1);
        }
    } else if (key>32 && key<127) {
        printc(o, '\'');
        printc(o, (u8)key);
        printc(o, '\'');
    } else {
        print(o, S("U+"));
        printhex(o, (u64)(u32)key, 4);
    }
}

static void dumpframe(Output *o, u8 const *p, iz n) {
    iz lim = min(n, (iz)6000);
    for (iz i = 0; i < lim; i++) {
        u8 c = p[i];
        if (c == 0x1b) {
            print(o, S("\\e"));
        } else if (c == '\r') {
            print(o, S("\\r"));
        } else if (c == '\n') {
            print(o, S("\\n\n"));
        } else if (c<0x20 || c>=0x7f) {
            print(o, S("\\x"));
            printhex(o, c, 2);
        } else {
            printc(o, c);
        }
    }
    if (lim < n) {
        print(o, S("..."));
    }
    print(o, S("\n"));
}

static _Noreturn void context(void) {
    Output *o = ctx.out;
    print(o, S("  seed:      0x"));
    printhex(o, ctx.seed, 1);
    print(o, S("\n  iteration: "));
    printi64(o, ctx.iter);
    print(o, S("\n  phase:     "));
    print(o, cstr(ctx.phase ? ctx.phase : "?"));
    print(o, S("\n  snapshot:  ncpu="));
    printi64(o, ctx.ncpu);
    print(o, S(" nprocs="));
    printi64(o, ctx.nprocs);
    print(o, S("\n  terminal:  "));
    printi64(o, ctx.width);
    print(o, S("x"));
    printi64(o, ctx.height);
    print(o, S("\n  reproduce: top-test.exe -s 0x"));
    printhex(o, ctx.seed, 1);
    print(o, S(" -i "));
    printi64(o, max(ctx.iter, (i64)0));
    print(o, S(" -n 1   (gdb: gdb --args top-test.exe ...)\n"));
    i64 count = min(ctx.nkeys, (i64)countof(ctx.keys));
    print(o, S("  keys:      (last "));
    printi64(o, count);
    print(o, S(" of "));
    printi64(o, ctx.nkeys);
    print(o, S(")"));
    for (i64 i = ctx.nkeys - count; i < ctx.nkeys; i++) {
        printc(o, ' ');
        printkey(o, ctx.keys[i % countof(ctx.keys)]);
    }
    print(o, S("\n"));
    if (ctx.framelen) {
        print(o, S("  last output ("));
        printi64(o, ctx.framelen);
        print(o, S(" bytes):\n"));
        dumpframe(o, ctx.frame, ctx.framelen);
    }
    flush(o);
    ExitProcess(1);
}

static _Noreturn void fail(char const *check, char const *what, i64 a, i64 b) {
    Output *o = ctx.out;
    if (ctx.dead) {
        ExitProcess(1);  // failed while reporting
    }
    ctx.dead = 1;
    print(o, S("FAIL: "));
    print(o, cstr(check));
    print(o, S(": "));
    print(o, cstr(what));
    print(o, S(" (a="));
    printi64(o, a);
    print(o, S(" b="));
    printi64(o, b);
    print(o, S(")\n"));
    context();
}

typedef struct ExceptionRecord {
    u32                     code;
    u32                     flags;
    struct ExceptionRecord *next;
    void                   *addr;
    u32                     nparams;
    u64                     info[15];
} ExceptionRecord;

typedef struct {
    ExceptionRecord *rec;
    void            *context;
} ExceptionPointers;

W32(void *) AddVectoredExceptionHandler(u32, i32 (*)(ExceptionPointers *));

// A fault (guard page, UBSan trap, assert) gets the same report as a failed
// check, so the seed and the key sequence are never lost.
static i32 onfault(ExceptionPointers *ep) {
    ExceptionRecord *e = ep->rec;
    if ((e->code & 0xc0000000u) != 0xc0000000u || ctx.dead) {
        return 0;  // not an error, e.g. a debugger message
    }
    ctx.dead = 1;
    Output *o = ctx.out;
    print(o, S("FAULT: "));
    if (e->code == 0xc0000005u) {
        print(o, e->info[0] ? S("write") : S("read"));
        print(o, S(" access violation at 0x"));
        printhex(o, e->info[1], 1);
        print(o, S(" (guard page overrun or stale pointer?)"));
    } else if (e->code == 0xc000001du) {
        print(o, S("illegal instruction (UBSan trap or assert) "));
    } else {
        print(o, S("exception 0x"));
        printhex(o, e->code, 8);
    }
    print(o, S(", instruction at 0x"));
    printhex(o, (u64)(uptr)e->addr, 1);
    print(o, S("\n"));
    context();
}


// Reference implementations, written differently from the real ones

// Length of the well-formed UTF-8 sequence at p (Unicode table 3-7), or 0.
static iz refdecode(u8 const *p, iz n, i32 *cp) {
    u8  c  = p[0];
    iz  len = 0;
    i32 v  = 0;
    u8  lo = 0x80;
    u8  hi = 0xbf;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if (c>=0xc2 && c<=0xdf) {
        len = 2; v = c & 0x1f;
    } else if (c>=0xe0 && c<=0xef) {
        len = 3; v = c & 0x0f;
        lo = c==0xe0 ? 0xa0 : lo;
        hi = c==0xed ? 0x9f : hi;
    } else if (c>=0xf0 && c<=0xf4) {
        len = 4; v = c & 0x07;
        lo = c==0xf0 ? 0x90 : lo;
        hi = c==0xf4 ? 0x8f : hi;
    } else {
        return 0;
    }
    if (n < len) {
        return 0;
    }
    for (iz i = 1; i < len; i++) {
        u8 b = p[i];
        if (b<(i==1 ? lo : 0x80) || b>(i==1 ? hi : 0xbf)) {
            return 0;
        }
        v = v*64 + (b & 0x3f);
    }
    *cp = v;
    return len;
}

// Encode a valid scalar value with arithmetic instead of shifts.
static iz refenc(u8 *b, i32 cp) {
    if (cp < 0x80) {
        b[0] = (u8)cp;
        return 1;
    } else if (cp < 0x800) {
        b[0] = (u8)(0xc0 + cp/64);
        b[1] = (u8)(0x80 + cp%64);
        return 2;
    } else if (cp < 0x10000) {
        b[0] = (u8)(0xe0 + cp/4096);
        b[1] = (u8)(0x80 + cp/64%64);
        b[2] = (u8)(0x80 + cp%64);
        return 3;
    }
    b[0] = (u8)(0xf0 + cp/262144);
    b[1] = (u8)(0x80 + cp/4096%64);
    b[2] = (u8)(0x80 + cp/64%64);
    b[3] = (u8)(0x80 + cp%64);
    return 4;
}

static b32 validutf8(u8 const *p, iz n) {
    for (iz i = 0; i < n;) {
        i32 cp  = 0;
        iz  len = refdecode(p+i, n-i, &cp);
        if (!len) {
            return 0;
        }
        i += len;
    }
    return 1;
}

static u8 reflower(u8 c) {
    return (u8)((u32)c - (u32)'A' < 26u ? c + 32 : c);
}

static b32 refcontains(u8 const *h, iz hn, u8 const *nd, iz nn) {
    for (iz i = 0; i+nn <= hn; i++) {
        b32 ok = 1;
        for (iz j = 0; j < nn && ok; j++) {
            ok = reflower(h[i+j]) == reflower(nd[j]);
        }
        if (ok) {
            return 1;
        }
    }
    return 0;
}

static b32 refint(u8 const *p, iz n, i64 *v) {
    if (n == 0) {
        return 0;
    }
    i64 x = 0;
    for (iz i = 0; i < n; i++) {
        if (p[i]<'0' || p[i]>'9') {
            return 0;
        }
        if (x < (i64)1<<40) {
            x = x*10 + (p[i] - '0');
        }
    }
    if (x > I32_MAX) {
        return 0;
    }
    *v = x;
    return 1;
}

static b32 refdelay(u8 const *p, iz n, i64 *ms) {
    iz  i      = 0;
    i64 whole  = 0;
    i64 frac3  = 0;
    iz  fd     = 0;
    i32 digits = 0;
    for (; i<n && p[i]>='0' && p[i]<='9'; i++) {
        digits++;
        if (whole < (i64)1<<40) {
            whole = whole*10 + (p[i] - '0');
        }
    }
    if (i<n && p[i]=='.') {
        for (i++; i<n && p[i]>='0' && p[i]<='9'; i++) {
            digits++;
            if (fd < 3) {
                frac3 = frac3*10 + (p[i] - '0');
            }
            fd++;
        }
    }
    if (!digits || i!=n || whole>1000000) {
        return 0;
    }
    for (; fd < 3; fd++) {
        frac3 *= 10;
    }
    *ms = max(whole*1000 + frac3, (i64)100);
    return 1;
}

static iz refi64(u8 *end, i64 v) {
    u64 u   = v<0 ? 0 - (u64)v : (u64)v;
    u8 *beg = end;
    do {
        *--beg = (u8)('0' + u%10);
        u /= 10;
    } while (u);
    if (v < 0) {
        *--beg = '-';
    }
    return end - beg;
}


// Generators

static i64 geni64(u64 *r) {
    switch (rnd(r, 12)) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return -1;
    case 3:  return rrange(r, 0, 1<<20);
    case 4:  return rrange(r, 0, (i64)1<<40);
    case 5:  return (i64)(rand64(r) >> 1);
    case 6:  return I64_MAX - rrange(r, 0, 3);
    case 7:  return -rrange(r, 1, 1<<20);
    case 8:  return I64_MIN + rrange(r, 0, 3);
    case 9:  return (i64)rand64(r);
    }
    return rrange(r, 0, 1<<30);
}

// Byte counts: mostly plausible, sometimes hostile.
static i64 genbytes(u64 *r) {
    return chance(r, 50) ? rrange(r, 0, (i64)64<<30) : geni64(r);
}

static i64 gentime(u64 *r) {
    return chance(r, 50) ? rrange(r, 0, (i64)1<<44) : geni64(r);
}

static i32 geni32(u64 *r) {
    switch (rnd(r, 8)) {
    case 0:  return 0;
    case 1:  return (i32)rrange(r, 0, 64);
    case 2:  return (i32)rrange(r, 0, 1<<20);
    case 3:  return -(i32)rrange(r, 1, 1000);
    case 4:  return I32_MAX;
    case 5:  return I32_MIN;
    case 6:  return (i32)rand64(r);
    }
    return (i32)rrange(r, 0, 5000);
}

#if __GNUC__
static f64 nanf64(void) { return __builtin_nan(""); }
static f64 inff64(void) { return __builtin_inf(); }
#else
// tcc has no such builtins. The volatile keeps the compiler from
// folding (and rejecting) the arithmetic.
static f64 nanf64(void) { volatile f64 z = 0; return z/z; }
static f64 inff64(void) { volatile f64 b = 1e300; return b*b; }
#endif

static f64 genfrac(u64 *r) {
    switch (rnd(r, 14)) {
    case 0:  return 0.0;
    case 1:  return 1.0;
    case 2:  return 1.0000001;
    case 3:  return 1.5;
    case 4:  return -0.25;
    case 5:  return nanf64();
    case 6:  return inff64();
    case 7:  return -inff64();
    case 8:  return 1e300;
    case 9:  return -1e300;
    case 10: return 5e-324;
    }
    return randf(r);
}

static CpuTimes gencpu(u64 *r) {
    CpuTimes t = {0};
    switch (rnd(r, 6)) {
    case 0:
        break;
    case 1:
        t = (CpuTimes){1, 1, 1, 1, 1};
        break;
    case 2:
    case 3: {
        f64 v[5];
        f64 sum = 0;
        for (i32 i = 0; i < 5; i++) {
            v[i] = randf(r) * (i<2 ? 1.0 : 0.1);
            sum += v[i];
        }
        if (sum > 0) {
            t = (CpuTimes){v[0]/sum, v[1]/sum, v[2]/sum, v[3]/sum, v[4]/sum};
        }
        break;
    }
    default: {
        // Initializer evaluation order is unspecified in C, so sequence
        // the draws explicitly, left to right.
        f64 us = genfrac(r);
        f64 sy = genfrac(r);
        f64 id = genfrac(r);
        f64 hi = genfrac(r);
        f64 si = genfrac(r);
        t = (CpuTimes){us, sy, id, hi, si};
    }
    }
    return t;
}

static i32 gencp(u64 *r) {
    static i32 const special[] = {
        0xfffd, 0x2028, 0x200b, 0x85, 0x9b, 0x7f, 0xfeff, 0x1f600, 0xa0, 0x10ffff,
    };
    switch (rnd(r, 7)) {
    case 0:  return (i32)rrange(r, 0x20, 0x7e);
    case 1:  return (i32)rrange(r, 0xa0, 0x7ff);
    case 2:  return (i32)rrange(r, 0x800, 0xd7ff);
    case 3:  return (i32)rrange(r, 0xe000, 0xffff);
    case 4:  return (i32)rrange(r, 0x10000, 0x10ffff);
    case 5:  return special[rnd(r, (u64)countof(special))];
    }
    return (i32)rrange(r, 0x80, 0x9f);
}

typedef struct {
    u8 n;
    u8 b[5];
} Seq;

// Malformed UTF-8: stray continuations, truncations, overlongs, surrogates,
// out of range, and bytes that never appear.
static Seq const badseq[] = {
    {1, {0x80}}, {1, {0xbf}}, {1, {0xc3}}, {2, {0xe2, 0x82}},
    {3, {0xf0, 0x9f, 0x98}}, {2, {0xc0, 0x80}}, {2, {0xc1, 0xbf}},
    {3, {0xe0, 0x80, 0x80}}, {3, {0xe0, 0x9f, 0xbf}},
    {4, {0xf0, 0x80, 0x80, 0x80}}, {4, {0xf0, 0x8f, 0xbf, 0xbf}},
    {3, {0xed, 0xa0, 0x80}}, {3, {0xed, 0xbf, 0xbf}},
    {4, {0xf4, 0x90, 0x80, 0x80}}, {4, {0xf5, 0x80, 0x80, 0x80}},
    {1, {0xff}}, {1, {0xfe}}, {5, {0xf8, 0x88, 0x80, 0x80, 0x80}},
    {2, {0xc3, 0x28}}, {3, {0xe2, 0x28, 0xa1}}, {1, {0x9b}},
};

// Fill t (at least 4096+16 bytes) with a hostile name, at most limit bytes.
static iz genname(u64 *r, u8 *t, iz limit) {
    iz n = 0;
    switch (WEIGHTED(r, 6, 38, 8, 6, 9, 14, 15, 4)) {
    case 0:
        break;
    case 1: {
        static char const alpha[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._- ";
        for (iz i = rrange(r, 1, 14); i > 0; i--) {
            t[n++] = (u8)alpha[rnd(r, countof(alpha) - 1)];
        }
        if (chance(r, 60)) {
            memcpy(t+n, ".exe", 4);
            n += 4;
        }
        break;
    }
    case 2:
        for (iz i = rrange(r, 20, 150); i > 0; i--) {
            t[n++] = (u8)rrange(r, 'a', 'z');
        }
        break;
    case 3: {
        iz want = rrange(r, 1000, 1700);
        b32 mb  = chance(r, 40);
        while (n < want) {
            n += utf8encode(t+n, mb&&chance(r, 30) ? gencp(r) : (i32)rrange(r, 'a', 'z'));
        }
        break;
    }
    case 4:
        for (iz i = rrange(r, 1, 12); i > 0; i--) {
            switch (rnd(r, 7)) {
            case 0:  t[n++] = (u8)rrange(r, 0, 31); break;
            case 1:  t[n++] = 127; break;
            case 2:  t[n++] = '\r'; t[n++] = '\n'; break;
            case 3:  memcpy(t+n, "\x1b[31m", 5); n += 5; break;
            case 4:  memcpy(t+n, "\x1b]0;x\x07", 6); n += 6; break;
            case 5:  n += utf8encode(t+n, (i32)rrange(r, 0x80, 0x9f)); break;
            default: t[n++] = (u8)rrange(r, 'a', 'z'); break;
            }
        }
        break;
    case 5:
        for (iz i = rrange(r, 1, 12); i > 0; i--) {
            n += utf8encode(t+n, gencp(r));
        }
        break;
    default:
        for (iz i = rrange(r, 1, 20); i > 0; i--) {
            switch (rnd(r, 4)) {
            case 0:
            case 1: {
                Seq const *q = badseq + rnd(r, (u64)countof(badseq));
                memcpy(t+n, q->b, q->n);
                n += q->n;
                break;
            }
            case 2:  t[n++] = (u8)rand64(r); break;
            default: t[n++] = (u8)rrange(r, 'a', 'z'); break;
            }
        }
        break;
    }
    return min(n, limit);
}

static i32 const cmdkeys[] = {
    'P', 'M', 'T', 'N', '<', '>', 'R', 'I', '1', 'E', 'h', '?', '/', 'k', 'd',
    'q', 27, 13, 8, 127, 3, 9,
};
static i32 const navkeys[] = {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PGUP, KEY_PGDN, KEY_HOME,
    KEY_END, KEY_DELETE,
};

static i32 genkey(u64 *r) {
    switch (WEIGHTED(r, 45, 20, 14, 9, 4, 4, 4)) {
    case 0:  return cmdkeys[rnd(r, (u64)countof(cmdkeys))];
    case 1:  return navkeys[rnd(r, (u64)countof(navkeys))];
    case 2:  return (i32)rrange(r, 32, 126);
    case 3:  return gencp(r);
    case 4:  return (i32)rrange(r, 0xd800, 0xdfff);  // lone surrogate
    case 5:  return (i32)rrange(r, 0, 31);
    }
    return (i32)rrange(r, 0x80, 0x9f);
}

// A chunk of typing: open a prompt, type past its limit, maybe finish.
static iz genrun(u64 *r, i32 *k, iz n) {
    static i32 const openers[] = {'/', 'k', 'd'};
    k[n++] = openers[rnd(r, 3)];
    iz  len  = chance(r, 40) ? rrange(r, 60, 200) : rrange(r, 0, 10);
    i32 mode = (i32)rnd(r, 3);
    for (iz i = 0; i < len; i++) {
        if (chance(r, 4)) {
            k[n++] = chance(r, 50) ? 8 : 127;
        } else if (mode == 0) {
            k[n++] = "0123456789.e"[rnd(r, 12)];
        } else if (mode == 1) {
            k[n++] = (i32)rrange(r, 32, 126);
        } else {
            k[n++] = chance(r, 8) ? (i32)rrange(r, 0xd800, 0xdfff) : gencp(r);
        }
    }
    switch (rnd(r, 5)) {
    case 0:
    case 1: k[n++] = 13; break;
    case 2: k[n++] = 27; break;
    }
    return n;
}

// Up to max keys (max >= 256 + a run's worth).
static iz genkeys(u64 *r, i32 *k, iz max) {
    iz n = 0;
    for (i64 chunks = rrange(r, 1, 6); chunks>0 && n+220<max; chunks--) {
        i32 roll = WEIGHTED(r, 55, 20, 15, 10);
        if (roll == 0) {
            k[n++] = genkey(r);
        } else if (roll == 1) {
            n = genrun(r, k, n);
        } else if (roll == 2) {
            i32 key = navkeys[rnd(r, (u64)countof(navkeys))];
            for (i64 i = rrange(r, 2, 12); i > 0; i--) {
                k[n++] = key;
            }
        } else {
            static i32 const sorts[] = {'P', 'M', 'T', 'N', '<', '>', 'R'};
            for (i64 i = rrange(r, 1, 4); i > 0; i--) {
                k[n++] = sorts[rnd(r, (u64)countof(sorts))];
            }
        }
    }
    return n;
}

static void gensize(u64 *r, i32 *w, i32 *h) {
    switch (WEIGHTED(r, 3, 3, 4, 4, 3, 5, 2, 2, 3, 3, 2, 2, 55)) {
    case 0:  *w = 0;  *h = 0; break;
    case 1:  *w = 1;  *h = 1; break;
    case 2:  *w = 1;  *h = (i32)rrange(r, 2, 60); break;
    case 3:  *w = (i32)rrange(r, 2, 300);  *h = 1; break;
    case 4:  *w = 10; *h = 3; break;
    case 5:  *w = 80; *h = 24; break;
    case 6:  *w = 0;  *h = (i32)rrange(r, 1, 50); break;
    case 7:  *w = (i32)rrange(r, 1, 100); *h = 0; break;
    case 8:  *w = (i32)rrange(r, 2, 40);  *h = (i32)rrange(r, 1, 12); break;
    case 9:  *w = 120; *h = 40; break;
    case 10: *w = 500; *h = 200; break;
    case 11: *w = 2;  *h = 2; break;
    default: *w = (i32)rrange(r, 0, 500); *h = (i32)rrange(r, 0, 200); break;
    }
}

static void genproc(u64 *r, ProcInfo *p, i32 ncpu) {
    p->ppid       = chance(r, 50) ? (u32)rrange(r, 0, 99999) : (u32)rand64(r);
    p->created    = geni64(r);
    p->cputime    = gentime(r);
    p->cpu        = chance(r, 60) ? randf(r) * (f64)max(ncpu, 1) : genfrac(r);
    p->workingset = genbytes(r);
    p->privbytes  = genbytes(r);
    p->threads    = geni32(r);
    p->handles    = geni32(r);
    p->priority   = chance(r, 70) ? (i32)rrange(r, 0, 15) : geni32(r);
    p->session    = chance(r, 70) ? (u32)rrange(r, 0, 3) : (u32)rand64(r);
}

// A contract-valid snapshot: cores has ncpu entries and procs has nprocs,
// each in its own guarded allocation. Names get their own allocation too,
// unless shared (huge nprocs), when they live in one buffer.
static Snapshot *gensnap(Arena *a, u64 *r, i32 ncpu, iz nprocs, b32 shared) {
    Snapshot *s = new(a, 1, Snapshot);
    s->ncpu     = ncpu;
    s->cores    = new(a, ncpu, CpuTimes);
    for (i32 i = 0; i < ncpu; i++) {
        s->cores[i] = gencpu(r);
    }
    s->cpu         = gencpu(r);
    s->interval    = chance(r, 30) ? 0 : geni64(r);
    s->uptime_ms   = WEIGHTED(r, 2, 4, 1)==0 ? 0 :
                     WEIGHTED(r, 4, 1)==0 ? rrange(r, 0, (i64)1e12) : I64_MAX;
    s->memtotal    = genbytes(r);
    s->memavail    = genbytes(r);
    s->commit      = genbytes(r);
    s->commitlimit = genbytes(r);

    s->procs  = new(a, nprocs, ProcInfo);
    s->nprocs = nprocs;
    u8 *pool  = shared ? new(a, nprocs*48, u8) : 0;
    u8  tmp[4096 + 16];
    i64 sumt = 0;
    i64 sumh = 0;
    for (iz i = 0; i < nprocs; i++) {
        ProcInfo *p = s->procs + i;
        genproc(r, p, ncpu);
        switch (WEIGHTED(r, 50, 8, 4, 8, 30)) {
        case 0:  p->pid = (u32)(4*(i + 1)); break;
        case 1:  p->pid = 0; break;
        case 2:  p->pid = U32_MAX; break;
        case 3:  p->pid = i ? p[-1].pid : 4; break;  // duplicate
        default: p->pid = (u32)rand64(r); break;
        }
        iz len = genname(r, tmp, shared ? 48 : 2048);
        if (shared) {
            p->name = (Str){pool + i*48, len};
        } else {
            p->name = (Str){new(a, len, u8), len};
        }
        if (len) {
            memcpy(p->name.data, tmp, (uz)len);
        }
        sumt += p->threads;
        sumh += p->handles;
    }
    s->nthreads = chance(r, 60) ? sumt : geni64(r);
    s->nhandles = chance(r, 60) ? sumh : geni64(r);
    return s;
}

// Change an existing snapshot in place: new numbers, swapped or shared
// names, fewer procs. It stays contract-valid.
static void mutate(u64 *r, Snapshot *s) {
    if (chance(r, 40)) {
        s->uptime_ms = chance(r, 50) ? rrange(r, 0, (i64)1e12) : I64_MAX;
        s->memtotal  = genbytes(r);
        s->memavail  = genbytes(r);
        s->commit    = genbytes(r);
        s->cpu       = gencpu(r);
    }
    for (i32 i = 0; i < s->ncpu; i++) {
        if (chance(r, 30)) {
            s->cores[i] = gencpu(r);
        }
    }
    if (s->nprocs > 0) {
        for (i64 k = rrange(r, 0, 20); k > 0; k--) {
            ProcInfo *p = s->procs + rnd(r, (u64)s->nprocs);
            ProcInfo *q = s->procs + rnd(r, (u64)s->nprocs);
            switch (rnd(r, 4)) {
            case 0: {
                u32 pid = p->pid;
                Str nm  = p->name;
                genproc(r, p, s->ncpu);
                p->pid  = pid;
                p->name = nm;
                break;
            }
            case 1: {
                ProcInfo t = *p;
                *p = *q;
                *q = t;
                break;
            }
            case 2:
                p->name = q->name;  // aliased name
                break;
            default:
                p->pid = chance(r, 50) ? q->pid : (u32)rand64(r);
                break;
            }
        }
        if (chance(r, 15)) {
            s->nprocs = rrange(r, 0, s->nprocs);
        }
    }
}


// Checks

static b32 uniquepids(Snapshot *s) {
    if (s->nprocs > 300) {
        return 0;
    }
    for (iz i = 0; i < s->nprocs; i++) {
        if (!s->procs[i].pid) {
            return 0;
        }
        for (iz j = 0; j < i; j++) {
            if (s->procs[j].pid == s->procs[i].pid) {
                return 0;
            }
        }
    }
    return 1;
}

static b32 refmatch(Ui *ui, ProcInfo *p) {
    Str flt = str(&ui->filter);
    return refcontains(p->name.data, p->name.len, flt.data, flt.len);
}

static iz refcount(Ui *ui, Snapshot *s) {
    iz n = 0;
    for (iz i = 0; i < s->nprocs; i++) {
        n += refmatch(ui, s->procs + i);
    }
    return n;
}

static u64 uihash(Ui *ui) {
    u64 h = 1469598103934665603ULL;
    i64 v[] = {
        ui->delay_ms, ui->sort, ui->reverse, ui->irix, ui->cores, ui->units,
        ui->mode, ui->sel_pid, ui->sel_idx, ui->scroll, ui->move, ui->page,
        ui->sel_name.len, ui->filter.len, ui->edit.len, ui->status.len,
    };
    for (iz i = 0; i < countof(v); i++) {
        h = (h ^ (u64)v[i]) * 1099511628211ULL;
    }
    Buf *bufs[] = {&ui->sel_name, &ui->filter, &ui->edit, &ui->status};
    for (iz i = 0; i < countof(bufs); i++) {
        iz n = min(max(bufs[i]->len, (iz)0), (iz)countof(bufs[i]->d));
        for (iz j = 0; j < n; j++) {
            h = (h ^ bufs[i]->d[j]) * 1099511628211ULL;
        }
    }
    return h;
}

static void check_ui(Ui *ui) {
    char const *c = "ui state";
    if (ui->sel_idx < 0) {
        fail(c, "sel_idx < 0", ui->sel_idx, 0);
    }
    if (ui->scroll < 0) {
        fail(c, "scroll < 0", ui->scroll, 0);
    }
    if (ui->filter.len<0 || ui->filter.len>MAXIN) {
        fail(c, "filter.len out of range", ui->filter.len, MAXIN);
    }
    if (ui->edit.len<0 || ui->edit.len>MAXIN) {
        fail(c, "edit.len out of range", ui->edit.len, MAXIN);
    }
    if (ui->units<0 || ui->units>3) {
        fail(c, "units out of range", ui->units, 0);
    }
    if (ui->mode<PROMPT_NONE || ui->mode>PROMPT_HELP) {
        fail(c, "mode invalid", ui->mode, 0);
    }
    if (ui->status.len<0 || ui->status.len>(iz)countof(ui->status.d)) {
        fail(c, "status.len out of range", ui->status.len, 0);
    }
    if (ui->sel_name.len<0 || ui->sel_name.len>(iz)countof(ui->sel_name.d)) {
        fail(c, "sel_name.len out of range", ui->sel_name.len, 0);
    }
    if (ui->sort<0 || ui->sort>=NCOL) {
        fail(c, "sort column invalid", ui->sort, 0);
    }
    if (ui->page < 1) {
        fail(c, "page < 1", ui->page, 0);
    }
    if (ui->delay_ms < 100) {
        fail(c, "delay below 100 ms", ui->delay_ms, 0);
    }
    if (ui->move>(1<<20) || ui->move<-(1<<20)) {
        fail(c, "move unbounded", ui->move, 0);
    }
    if (!validutf8(ui->filter.d, ui->filter.len)) {
        fail(c, "filter is not valid UTF-8", ui->filter.len, 0);
    }
    if (!validutf8(ui->edit.d, ui->edit.len)) {
        fail(c, "edit is not valid UTF-8", ui->edit.len, 0);
    }
}

// After a main-screen render the selection must be coherent.
static void check_selection(Ui *ui, Snapshot *s) {
    char const *c = "selection";
    iz n = refcount(ui, s);
    if (ui->move != 0) {
        fail(c, "move not consumed by render", ui->move, 0);
    }
    if (ui->sel_idx >= max(n, (iz)1)) {
        fail(c, "sel_idx past the last row", ui->sel_idx, n);
    }
    if (ui->scroll > ui->sel_idx) {
        fail(c, "scroll below the selection", ui->scroll, ui->sel_idx);
    }
    b32 found = n == 0 && ui->sel_pid == 0;
    for (iz i = 0; i < s->nprocs && !found; i++) {
        found = refmatch(ui, s->procs+i) && s->procs[i].pid==ui->sel_pid;
    }
    if (!found) {
        fail(c, "sel_pid is not the pid of a matching process", ui->sel_pid, n);
    }
}

static i32 rowlimit(i64 row, i32 width, i32 height) {
    return max(row==height-1 ? width-1 : width, 0);
}

// An interactive frame: \e[H, rows split by \r\n, \e[J.
static void check_frame(Str f, i32 width, i32 height) {
    char const *c = "frame";
    if (f.len<6 || !equals((Str){f.data, 3}, S("\x1b[H")) || !equals((Str){f.data+f.len-3, 3}, S("\x1b[J"))) {
        fail(c, "must start with ESC[H and end with ESC[J", f.len, 0);
    }
    Str body = (Str){f.data+3, f.len-6};
    i64 rows = body.len ? 1 : 0;
    i64 col  = 0;
    for (iz i = 0; i < body.len;) {
        u8 b = body.data[i];
        if (b == 0x1b) {
            iz j = i + 2;
            if (i+1>=body.len || body.data[i+1]!='[') {
                fail(c, "ESC not followed by [", rows, i);
            }
            for (; j<body.len && body.data[j]>=0x30 && body.data[j]<=0x3f; j++) {}
            for (; j<body.len && body.data[j]>=0x20 && body.data[j]<=0x2f; j++) {}
            if (j>=body.len || body.data[j]<0x40 || body.data[j]>0x7e) {
                fail(c, "malformed CSI sequence", rows, i);
            }
            i = j + 1;
        } else if (b == '\r') {
            if (i+1>=body.len || body.data[i+1]!='\n') {
                fail(c, "CR without LF", rows, i);
            }
            if (col > rowlimit(rows-1, width, height)) {
                fail(c, "row wider than the screen", rows-1, col);
            }
            rows++;
            col = 0;
            i  += 2;
        } else if (b<0x20 || b==0x7f) {
            fail(c, "raw control byte", b, rows-1);
        } else if (b < 0x80) {
            col++;
            i++;
        } else {
            Str  rest = (Str){body.data+i, body.len-i};
            Utf8 d    = utf8decode(rest);
            iz   used = rest.len - d.tail.len;
            if (d.rune==0xfffd && used==1) {
                fail(c, "invalid UTF-8 in output", b, rows-1);
            }
            if (d.rune>=0x80 && d.rune<0xa0) {
                fail(c, "C1 control character in output", d.rune, rows-1);
            }
            col++;
            i += used;
        }
    }
    if (rows > 0 && col > rowlimit(rows-1, width, height)) {
        fail(c, "row wider than the screen", rows-1, col);
    }
    if (rows > height) {
        fail(c, "more rows than the screen height", rows, height);
    }
}

static Str lineat(Str t, iz index) {
    iz beg = 0;
    for (iz i = 0; i<t.len && index>0; i++) {
        if (t.data[i] == '\n') {
            index--;
            beg = i + 1;
        }
    }
    iz end = beg;
    for (; end<t.len && t.data[end]!='\n'; end++) {}
    return (Str){t.data+beg, end-beg};
}

// Batch output: plain text, one row per matching process.
static void check_batch(Str t, Ui *ui, Snapshot *s) {
    char const *c = "batch";
    if (t.len<2 || !equals((Str){t.data+t.len-2, 2}, S("\n\n"))) {
        fail(c, "must end with a blank line", t.len, 0);
    }
    iz newlines = 0;
    for (iz i = 0; i < t.len;) {
        u8 b = t.data[i];
        if (b == 0x1b) {
            fail(c, "escape byte in plain output", i, 0);
        } else if (b == '\n') {
            newlines++;
            i++;
        } else if (b<0x20 || b==0x7f) {
            fail(c, "raw control byte", b, i);
        } else if (b < 0x80) {
            i++;
        } else {
            Str  rest = (Str){t.data+i, t.len-i};
            Utf8 d    = utf8decode(rest);
            iz   used = rest.len - d.tail.len;
            if (d.rune==0xfffd && used==1) {
                fail(c, "invalid UTF-8 in output", b, i);
            }
            if (d.rune>=0x80 && d.rune<0xa0) {
                fail(c, "C1 control character in output", d.rune, i);
            }
            i += used;
        }
    }
    // Rows: top, tasks, nc CPU, mem, commit, blank, header, n procs, and a
    // newline after every row but the first, then the final "\n\n".
    iz nc   = ui->cores ? max((iz)s->ncpu, (iz)0) : 0;
    iz nrow = newlines - (max(nc, (iz)1) + 7);
    iz want = refcount(ui, s);
    if (nrow != want) {
        fail(c, "table rows differ from matching processes", nrow, want);
    }
    iz hdr = max(nc, (iz)1) + 5;
    if (lineat(t, hdr-1).len != 0) {
        fail(c, "no blank line before the header", hdr-1, 0);
    }
    Str h = lineat(t, hdr);
    if (h.len<4 || !icontains(h, S("PID")) || !icontains(h, S("NAME"))) {
        fail(c, "header line missing", hdr, h.len);
    }
    if (ui->sort == COL_PID) {  // PID column must be ordered
        u64 prev = 0;
        for (iz i = 0; i < nrow; i++) {
            Str row = lineat(t, hdr + 1 + i);
            iz  j   = 0;
            u64 pid = 0;
            for (; j<row.len && row.data[j]==' '; j++) {}
            for (; j<row.len && row.data[j]>='0' && row.data[j]<='9'; j++) {
                pid = pid*10 + (u64)(row.data[j] - '0');
            }
            if (i && (ui->reverse ? pid>prev : pid<prev)) {
                fail(c, "rows not sorted by PID", i, 0);
            }
            prev = pid;
        }
    }
}


// The world: arenas and the output the UI draws to

typedef struct {
    Arena   perm;
    Arena   snap;
    Arena   scratch;
    Arena   ut;       // unit tests
    Arena   rs[2];    // real snapshots
    Arena   big;      // scratch for sample()
    Output *out;
    i64     frames, batches, keys, units;
} World;

static void settle(World *w) {
    poison(w->scratch);
    capt.len = 0;
}

static Str captured() {
    ctx.frame    = capt.buf;
    ctx.framelen = capt.len;
    return (Str){capt.buf, capt.len};
}

static void render(World *w, Ui *ui, Snapshot *s, i32 width, i32 height, b32 idem) {
    ctx.phase = "render";
    settle(w);
    ui_render(ui, s, w->out, width, height, w->scratch);
    flush(w->out);
    if (w->out->err) {
        fail("render", "capture buffer overflow", capt.len, 0);
    }
    Str f = captured();
    check_frame(f, width, height);
    check_ui(ui);
    if (ui->mode != PROMPT_HELP) {
        check_selection(ui, s);
    }
    w->frames++;

    if (idem) {  // a second render of nothing new changes nothing but the clock
        u64 h = uihash(ui);
        iz  n = capt.len;
        settle(w);
        capt.len = n;
        ui_render(ui, s, w->out, width, height, w->scratch);
        flush(w->out);
        Str g = (Str){capt.buf+n, capt.len-n};
        iz  i = 0;
        iz  j = 0;
        for (; i+1<f.len && !(f.data[i]=='\r' && f.data[i+1]=='\n'); i++) {}
        for (; j+1<g.len && !(g.data[j]=='\r' && g.data[j+1]=='\n'); j++) {}
        ctx.frame    = g.data;
        ctx.framelen = g.len;
        if (uihash(ui) != h) {
            fail("render", "second render changed the UI state", 0, 0);
        }
        if (i+1<f.len && j+1<g.len) {
            Str a = (Str){f.data+i, f.len-i};
            Str b = (Str){g.data+j, g.len-j};
            if (!equals(a, b)) {
                fail("render", "second render differs below the first row", a.len, b.len);
            }
        }
        w->frames++;
    }
}

static void batch(World *w, Ui *ui, Snapshot *s) {
    ctx.phase = "batch";
    u64 h = uihash(ui);
    settle(w);
    ui_batch(ui, s, w->out, w->scratch);
    flush(w->out);
    if (w->out->err) {
        fail("batch", "capture buffer overflow", capt.len, 0);
    }
    Str t = captured();
    check_batch(t, ui, s);
    if (uihash(ui) != h) {
        fail("batch", "ui_batch changed the UI state", 0, 0);
    }
    w->batches++;
}

// Feed one key and check everything we can say about it.
static void feed(World *w, Ui *ui, i32 key) {
    ctx.keys[ctx.nkeys++ % countof(ctx.keys)] = key;
    ctx.phase = "key";
    w->keys++;

    i32 mode    = ui->mode;
    u32 selpid  = ui->sel_pid;
    i32 delay   = ui->delay_ms;
    u8  in[MAXIN];
    iz  inlen   = ui->edit.len;
    if (inlen<0 || inlen>MAXIN) {
        fail("ui state", "edit.len out of range before key", inlen, 0);
    }
    memcpy(in, ui->edit.d, (uz)inlen);
    i32 kills0 = killed.count;

    i32 res = ui_key(ui, key);
    check_ui(ui);

    b32 wantquit = key==3 || (key=='q' && mode==PROMPT_NONE);
    if (wantquit != !!(res & UI_QUIT)) {
        fail("key", wantquit ? "quit key did not quit" : "spurious UI_QUIT", key, res);
    }

    i64 v     = 0;
    i32 kills = killed.count - kills0;
    if (mode==PROMPT_KILL && key==13) {
        i32 pid = (i32)selpid;
        b32 ok  = 1;
        if (inlen) {
            ok = refint(in, inlen, &v);
            pid = (i32)v;
        }
        b32 want = ok && pid;
        if (kills != want) {
            fail("kill", "os_kill called the wrong number of times", kills, want);
        }
        if (want && killed.last != (u32)pid) {
            fail("kill", "os_kill got the wrong pid", killed.last, pid);
        }
        if (!ui->status.len) {
            fail("kill", "no status after a kill prompt", 0, 0);
        }
        if (want) {  // the status names the pid as an unsigned number
            u8 num[24];
            iz n = refi64(num + sizeof(num), (i64)(u32)pid);
            if (!icontains(str(&ui->status), (Str){num + sizeof(num) - n, n})) {
                ctx.frame    = ui->status.d;
                ctx.framelen = ui->status.len;
                fail("kill", "status does not show the pid as unsigned", (u32)pid, pid);
            }
        }
    } else if (kills) {
        fail("kill", "os_kill called without a committed kill prompt", key, kills);
    }
    if (mode==PROMPT_DELAY && key==13) {
        b32 ok = refdelay(in, inlen, &v);
        if (ui->delay_ms != (ok ? v : delay)) {
            fail("delay", "wrong delay after commit", ui->delay_ms, ok ? v : delay);
        }
    } else if (ui->delay_ms != delay) {
        fail("delay", "delay changed without a delay commit", ui->delay_ms, delay);
    }
}

static Snapshot *newsnap(World *w, u64 *r) {
    ctx.phase = "gensnap";
    Arena a = w->snap;
    poison(a);
    static i32 const cpus[] = {0, 1, 2, 4, 7, 64, 256};
    i32 ncpu = cpus[rnd(r, (u64)countof(cpus))];
    iz  nprocs = 0;
    switch (WEIGHTED(r, 9, 9, 40, 18, 12, 12)) {
    case 0:  nprocs = 0; break;
    case 1:  nprocs = 1; break;
    case 2:  nprocs = rrange(r, 2, 9); break;
    case 3:  nprocs = rrange(r, 10, 60); break;
    case 4:  nprocs = 300; break;
    default: nprocs = chance(r, 20) ? 10000 : rrange(r, 61, 299); break;
    }
    ctx.ncpu   = ncpu;
    ctx.nprocs = nprocs;
    return gensnap(&a, r, ncpu, nprocs, nprocs > 300);
}

// Unit tests, see below.
static void unit_round(World *w, u64 *r);

static void iteration(World *w, u64 seed, i64 iter) {
    u64  st = seed ^ ((u64)(iter + 1) * 0x9e3779b97f4a7c15ULL);
    u64 *r  = &st;
    rand64(r);
    rand64(r);
    ctx.iter     = iter;
    ctx.nkeys    = 0;
    ctx.frame    = 0;
    ctx.framelen = 0;
    killed.rng   = rand64(r);

    unit_round(w, r);

    Arena perm = w->perm;
    Ui   *ui   = ui_new(&perm, 2000);
    i32   width = 0, height = 0;
    gensize(r, &width, &height);
    ctx.width  = width;
    ctx.height = height;
    Snapshot *s = newsnap(w, r);
    b32 unique  = uniquepids(s);

    i32 keys[512];
    for (i64 steps = rrange(r, 3, 30); steps > 0; steps--) {
        ctx.frame    = 0;
        ctx.framelen = 0;
        switch (WEIGHTED(r, 8, 12, 8, 5, 6, 61)) {
        case 0:
            s      = newsnap(w, r);
            unique = uniquepids(s);
            break;
        case 1:
            mutate(r, s);
            unique = uniquepids(s);
            ctx.nprocs = s->nprocs;
            break;
        case 2:
            gensize(r, &width, &height);
            ctx.width  = width;
            ctx.height = height;
            break;
        case 3:
            batch(w, ui, s);
            continue;
        case 4:
            if (unique && ui->mode==PROMPT_NONE) {  // selection model
                render(w, ui, s, width, height, 0);
                i64 n    = refcount(ui, s);
                i64 idx  = ui->sel_idx;
                i64 page = ui->page;
                i32 key  = navkeys[rnd(r, 8)];  // all but DELETE
                i64 want = idx;
                switch (key) {
                case KEY_UP:   want = idx - 1; break;
                case KEY_DOWN: want = idx + 1; break;
                case KEY_PGUP: want = idx - page; break;
                case KEY_PGDN: want = idx + page; break;
                case KEY_HOME: want = 0; break;
                case KEY_END:  want = n - 1; break;
                }
                want = max((i64)0, min(want, n - 1));
                feed(w, ui, key);
                render(w, ui, s, width, height, 0);
                if (ui->sel_idx != want && (key==KEY_UP || key==KEY_DOWN || key==KEY_PGUP ||
                                            key==KEY_PGDN || key==KEY_HOME || key==KEY_END)) {
                    fail("navigation", "selection moved to the wrong row", ui->sel_idx, want);
                }
                continue;
            }
            // fallthrough
        default: {
            iz n = genkeys(r, keys, countof(keys));
            for (iz i = 0; i < n; i++) {
                feed(w, ui, keys[i]);
            }
            break;
        }
        }
        render(w, ui, s, width, height, unique && chance(r, 40));
    }
}


// Unit tests

static Str gstr(Arena *a, u8 const *src, iz n) {
    u8 *p = new(a, n, u8);
    if (n) {
        memcpy(p, src, (uz)n);
    }
    return (Str){p, n};
}

// Every scalar value round-trips, with the bytes at the very end of a guard.
static void test_utf8_all(World *w) {
    ctx.phase = "utf8 exhaustive";
    Arena a = w->ut;
    u8 *g   = new(&a, 4, u8);
    for (i32 cp = 0; cp <= 0x10ffff; cp++) {
        if (cp>=0xd800 && cp<=0xdfff) {
            continue;
        }
        u8  ref[4];
        iz  rn = refenc(ref, cp);
        u8 *p  = g + 4 - rn;
        iz  n  = utf8encode(p, cp);
        if (n != rn) {
            fail(ctx.phase, "utf8encode length", cp, n);
        }
        for (iz i = 0; i < n; i++) {
            if (p[i] != ref[i]) {
                fail(ctx.phase, "utf8encode bytes", cp, i);
            }
        }
        Utf8 d = utf8decode((Str){p, n});
        if (d.rune!=cp || d.tail.len!=0 || d.tail.data!=p+n) {
            fail(ctx.phase, "utf8decode round trip", cp, d.rune);
        }
    }
    static i32 const bad[] = {0xd800, 0xdbff, 0xdc00, 0xdfff, -1, 0x110000, I32_MAX, I32_MIN};
    for (iz i = 0; i < countof(bad); i++) {
        u8 *p = g + 1;
        if (utf8encode(p, bad[i])!=3 || p[0]!=0xef || p[1]!=0xbf || p[2]!=0xbd) {
            fail(ctx.phase, "invalid scalar must encode as U+FFFD", bad[i], 0);
        }
    }
}

static void test_utf8_random(World *w, u64 *r) {
    ctx.phase = "utf8 random";
    Arena a = w->ut;
    static u8 const interesting[] = {
        0x80, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf, 0xe0, 0xed, 0xef, 0xf0, 0xf4, 0xf5,
        0xff, 0xa0, 0x9f, 0x90, 0x8f, 0x41, 0x00,
    };
    u8 tmp[40];
    iz n = 0;
    for (iz want = rrange(r, 1, 10); n < want;) {
        switch (rnd(r, 3)) {
        case 0:  tmp[n++] = (u8)rand64(r); break;
        case 1:  tmp[n++] = interesting[rnd(r, (u64)countof(interesting))]; break;
        default: n += utf8encode(tmp+n, gencp(r)); break;
        }
    }
    Str s = gstr(&a, tmp, n);
    while (s.len) {
        i32  cp   = 0;
        iz   rl   = refdecode(s.data, s.len, &cp);
        Utf8 d    = utf8decode(s);
        iz   used = s.len - d.tail.len;
        if (used<1 || used>4 || used>s.len || d.tail.data!=s.data+used) {
            fail(ctx.phase, "utf8decode consumed an impossible amount", used, s.len);
        }
        if (rl && (used!=rl || d.rune!=cp)) {
            fail(ctx.phase, "valid sequence decoded wrongly", used, rl);
        }
        if (!rl && (used!=1 || d.rune!=0xfffd)) {
            fail(ctx.phase, "invalid input must give one U+FFFD per byte", used, d.rune);
        }
        s = d.tail;
    }
}

static void test_utf16(World *w, u64 *r) {
    ctx.phase = "utf16";
    Arena a = w->ut;
    c16 units[96];
    iz  nu   = 0;
    b32 lone = chance(r, 50);
    for (iz want = rrange(r, 0, 40); want > 0; want--) {
        switch (rnd(r, lone ? 6 : 4)) {
        case 0:
        case 1:
            units[nu++] = (c16)(chance(r, 50) ? rrange(r, 0, 0xd7ff) : rrange(r, 0xe000, 0xffff));
            break;
        case 2:
        case 3: {
            i32 v = (i32)rrange(r, 0, 0xfffff);
            units[nu++] = (c16)(0xd800 + v/1024);
            units[nu++] = (c16)(0xdc00 + v%1024);
            break;
        }
        default:
            units[nu++] = (c16)rrange(r, 0xd800, 0xdfff);
            break;
        }
    }
    // Reference: code points, lone surrogates replaced.
    i32 cps[96];
    iz  nc      = 0;
    b32 hadlone = 0;
    for (iz i = 0; i < nu;) {
        i32 u = units[i];
        if (u>=0xd800 && u<=0xdbff && i+1<nu && units[i+1]>=0xdc00 && units[i+1]<=0xdfff) {
            cps[nc++] = 0x10000 + (u - 0xd800)*1024 + (units[i+1] - 0xdc00);
            i += 2;
        } else if (u>=0xd800 && u<=0xdfff) {
            cps[nc++] = 0xfffd;
            hadlone   = 1;
            i++;
        } else {
            cps[nc++] = u;
            i++;
        }
    }
    c16 *g = new(&a, nu, c16);
    for (iz i = 0; i < nu; i++) {
        g[i] = units[i];
    }
    Str s = fromutf16(&a, g, nu);
    iz  pos = 0;
    for (iz i = 0; i < nc; i++) {
        i32 cp = 0;
        iz  l  = pos<s.len ? refdecode(s.data+pos, s.len-pos, &cp) : 0;
        if (!l || cp!=cps[i]) {
            fail(ctx.phase, "fromutf16 produced the wrong code point", i, cps[i]);
        }
        pos += l;
    }
    if (pos != s.len) {
        fail(ctx.phase, "fromutf16 produced extra bytes", pos, s.len);
    }
    c16 *dst = new(&a, s.len, c16);
    iz   m   = toutf16(dst, s);
    iz   k   = 0;
    for (iz i = 0; i < nc; i++) {
        if (cps[i] >= 0x10000) {
            i32 v = cps[i] - 0x10000;
            if (k+1>=m || dst[k]!=0xd800+v/1024 || dst[k+1]!=0xdc00+v%1024) {
                fail(ctx.phase, "toutf16 surrogate pair wrong", i, k);
            }
            k += 2;
        } else {
            if (k>=m || dst[k]!=cps[i]) {
                fail(ctx.phase, "toutf16 unit wrong", i, k);
            }
            k++;
        }
    }
    if (k != m) {
        fail(ctx.phase, "toutf16 unit count wrong", m, k);
    }
    if (!hadlone) {
        if (m != nu) {
            fail(ctx.phase, "UTF-16 round trip changed the length", m, nu);
        }
        for (iz i = 0; i < nu; i++) {
            if (dst[i] != units[i]) {
                fail(ctx.phase, "UTF-16 round trip changed a unit", i, 0);
            }
        }
    }
}

typedef struct {
    char const *s;
    b32         intok;
    i32         intval;
    b32         delayok;
    i32         delayms;
} ParseCase;

static ParseCase const pcases[] = {
    {"",                0, 0,          0, 0},
    {"0",               1, 0,          1, 100},
    {"-1",              0, 0,          0, 0},
    {"2147483647",      1, I32_MAX,    0, 0},
    {"2147483648",      0, 0,          0, 0},
    {"99999999999",     0, 0,          0, 0},
    {"1.",              0, 0,          1, 1000},
    {".5",              0, 0,          1, 500},
    {".",               0, 0,          0, 0},
    {"0.05",            0, 0,          1, 100},
    {"1.2345",          0, 0,          1, 1234},
    {"abc",             0, 0,          0, 0},
    {"1e3",             0, 0,          0, 0},
    {" 1",              0, 0,          0, 0},
    {"1 ",              0, 0,          0, 0},
    {"007",             1, 7,          1, 7000},
    {"2",               1, 2,          1, 2000},
    {"0.1",             0, 0,          1, 100},
    {"0.25",            0, 0,          1, 250},
    {"1000000",         1, 1000000,    1, 1000000000},
    {"1000001",         1, 1000001,    0, 0},
    {"000000000000000000000000000001", 1, 1, 1, 1000},
    {"1.5.5",           0, 0,          0, 0},
    {"..5",             0, 0,          0, 0},
    {"+1",              0, 0,          0, 0},
};

static void test_parse_table(World *w) {
    ctx.phase = "parse table";
    for (iz i = 0; i < countof(pcases); i++) {
        Arena a = w->ut;
        ParseCase const *c = pcases + i;
        Str s  = cstr(c->s);
        Str g  = gstr(&a, s.data, s.len);
        i32 v  = -7;
        b32 ok = parseint(g, &v);
        if (ok != c->intok || (ok && v != c->intval)) {
            fail("parseint", c->s, ok, v);
        }
        v  = -7;
        ok = parsedelay(g, &v);
        if (ok != c->delayok || (ok && v != c->delayms)) {
            fail("parsedelay", c->s, ok, v);
        }
    }
}

static void test_parse_random(World *w, u64 *r) {
    ctx.phase = "parse random";
    Arena a = w->ut;
    u8 tmp[32];
    iz n = chance(r, 15) ? rrange(r, 15, 30) : rrange(r, 0, 12);
    for (iz i = 0; i < n; i++) {
        tmp[i] = chance(r, 75) ? (u8)rrange(r, '0', '9') : (u8)"-.e +x\t,"[rnd(r, 8)];
    }
    Str g = gstr(&a, tmp, n);
    i64 want = 0;
    i32 v    = 0;
    b32 ok   = parseint(g, &v);
    if (ok != refint(tmp, n, &want) || (ok && v != want)) {
        fail("parseint", "disagrees with reference", ok, v);
    }
    ok = parsedelay(g, &v);
    if (ok != refdelay(tmp, n, &want) || (ok && v != want)) {
        fail("parsedelay", "disagrees with reference", ok, v);
    }
}

static void test_icontains(World *w, u64 *r) {
    ctx.phase = "icontains";
    Arena a = w->ut;
    static u8 const alpha[] = {'a', 'A', 'b', 'B', 'c', 'C', ' ', '1', '.', 0xc3, 0xa9, '@', '[', '`', '{'};
    u8 h[40];
    u8 nd[8];
    iz hn = rrange(r, 0, 30);
    iz nn = rrange(r, 0, 6);
    for (iz i = 0; i < hn; i++) {
        h[i] = alpha[rnd(r, (u64)countof(alpha))];
    }
    for (iz i = 0; i < nn; i++) {
        nd[i] = alpha[rnd(r, (u64)countof(alpha))];
    }
    if (hn>=nn && chance(r, 50)) {  // a needle that is there, in other case
        iz at = rrange(r, 0, hn - nn);
        for (iz i = 0; i < nn; i++) {
            u8 c = h[at+i];
            nd[i] = chance(r, 50) && c>='a' && c<='z' ? (u8)(c - 32) : c;
        }
    }
    Str gh = gstr(&a, h, hn);
    Str gn = gstr(&a, nd, nn);
    b32 got  = icontains(gh, gn);
    b32 want = refcontains(h, hn, nd, nn);
    if (got != want) {
        fail(ctx.phase, "disagrees with reference", got, want);
    }
}

static void expect(Str want, char const *what, i64 a) {
    Str got = (Str){capt.buf, capt.len};
    if (!equals(got, want)) {
        ctx.frame    = capt.buf;
        ctx.framelen = capt.len;
        fail(ctx.phase, what, a, capt.len);
    }
    capt.len     = 0;
    ctx.framelen = 0;
}

static void check_print_i64(World *w, i64 v, iz outcap) {
    Arena a = w->ut;
    u8 ref[24];
    iz n = refi64(ref + sizeof(ref), v);
    capt.len = 0;
    Output *o = newoutput(&a, CAPTURE, 0, outcap);
    printi64(o, v);
    flush(o);
    expect((Str){ref + sizeof(ref) - n, n}, "print(i64) formatted wrongly", v);
}

static void check_print_f64(World *w, f64 v, i32 prec, iz outcap) {
    Arena a = w->ut;
    capt.len = 0;
    Output *o = newoutput(&a, CAPTURE, 0, outcap);
    printf64(o, v, prec);
    flush(o);
    Str s = (Str){capt.buf, capt.len};
    ctx.frame    = capt.buf;
    ctx.framelen = capt.len;
    iz  i   = 0;
    b32 neg = s.len && s.data[0]=='-';
    if (neg != (v < 0)) {
        fail("print(f64)", "wrong sign", neg, prec);
    }
    i += neg;
    u64 ip = 0;
    iz  nd = 0;
    for (; i<s.len && s.data[i]>='0' && s.data[i]<='9'; i++, nd++) {
        ip = ip*10 + (u64)(s.data[i] - '0');
    }
    if (!nd || nd>17) {
        fail("print(f64)", "bad integer part", nd, prec);
    }
    f64 got = (f64)(i64)ip;  // at most 17 digits, see above
    f64 sc  = 1;
    if (prec > 0) {
        if (i>=s.len || s.data[i++]!='.') {
            fail("print(f64)", "missing decimal point", prec, 0);
        }
        iz fd = 0;
        f64 f = 0;
        for (; i<s.len && s.data[i]>='0' && s.data[i]<='9'; i++, fd++) {
            f  = f*10 + (f64)(s.data[i] - '0');
            sc *= 10;
        }
        if (fd != prec) {
            fail("print(f64)", "wrong number of fraction digits", fd, prec);
        }
        got += f / sc;
    }
    if (i != s.len) {
        fail("print(f64)", "trailing junk", i, s.len);
    }
    f64 mag = v<0 ? -v : v;
    if (mag == mag) {  // not NaN
        f64 lim  = 9e18 / sc;
        f64 want = min(mag, min(1e15, lim));
        f64 tol  = 0.5/sc + want*1e-15 + 1e-9;
        if (got-want > tol || want-got > tol) {
            fail("print(f64)", "value is off", (i64)got, (i64)want);
        }
    }
    capt.len     = 0;
    ctx.framelen = 0;
}

static void test_print(World *w) {
    ctx.phase = "print table";
    check_print_i64(w, I64_MIN, 4096);
    check_print_i64(w, I64_MAX, 4096);
    check_print_i64(w, 0, 4096);
    check_print_i64(w, -1, 4096);
    {
        Arena a = w->ut;
        capt.len = 0;
        Output *o = newoutput(&a, CAPTURE, 0, 4096);
        printi64(o, I64_MIN);
        printc(o, ' ');
        printi64(o, I64_MAX);
        printc(o, ' ');
        printi64(o, (i64)0);
        printc(o, ' ');
        printi64(o, (i64)-1);
        flush(o);
        expect(S("-9223372036854775808 9223372036854775807 0 -1"), "print(i64) literals", 0);
    }
    f64 nanv = nanf64();
    f64 infv = inff64();
    f64 fs[] = {nanv, infv, -infv, 0.0, -0.0, 1.5, -1.25, 1e300, -1e300, 1e15, 999999999999999.9, 5e-324};
    for (iz i = 0; i < countof(fs); i++) {
        for (i32 prec = 0; prec <= 9; prec++) {
            check_print_f64(w, fs[i], prec, 4096);
        }
    }
    {
        Arena a = w->ut;
        capt.len = 0;
        Output *o = newoutput(&a, CAPTURE, 0, 4096);
        printf64(o, 1.5, 1);
        printc(o, ' ');
        printf64(o, 0.0, 2);
        printc(o, ' ');
        printf64(o, -2.0, 0);
        flush(o);
        expect(S("1.5 0.00 -2"), "print(f64) literals", 0);
    }
}

static void test_print_random(World *w, u64 *r) {
    ctx.phase = "print random";
    iz outcap = chance(r, 50) ? rrange(r, 1, 12) : 4096;
    check_print_i64(w, geni64(r), outcap);
    f64 v;
    switch (rnd(r, 4)) {
    case 0:  v = genfrac(r); break;
    case 1:  v = (f64)geni64(r); break;
    case 2:  v = randf(r) * 1000; break;
    default: v = -randf(r) * 1e14; break;
    }
    check_print_f64(w, v, (i32)rrange(r, 0, 3), outcap);
}

// Hostile kernel buffers for procat, walked the way sample() walks them.
static void test_procat(World *w, u64 *r) {
    ctx.phase = "procat";
    Arena a    = w->ut;
    iz    size = 8 * rrange(r, 0, chance(r, 70) ? 120 : 600);
    u64  *mem  = new(&a, size/8, u64);  // ends exactly at the guard page
    u8   *b    = (u8 *)mem;
    if (chance(r, 20)) {
        for (iz i = 0; i < size/8; i++) {
            mem[i] = rand64(r);
        }
    }
    iz off = 0;
    for (i64 k = rrange(r, 0, 8); k>0 && off+8<=size; k--) {
        iz  at   = (chance(r, 15) ? size - (iz)sizeof(NtProcessInfo) + 8*rrange(r, -2, 2) : off) & ~(iz)7;
        if (at<0 || at+(iz)sizeof(NtProcessInfo)>size) {
            for (iz i = max(at, (iz)0); i < size; i++) {  // a truncated final entry
                b[i] = (u8)rand64(r);
            }
            break;
        }
        NtProcessInfo *p = (NtProcessInfo *)(b + at);
        p->pid        = (uptr)rrange(r, 0, 99999);
        p->nthreads   = (u32)rrange(r, 0, 100);
        p->nhandles   = (u32)rand64(r);
        p->usertime   = geni64(r);
        p->workingset = (uz)rand64(r);
        iz rest = size - (at + (iz)sizeof(*p));  // bytes after this entry
        iz nlen = 0;
        switch (rnd(r, 10)) {
        case 0:  // no name
            p->name = (UnicodeString){0};
            break;
        case 1:  // a name in the room after the entry
        case 2:
        case 7:
            nlen = rest>=2 ? 2*rrange(r, 1, min(rest/2, (iz)30)) : 0;
            p->name = (UnicodeString){(u16)nlen, (u16)nlen, (c16 *)(b + size - nlen - (rnd(r, 2) ? 0 : min(rest - nlen, (iz)8)))};
            break;
        case 3:  // one unit too long
            nlen = 2*rrange(r, 1, 10);
            p->name = (UnicodeString){(u16)nlen, (u16)nlen, (c16 *)(b + size - nlen + 2)};
            break;
        case 4:  // before the buffer
            p->name = (UnicodeString){8, 8, (c16 *)((uptr)b - 16)};
            break;
        case 5:  // odd address
            p->name = (UnicodeString){4, 4, (c16 *)((uptr)b + at + sizeof(*p) + 1)};
            break;
        case 6: {  // garbage pointer
            // Initializer evaluation order is unspecified in C, so draw
            // explicitly, left to right.
            u16  len = (u16)rand64(r);
            uptr ptr = (uptr)rand64(r);
            p->name  = (UnicodeString){len, 0, (c16 *)ptr};
            break;
        }
        case 8: {  // at the very end, or null, with a length
            u16 len = (u16)(2*rrange(r, 1, 4));
            p->name = (UnicodeString){len, 0, rnd(r, 2) ? (c16 *)(b + size) : 0};
            break;
        }
        default:  // odd byte length, in range
            nlen = rest>=8 ? 5 : 0;
            p->name = (UnicodeString){(u16)nlen, 0, (c16 *)(b + size - 8)};
            break;
        }
        switch (WEIGHTED(r, 40, 10, 10, 10, 10, 10, 10)) {
        case 0:  p->next = (u32)(sizeof(*p) + 8*rrange(r, 0, 20)); break;
        case 1:  p->next = 0; break;
        case 2: {  // unaligned
            // Two draws in one expression have no defined order, so sequence
            // them explicitly, the way gcc does.
            i64 words = rrange(r, 0, 5);
            i64 skew  = rrange(r, 1, 7);
            p->next = (u32)(sizeof(*p) + 8*words + skew);
            break;
        }
        case 3:  p->next = (u32)(size + 8*rrange(r, 0, 10)); break;                          // past the end
        case 4:  p->next = 0xfffffff8u; break;
        case 5:  p->next = U32_MAX; break;
        default: p->next = (u32)rand64(r); break;
        }
        off = at + p->next;
        if (off<0 || off>size) {
            break;
        }
    }

    if (size>=512 && chance(r, 35)) {  // a list whose last entry is cut short
        iz tail = size - (iz)sizeof(NtProcessInfo) + 8*rrange(r, -3, 2);
        NtProcessInfo *p = (NtProcessInfo *)b;
        p->name = (UnicodeString){0};
        p->next = (u32)tail;
        for (iz i = tail; i < size; i++) {
            b[i] = 0;  // no name, no next: only the bounds decide
        }
    }

    ProcList l = {b, b + size};
    if (chance(r, 5)) {
        l = (ProcList){0};
    }
    iz  steps = 0;
    u64 sink  = 0;
    for (iz at = 0;;) {
        NtProcessInfo *p = procat(l, at);
        if (!p) {
            break;
        }
        uptr lo = (uptr)p;
        if (lo<(uptr)l.beg || lo+sizeof(*p)>(uptr)l.end) {
            fail(ctx.phase, "entry lies outside the buffer", at, size);
        }
        if (p->name.length) {
            uptr nm = (uptr)p->name.buffer;
            if (nm<(uptr)l.beg || nm+p->name.length>(uptr)l.end || nm%2) {
                fail(ctx.phase, "name lies outside the buffer", at, p->name.length);
            }
            Str s = fromutf16(&a, p->name.buffer, p->name.length/2);
            sink += (u64)s.len;
        }
        sink += p->pid + p->ppid + p->nthreads + p->nhandles + (u64)p->usertime +
                (u64)p->kerneltime + p->workingset + p->privatebytes + (u64)p->createtime;
        if (++steps > size/8 + 1) {
            fail(ctx.phase, "walk does not terminate", steps, size);
        }
        if (!p->next) {
            break;
        }
        at += p->next;
    }
    if (sink == 0xdeadbeef) {
        capt.len = 0;  // keep sink alive
    }
}

static void unit_round(World *w, u64 *r) {
    ctx.phase = "unit";
    for (i32 i = 0; i < 8; i++) {
        test_utf8_random(w, r);
        test_parse_random(w, r);
        test_icontains(w, r);
    }
    for (i32 i = 0; i < 3; i++) {
        test_utf16(w, r);
        test_print_random(w, r);
    }
    test_procat(w, r);
    w->units += 8*3 + 3*2 + 1;
}

// The real thing, once: sample twice and draw it.
static void test_real(World *w) {
    ctx.phase = "real sample";
    Arena big = w->big;
    poison(big);
    Arena a0 = w->rs[0];
    Snapshot *s0 = sample(&a0, big, 0);
    poison(big);
    Arena a1 = w->rs[1];
    Snapshot *s1 = sample(&a1, big, s0);
    ctx.ncpu   = s1->ncpu;
    ctx.nprocs = s1->nprocs;
    if (s1->ncpu<1 || s1->nprocs<1) {
        fail(ctx.phase, "sample() returned nothing", s1->ncpu, s1->nprocs);
    }
    Arena perm = w->perm;
    Ui   *ui   = ui_new(&perm, 2000);
    static i32 const sizes[][2] = {{80, 24}, {120, 40}, {40, 10}, {200, 60}, {1, 1}, {0, 0}};
    for (iz i = 0; i < countof(sizes); i++) {
        ctx.width  = sizes[i][0];
        ctx.height = sizes[i][1];
        ui->cores  = i & 1;
        render(w, ui, s1, sizes[i][0], sizes[i][1], 1);
        feed(w, ui, KEY_DOWN);
        feed(w, ui, KEY_PGDN);
    }
    ui->cores = 1;
    feed(w, ui, '/');
    feed(w, ui, 'e');
    feed(w, ui, 13);
    batch(w, ui, s1);
    ui->mode = PROMPT_NONE;
    ui->filter.len = 0;
    batch(w, ui, s1);
}

static void startup_tests(World *w) {
    ctx.iter = -1;
    test_utf8_all(w);
    test_parse_table(w);
    test_print(w);
    test_real(w);
}


// Command line

typedef struct {
    Str *argv;
    i32  argc;
} Args;

static Args getargs(Arena *perm) {
    c16 *cmd = GetCommandLineW();
    iz   len = 0;
    for (; cmd[len]; len++) {}
    Str  s = fromutf16(perm, cmd, len);

    Args r = {0};
    r.argv = new(perm, s.len/2 + 1, Str);
    for (iz i = 0;;) {
        for (; i<s.len && (at(s, i)==' ' || at(s, i)=='\t'); i++) {}
        if (i == s.len) {
            break;
        }
        b32 quoted = 0;
        u8 *beg    = s.data + i;
        u8 *out    = beg;
        for (; i < s.len; i++) {
            u8 c = at(s, i);
            if (c == '"') {
                quoted = !quoted;
            } else if (!quoted && (c==' ' || c=='\t')) {
                break;
            } else {
                *out++ = c;
            }
        }
        r.argv[r.argc++] = span(beg, out);
    }
    return r;
}

// Decimal or 0x-prefixed hex.
static b32 parseu64(Str s, u64 *v) {
    u64 base = 10;
    if (s.len>2 && s.data[0]=='0' && (s.data[1]=='x' || s.data[1]=='X')) {
        base = 16;
        s    = (Str){s.data+2, s.len-2};
    }
    if (!s.len) {
        return 0;
    }
    u64 x = 0;
    for (iz i = 0; i < s.len; i++) {
        u64 c = s.data[i];
        u64 d = c>='0' && c<='9' ? c - '0' :
                base==16 && c>='a' && c<='f' ? c - 'a' + 10 :
                base==16 && c>='A' && c<='F' ? c - 'A' + 10 : 99;
        if (d >= base) {
            return 0;
        }
        x = x*base + d;
    }
    *v = x;
    return 1;
}

static Str usage(void) {
    return S(
        "usage: top-test [-s SEED] [-n ITERS] [-i START]\n"
        "  -s SEED   random seed, decimal or 0x hex (default 0x5eed)\n"
        "  -n ITERS  iterations to run (default 1500)\n"
        "  -i START  first iteration number, to replay a failure (default 0)\n"
    );
}

enum {
    TOTAL_SIZE   = (iz)128<<20,
    PERM_SIZE    = (iz)24<<20,
    SNAP_SIZE    = (iz)8<<20,
    SCRATCH_SIZE = (iz)1<<20,
    UT_SIZE      = (iz)4<<20,
    REAL_SIZE    = (iz)24<<20,
    BIG_SIZE     = (iz)8<<20,
    CAPTURE_SIZE = (iz)16<<20,
    OUT_SIZE     = (iz)8<<20,
};

static Arena carve(Arena *a, iz size) {
    a->beg = (u8 *)(((uptr)a->beg + PAGE - 1) & ~(uptr)(PAGE - 1));
    assert(size%PAGE == 0);
    assert(size <= a->end - a->beg);
    Arena r = {a->beg, a->beg+size};
    a->beg += size;
    return r;
}

static i32 run(Arena all) {
    World w = {0};
    w.perm    = carve(&all, PERM_SIZE);
    w.snap    = carve(&all, SNAP_SIZE);
    w.scratch = carve(&all, SCRATCH_SIZE);
    w.ut      = carve(&all, UT_SIZE);
    w.rs[0]   = carve(&all, REAL_SIZE);
    w.rs[1]   = carve(&all, REAL_SIZE);
    w.big     = carve(&all, BIG_SIZE);

    capt.buf  = (u8 *)VirtualAlloc(0, (uz)CAPTURE_SIZE, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    capt.size = CAPTURE_SIZE;
    if (!capt.buf) {
        return 3;
    }
    Arena perm = w.perm;
    ctx.out    = newoutput(&perm, GetStdHandle(STD_OUTPUT_HANDLE), 0, 1<<16);
    w.out      = newoutput(&perm, CAPTURE, 0, OUT_SIZE);
    w.perm     = perm;
    AddVectoredExceptionHandler(1, onfault);

    u64 seed  = 0x5eed;
    u64 iters = 600;
    u64 start = 0;
    Args args = getargs(&perm);
    for (i32 i = 1; i < args.argc; i++) {
        Str arg = args.argv[i];
        u64 *dst = 0;
        if (equals(arg, S("-s"))) {
            dst = &seed;
        } else if (equals(arg, S("-n"))) {
            dst = &iters;
        } else if (equals(arg, S("-i"))) {
            dst = &start;
        }
        if (!dst || i+1>=args.argc || !parseu64(args.argv[++i], dst)) {
            print(ctx.out, S("top-test: bad argument: "));
            print(ctx.out, arg);
            print(ctx.out, S("\n"));
            print(ctx.out, usage());
            flush(ctx.out);
            return 2;
        }
    }
    ctx.seed = seed;

    u64 t0 = GetTickCount64();
    startup_tests(&w);
    for (u64 i = 0; i < iters; i++) {
        iteration(&w, seed, (i64)(start + i));
    }
    u64 ms = max(GetTickCount64() - t0, (u64)1);

    Output *o = ctx.out;
    print(o, S("top-test: ok seed=0x"));
    printhex(o, seed, 1);
    print(o, S(" iters="));
    printi64(o, (i64)iters);
    print(o, S(" frames="));
    printi64(o, w.frames);
    print(o, S(" batches="));
    printi64(o, w.batches);
    print(o, S(" keys="));
    printi64(o, w.keys);
    print(o, S(" unit-cases="));
    printi64(o, w.units);
    print(o, S(" time="));
    printf64(o, (f64)(i64)ms / 1000.0, 1);
    print(o, S("s ("));
    printi64(o, (i64)(iters*1000/ms));
    print(o, S(" iters/s)\n"));
    flush(o);
    return 0;
}

void mainCRTStartup(void) {
    u8 *mem = (u8 *)VirtualAlloc(0, (uz)TOTAL_SIZE, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        ExitProcess(3);
    }
    ExitProcess((u32)run((Arena){mem, mem+TOTAL_SIZE}));
}
