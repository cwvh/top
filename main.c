// top: a top(1) for Windows, built without a CRT
//   $ make                 # top.exe
//   $ make top-debug.exe   # debug info, UBSan traps, guard pages
// Unity build: this file includes all the others.
#include "base.c"
#include "win32.c"
#include "sample.c"
#include "ui.c"

static Str usage(void) {
    return S(
        "usage: top [-b] [-d SECS] [-n N]\n"
        "  -b       batch mode: print plain-text snapshots to stdout\n"
        "  -d SECS  refresh delay in seconds (default 2, minimum 0.1)\n"
        "  -n N     batch mode: exit after N snapshots (default: run forever)\n"
        "  -h       print this message\n"
    );
}


// Platform

static b32 os_write(Output *o) {
    if (!o->console) {
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

    iz len = toutf16(o->wbuf, (Str){o->buf, o->len});
    for (iz off = 0; off < len;) {
        iz count = min(len - off, (iz)1<<15);
        c16 last = o->wbuf[off+count-1];
        if (off+count<len && last>=0xd800 && last<=0xdbff) {
            count--;  // don't split a surrogate pair
        }
        u32 n = 0;
        if (!WriteConsoleW(o->handle, o->wbuf+off, (u32)count, &n, 0)) {
            return 0;
        }
        off += count;
    }
    return 1;
}

#ifdef DEBUG
// Debug memory checking, see alloc() in base.c.
static void os_protect(void *p, iz size, b32 readwrite) {
    u32 old = 0;
    b32 ok  = VirtualProtect(p, (uz)size, readwrite ? PAGE_READWRITE : PAGE_NOACCESS, &old);
    assert(ok);
}

// Fill freed memory with 0xCC so a stale pointer reads obvious junk. The
// region may contain guard pages left by dead allocations, so make it all
// accessible first. alloc() zeroes what it returns, so valid allocations
// never see the 0xCC.
static void poison(Arena a) {
    os_protect(a.beg, a.end - a.beg, 1);
    memset(a.beg, 0xcc, (uz)(a.end - a.beg));
}
#else
static void poison(Arena a) { (void)a; }
#endif

static b32 os_kill(u32 pid) {
    uptr h = OpenProcess(PROCESS_TERMINATE, 0, pid);
    if (!h) {
        return 0;
    }
    b32 r = TerminateProcess(h, 1);
    CloseHandle(h);
    return r;
}

static Output *newstdout(Arena *a, i32 std, iz cap) {
    uptr h    = GetStdHandle(std);
    u32  mode = 0;
    return newoutput(a, h, GetConsoleMode(h, &mode), cap);
}


// Command line

typedef struct {
    Str *argv;
    i32  argc;
} Args;

// Split the command line on whitespace, honoring double quotes (but not
// CommandLineToArgvW's backslash rules). argv[0] is the program.
static Args getargs(Arena *perm) {
    c16 *cmd = GetCommandLineW();
    iz   len = 0;
    for (; cmd[len]; len++) {}
    Str  s = fromutf16(perm, cmd, len);

    Args r = {0};
    r.argv = new(perm, s.len/2 + 1, Str);
    for (iz i = 0;;) {
        for (; i<s.len && (s.data[i]==' ' || s.data[i]=='\t'); i++) {}
        if (i == s.len) {
            break;
        }
        b32 quoted = 0;
        u8 *beg    = s.data + i;
        u8 *out    = beg;  // unquote in place
        for (; i < s.len; i++) {
            u8 c = s.data[i];
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

typedef struct {
    i32 delay_ms;
    i32 iterations;     // batch only; 0 means forever
    b32 batch;
} Config;

// Returns 0 on success, otherwise an exit status (usage already printed).
static i32 parseargs(Config *conf, Args args, Output *err) {
    for (i32 i = 1; i < args.argc; i++) {
        Str arg = args.argv[i];
        if (arg.len<2 || arg.data[0]!='-') {
            print(err, S("top: unexpected argument: "));
            print(err, arg);
            print(err, S("\n"));
            print(err, usage());
            return 2;
        }

        u8  flag  = arg.data[1];
        Str value = {0};
        if (flag=='d' || flag=='n') {
            if (arg.len > 2) {
                value = (Str){arg.data+2, arg.len-2};  // -d2
            } else if (i+1 < args.argc) {
                value = args.argv[++i];              // -d 2
            } else {
                print(err, S("top: missing value for -"));
                printc(err, flag);
                print(err, S("\n"));
                return 2;
            }
        } else if (arg.len != 2) {
            flag = 0;
        }

        switch (flag) {
        case 'b':
            conf->batch = 1;
            break;
        case 'd':
            if (!parsedelay(value, &conf->delay_ms)) {
                print(err, S("top: invalid delay: "));
                print(err, value);
                print(err, S("\n"));
                return 2;
            }
            break;
        case 'n':
            if (!parseint(value, &conf->iterations) || !conf->iterations) {
                print(err, S("top: invalid iteration count: "));
                print(err, value);
                print(err, S("\n"));
                return 2;
            }
            break;
        case 'h':
            print(err, usage());
            return 1;
        default:
            print(err, S("top: unknown option: "));
            print(err, arg);
            print(err, S("\n"));
            print(err, usage());
            return 2;
        }
    }
    return 0;
}


// Main loops

// Snapshots alternate between two arenas so the previous one survives
// while the next is taken.
typedef struct {
    Arena     arenas[2];
    Snapshot *last;
    i32       which;
} Sampler;

static Snapshot *resample(Sampler *s, Arena scratch) {
    Arena a   = s->arenas[s->which];  // copy, i.e. reset this half
    poison(a);                        // the snapshot from two samples ago
    s->which ^= 1;
    s->last   = sample(&a, scratch, s->last);
    return s->last;
}

static i32 runbatch(Config conf, Ui *ui, Sampler *smp, Output *out, Arena scratch) {
    poison(scratch);
    resample(smp, scratch);  // baseline for CPU deltas
    for (i32 i = 0; !conf.iterations || i<conf.iterations; i++) {
        Sleep((u32)conf.delay_ms);
        poison(scratch);
        Snapshot *snap = resample(smp, scratch);
        poison(scratch);
        ui_batch(ui, snap, out, scratch);
        flush(out);
        if (out->err) {
            return 1;  // e.g. closed pipe
        }
    }
    return 0;
}

static i32 translatekey(KeyEventRecord *k) {
    if (k->ch) {
        return k->ch;
    }
    switch (k->vkey) {
    case VK_UP:     return KEY_UP;
    case VK_DOWN:   return KEY_DOWN;
    case VK_LEFT:   return KEY_LEFT;
    case VK_RIGHT:  return KEY_RIGHT;
    case VK_PRIOR:  return KEY_PGUP;
    case VK_NEXT:   return KEY_PGDN;
    case VK_HOME:   return KEY_HOME;
    case VK_END:    return KEY_END;
    case VK_DELETE: return KEY_DELETE;
    }
    return 0;
}

static i32 runtui(Ui *ui, Sampler *smp, Output *out, Output *err, Arena scratch) {
    uptr hin     = GetStdHandle(STD_INPUT_HANDLE);
    u32  inmode  = 0;
    u32  outmode = 0;
    if (!out->console || !GetConsoleMode(hin, &inmode)) {
        print(err, S("top: interactive mode needs a console (try -b)\n"));
        return 1;
    }
    GetConsoleMode(out->handle, &outmode);
    u32 vtmode = outmode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(out->handle, vtmode)) {
        print(err, S("top: console does not support virtual terminal sequences\n"));
        return 1;
    }
    // Raw keys, resize events, no quick-edit selection, Ctrl+C as a key
    SetConsoleMode(hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    print(out, S("\x1b[?1049h\x1b[?25l"));  // alternate screen, hide cursor

    poison(scratch);
    resample(smp, scratch);
    u64 next    = GetTickCount64() + 250;  // second sample soon, for CPU%
    b32 running = 1;
    while (running) {
        ConsoleScreenBufferInfo csbi = {0};
        GetConsoleScreenBufferInfo(out->handle, &csbi);
        i32 width  = csbi.window.right  - csbi.window.left + 1;
        i32 height = csbi.window.bottom - csbi.window.top  + 1;
        poison(scratch);
        ui_render(ui, smp->last, out, width, height, scratch);
        flush(out);

        i32 flags = 0;
        while (!flags) {
            u64 now = GetTickCount64();
            if (now >= next) {
                flags = UI_RESAMPLE;
                break;
            }
            u32 r = WaitForSingleObject(hin, (u32)(next - now));
            if (r == WAIT_TIMEOUT) {
                continue;
            } else if (r != WAIT_OBJECT_0) {
                flags = UI_QUIT;
                break;
            }

            InputRecord recs[64];
            u32 n = 0;
            if (!ReadConsoleInputW(hin, recs, countof(recs), &n)) {
                flags = UI_QUIT;
                break;
            }
            for (u32 i = 0; i < n; i++) {
                InputRecord *rec = recs + i;
                if (rec->type == WINDOW_BUFFER_SIZE_EVENT) {
                    flags |= UI_REDRAW;
                } else if (rec->type==KEY_EVENT && rec->key.keydown) {
                    i32 key = translatekey(&rec->key);
                    for (i32 j = 0; key && j<max(rec->key.repeat, (u16)1); j++) {
                        flags |= ui_key(ui, key);
                    }
                }
            }
        }

        if (flags & UI_QUIT) {
            running = 0;
        } else if (flags & UI_RESAMPLE) {
            poison(scratch);
            resample(smp, scratch);
            next = GetTickCount64() + (u64)ui_delay(ui);
        }
    }

    print(out, S("\x1b[?25h\x1b[?1049l"));  // show cursor, main screen
    flush(out);
    SetConsoleMode(hin, inmode);
    SetConsoleMode(out->handle, outmode);
    return 0;
}

// Carve a fixed-size arena off another without zeroing it.
static Arena carve(Arena *a, iz size) {
#ifdef DEBUG
    // Keep arenas page-aligned so a guard page never overlaps a neighbor.
    a->beg = (u8 *)(((uptr)a->beg + PAGE - 1) & ~(uptr)(PAGE - 1));
    assert(size%PAGE == 0);
#endif
    assert(size <= a->end - a->beg);
    Arena r = {a->beg, a->beg+size};
    a->beg += size;
    return r;
}

// Debug allocations cost at least 8 KiB each (data page plus guard), so
// snapshots (about 300 names each) get more room. poison() fills whole arenas
// every frame, which caps how large these can reasonably be.
#ifdef DEBUG
enum { SNAP_SIZE = (iz)24<<20, SCRATCH_SIZE = (iz)8<<20, TOTAL_SIZE = (iz)64<<20 };
#else
enum { SNAP_SIZE = (iz)16<<20, SCRATCH_SIZE = (iz)24<<20, TOTAL_SIZE = (iz)64<<20 };
#endif

static i32 run(Arena perm) {
    Output *out = newstdout(&perm, STD_OUTPUT_HANDLE, (iz)1<<20);
    Output *err = newstdout(&perm, STD_ERROR_HANDLE,  (iz)1<<12);

    Config conf = {0};
    conf.delay_ms = 2000;
    i32 r = parseargs(&conf, getargs(&perm), err);
    if (r) {
        flush(err);
        return r == 1 ? 0 : r;  // -h exits successfully
    }

    Sampler smp = {0};
    smp.arenas[0] = carve(&perm, SNAP_SIZE);
    smp.arenas[1] = carve(&perm, SNAP_SIZE);
    Arena scratch = carve(&perm, SCRATCH_SIZE);
    Ui   *ui      = ui_new(&perm, conf.delay_ms);

    if (conf.batch) {
        r = runbatch(conf, ui, &smp, out, scratch);
    } else {
        r = runtui(ui, &smp, out, err, scratch);
    }
    flush(err);
    return r;
}

void mainCRTStartup(void) {
    iz    cap  = TOTAL_SIZE;
    u8   *mem  = (u8 *)VirtualAlloc(0, (uz)cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    Arena perm = {mem, mem+cap};
    if (!mem) {
        ExitProcess(3);
    }
    ExitProcess((u32)run(perm));
}
