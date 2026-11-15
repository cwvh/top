// Base layer: types, arena, strings, UTF conversion, buffered output.
// Freestanding: no CRT, no headers.

typedef unsigned char      u8;
typedef signed   short     i16;
typedef unsigned short     u16;
typedef signed   int       b32;
typedef signed   int       i32;
typedef unsigned int       u32;
typedef signed   long long i64;
typedef unsigned long long u64;
typedef unsigned short     c16;
typedef double             f64;
typedef __PTRDIFF_TYPE__   iz;
typedef __SIZE_TYPE__      uz;
typedef __UINTPTR_TYPE__   uptr;

#if __GNUC__
#  define assert(c)     while (!(c)) __builtin_trap()
#else
#  define assert(c)     while (!(c)) __asm__ volatile ("ud2")
#endif
#define countof(a)      (iz)(sizeof(a) / sizeof(*(a)))
#define offsetof(t, f)  (iz)__builtin_offsetof(t, f)

#define min(a, b)       ((a)<(b) ? (a) : (b))
#define max(a, b)       ((a)>(b) ? (a) : (b))

// GCC may emit calls to these, so define them here rather than import
// them from msvcrt.dll.
void *memset(void *dst, int c, uz len) {
    void *r = dst;
    __asm__ volatile ("rep stosb" : "+D"(dst), "+c"(len) : "a"(c) : "memory");
    return r;
}

void *memcpy(void *dst, void const *src, uz len) {
    void *r = dst;
    __asm__ volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(len) : : "memory");
    return r;
}


// Arena

typedef struct {
    u8 *beg;
    u8 *end;
} Arena;

#ifdef DEBUG
// Platform hook (main.c): make whole pages readable and writable, or
// inaccessible. Only used to build guard pages.
static void os_protect(void *, iz, b32 readwrite);

enum { PAGE = 4096 };
#endif

// Allocate count zero-initialized objects. Traps when out of memory.
#define new(a, n, t) (t *)alloc(a, n, sizeof(t), _Alignof(t))
#ifdef DEBUG
// Electric Fence: the allocation ends exactly at the end of its own run
// of pages, which is followed by a PAGE_NOACCESS guard page, so any write
// or read past the end faults on the offending instruction. Nothing may
// shrink an allocation by moving a->beg back, as that would defeat this.
static void *alloc(Arena *a, iz count, iz size, iz align) {
    assert(align <= PAGE);
    uptr beg  = ((uptr)a->beg + PAGE - 1) & ~(uptr)(PAGE - 1);
    assert(count >= 0);
    assert(beg <= (uptr)a->end);
    iz room = (iz)((uptr)a->end - beg) / PAGE;  // whole pages left
    assert(count <= room*PAGE/size);            // out of memory
    iz npages = (count*size + PAGE - 1) / PAGE;
    assert(npages < room);                      // out of memory, guard included
    u8 *guard = (u8 *)beg + npages*PAGE;
    // Arenas are reused (scratch is passed by value and "freed" by
    // returning), so these pages may still be a guard from an earlier,
    // dead allocation. Reset them before use, then guard our own tail.
    if (npages) {
        os_protect((u8 *)beg, npages*PAGE, 1);
    }
    os_protect(guard, PAGE, 0);
    a->beg = guard + PAGE;
    // Zero-sized: points at the guard, valid as long as nobody touches it.
    // size is a multiple of align, so this is aligned.
    return memset(guard - count*size, 0, (uz)(count*size));
}
#else
static void *alloc(Arena *a, iz count, iz size, iz align) {
    iz pad = (iz)(-(uptr)a->beg & (uptr)(align - 1));
    assert(count >= 0);
    assert(count <= (a->end - a->beg - pad)/size);  // out of memory
    void *r = a->beg + pad;
    a->beg += pad + count*size;
    return memset(r, 0, (uz)(count*size));
}
#endif


// Strings (UTF-8, not null-terminated)

typedef struct {
    u8 *data;
    iz  len;
} Str;

// String literals only: the length comes from the array size, and the
// empty literals on each side make anything else (such as a pointer, whose
// size is not its length) a compile error.
#define S(s) (Str){(u8 *)"" s "", sizeof(s)-1}

static Str span(u8 *beg, u8 *end) {
    return (Str){beg, end - beg};
}

static b32 equals(Str a, Str b) {
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) {
            return 0;
        }
    }
    return 1;
}

static u8 lower(u8 c) {
    return c>='A' && c<='Z' ? (u8)(c + 'a' - 'A') : c;
}

// ASCII case-insensitive substring search.
static b32 icontains(Str hay, Str needle) {
    for (iz i = 0; i+needle.len <= hay.len; i++) {
        iz j = 0;
        for (; j<needle.len && lower(hay.data[i+j])==lower(needle.data[j]); j++) {}
        if (j == needle.len) {
            return 1;
        }
    }
    return 0;
}

static Str clone(Arena *a, Str s) {
    Str r = {0};
    r.data = new(a, s.len, u8);
    r.len  = s.len;
    if (s.len) {
        memcpy(r.data, s.data, (uz)s.len);
    }
    return r;
}

// Parse a non-negative decimal integer that fits in i32.
static b32 parseint(Str s, i32 *v) {
    if (!s.len) {
        return 0;
    }
    i64 r = 0;
    for (iz i = 0; i < s.len; i++) {
        if (s.data[i]<'0' || s.data[i]>'9') {
            return 0;
        }
        r = r*10 + (s.data[i] - '0');
        if (r > 0x7fffffff) {
            return 0;
        }
    }
    *v = (i32)r;
    return 1;
}

// Parse seconds with an optional fraction ("2", "0.5", ".25") into
// milliseconds, clamped to at least 100 ms.
static b32 parsedelay(Str s, i32 *ms) {
    i64 whole  = 0;
    i64 frac   = 0;
    i64 scale  = 1000;
    b32 digits = 0;
    iz  i      = 0;
    for (; i<s.len && s.data[i]>='0' && s.data[i]<='9'; i++) {
        digits = 1;
        whole  = whole*10 + (s.data[i] - '0');
        if (whole > 1000000) {
            return 0;
        }
    }
    if (i<s.len && s.data[i]=='.') {
        for (i++; i<s.len && s.data[i]>='0' && s.data[i]<='9'; i++) {
            digits = 1;
            if (scale > 1) {
                scale /= 10;
                frac  += (s.data[i] - '0') * scale;
            }
        }
    }
    if (!digits || i!=s.len) {
        return 0;
    }
    *ms = (i32)max(whole*1000 + frac, (i64)100);
    return 1;
}


// UTF-8 / UTF-16

typedef struct {
    Str tail;
    i32 rune;
} Utf8;

// Decode one code point from the front of s. Invalid input consumes one
// byte and yields U+FFFD.
static Utf8 utf8decode(Str s) {
    assert(s.len > 0);
    Utf8 r    = {(Str){s.data+1, s.len-1}, 0xfffd};
    u8  *p    = s.data;
    iz   n    = 0;
    i32  low  = 0;
    i32  rune = 0;
    if (p[0] < 0x80) {
        r.rune = p[0];
        return r;
    } else if ((p[0]&0xe0) == 0xc0) {
        n = 2; low = 0x80;    rune = p[0]&0x1f;
    } else if ((p[0]&0xf0) == 0xe0) {
        n = 3; low = 0x800;   rune = p[0]&0x0f;
    } else if ((p[0]&0xf8) == 0xf0) {
        n = 4; low = 0x10000; rune = p[0]&0x07;
    } else {
        return r;
    }
    if (n > s.len) {
        return r;
    }
    for (iz i = 1; i < n; i++) {
        if ((p[i]&0xc0) != 0x80) {
            return r;
        }
        rune = rune<<6 | (p[i]&0x3f);
    }
    if (rune<low || rune>0x10ffff || (rune>=0xd800 && rune<=0xdfff)) {
        return r;
    }
    r.tail = (Str){p+n, s.len-n};
    r.rune = rune;
    return r;
}

// Encode a code point, returning the byte count (1-4).
static iz utf8encode(u8 *s, i32 rune) {
    if (rune<0 || rune>0x10ffff || (rune>=0xd800 && rune<=0xdfff)) {
        rune = 0xfffd;
    }
    if (rune < 0x80) {
        s[0] = (u8)rune;
        return 1;
    } else if (rune < 0x800) {
        s[0] = (u8)(0xc0 | (rune >>  6));
        s[1] = (u8)(0x80 | (rune & 0x3f));
        return 2;
    } else if (rune < 0x10000) {
        s[0] = (u8)(0xe0 | (rune >> 12));
        s[1] = (u8)(0x80 | ((rune >> 6) & 0x3f));
        s[2] = (u8)(0x80 | (rune & 0x3f));
        return 3;
    }
    s[0] = (u8)(0xf0 | (rune >> 18));
    s[1] = (u8)(0x80 | ((rune >> 12) & 0x3f));
    s[2] = (u8)(0x80 | ((rune >>  6) & 0x3f));
    s[3] = (u8)(0x80 | (rune & 0x3f));
    return 4;
}

// Decode the code point at s[*i], joining a surrogate pair, and advance *i.
static i32 utf16next(c16 const *s, iz len, iz *i) {
    i32 rune = s[(*i)++];
    if (rune>=0xd800 && rune<=0xdbff &&
            *i<len && s[*i]>=0xdc00 && s[*i]<=0xdfff) {
        rune = 0x10000 + ((rune - 0xd800)<<10) + (s[(*i)++] - 0xdc00);
    }
    return rune;
}

// Convert UTF-16 to UTF-8. Lone surrogates become U+FFFD. Allocates
// exactly the result size (no over-allocate and trim), so it composes
// with guard pages.
static Str fromutf16(Arena *a, c16 const *s, iz len) {
    Str r = {0};
    u8  tmp[4];
    for (iz i = 0; i < len;) {
        r.len += utf8encode(tmp, utf16next(s, len, &i));
    }
    r.data = new(a, r.len, u8);
    iz n   = 0;
    for (iz i = 0; i < len;) {
        n += utf8encode(r.data+n, utf16next(s, len, &i));
    }
    return r;
}

// Convert UTF-8 to UTF-16, returning the unit count. dst must have room
// for at least s.len units.
static iz toutf16(c16 *dst, Str s) {
    iz n = 0;
    while (s.len) {
        Utf8 r = utf8decode(s);
        s = r.tail;
        if (r.rune >= 0x10000) {
            i32 v = r.rune - 0x10000;
            dst[n++] = (c16)(0xd800 + (v >> 10));
            dst[n++] = (c16)(0xdc00 + (v & 0x3ff));
        } else {
            dst[n++] = (c16)r.rune;
        }
    }
    return n;
}


// Buffered output. Build a whole frame, then flush once.

typedef struct {
    u8  *buf;
    c16 *wbuf;      // UTF-16 staging for console writes, cap units
    iz   len;
    iz   cap;
    uptr handle;
    b32  console;   // write with WriteConsoleW instead of WriteFile
    b32  err;
} Output;

static b32 os_write(Output *);  // platform layer (main.c)

static Output *newoutput(Arena *a, uptr handle, b32 console, iz cap) {
    Output *o  = new(a, 1, Output);
    o->buf     = new(a, cap, u8);
    o->wbuf    = console ? new(a, cap, c16) : 0;
    o->cap     = cap;
    o->handle  = handle;
    o->console = console;
    return o;
}

static void flush(Output *o) {
    if (!o->err && o->len) {
        o->err = !os_write(o);
    }
    o->len = 0;
}

static void print(Output *o, Str s) {
    for (iz off = 0; !o->err && off<s.len;) {
        iz count = min(o->cap - o->len, s.len - off);
        memcpy(o->buf+o->len, s.data+off, (uz)count);
        o->len += count;
        off    += count;
        if (o->len == o->cap) {
            flush(o);
        }
    }
}

static void printc(Output *o, u8 c) {
    print(o, (Str){&c, 1});
}

// Print c repeated n times.
static void printpad(Output *o, u8 c, iz n) {
    for (iz i = 0; i < n; i++) {
        printc(o, c);
    }
}

static void printi64(Output *o, i64 v) {
    u8  buf[32];
    u8 *end = buf + countof(buf);
    u8 *beg = end;
    i64 t   = v<0 ? v : -v;  // negative so INT64_MIN works
    do {
        *--beg = (u8)('0' - t%10);
    } while (t /= 10);
    if (v < 0) {
        *--beg = '-';
    }
    print(o, span(beg, end));
}

// Fixed-point with prec digits after the decimal point.
static void printf64(Output *o, f64 v, i32 prec) {
    if (v < 0) {
        printc(o, '-');
        v = -v;
    }
    prec = max(min(prec, (i32)15), (i32)0);
    i64 scale = 1;
    for (i32 i = 0; i < prec; i++) {
        scale *= 10;
    }
    f64 lim = min(1e15, 9e18/(f64)scale);  // v*scale must fit in an i64
    if (!(v < lim)) {
        v = lim;  // also catches NaN
    }
    i64 x = (i64)(v*(f64)scale + 0.5);
    printi64(o, x/scale);
    if (prec > 0) {
        printc(o, '.');
        i64 f = x % scale;
        for (i64 d = scale/10; d; d /= 10) {
            printc(o, (u8)('0' + f/d%10));
        }
    }
}
