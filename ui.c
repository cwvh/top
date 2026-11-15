// Interactive display and batch output.
//
// All state lives in Ui (perm). Snapshots are freed between frames, so the
// UI remembers the selected process by pid and copied name, never by pointer.

// Platform hook, defined by the entry file (main.c, or a stub in
// test.c).
static b32 os_kill(u32 pid);

// Keys arrive as code points (Esc=27, Enter=13, Backspace=8, Ctrl+C=3,
// Tab=9) or as one of these for non-character keys.
enum {
    KEY_UP = 0x110000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PGUP,
    KEY_PGDN,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
};

// ui_key result flags
enum {
    UI_REDRAW   = 1 << 0,
    UI_RESAMPLE = 1 << 1,  // take a fresh sample now (e.g. after a kill)
    UI_QUIT     = 1 << 2,
};

enum {
    COL_PID, COL_PRI, COL_THR, COL_HND, COL_WS, COL_PRIV, COL_CPU, COL_TIME,
    COL_NAME, NCOL,
};

enum {
    PROMPT_NONE, PROMPT_FILTER, PROMPT_KILL, PROMPT_DELAY, PROMPT_HELP,
};

enum { MAXIN = 64 };  // longest filter / prompt input in bytes

// Small fixed-capacity text buffer. Appends silently truncate.
typedef struct {
    u8 d[128];
    iz len;
} Buf;

typedef struct {
    i32 delay_ms;
    i32 sort;
    b32 reverse;
    b32 irix;       // 100% = one core instead of the whole machine
    b32 cores;      // one CPU line per core
    i32 units;      // memory units: 0 KiB, 1 MiB, 2 GiB, 3 TiB
    i32 mode;
    u32 sel_pid;
    i32 sel_idx;    // row of the selection at the last frame
    Buf sel_name;
    i32 scroll;
    i32 move;       // pending relative selection move, applied at render
    i32 page;       // process rows visible at the last frame
    Buf filter;
    Buf edit;
    Buf status;     // shown until the next keypress
} Ui;

static Ui *ui_new(Arena *perm, i32 delay_ms) {
    Ui *ui       = new(perm, 1, Ui);
    ui->delay_ms = delay_ms;
    ui->sort     = COL_CPU;
    ui->units    = 1;
    ui->page     = 10;
    return ui;
}

static i32 ui_delay(Ui *ui) {
    return ui->delay_ms;
}


// Text buffers

static Str str(Buf *b) {
    return (Str){b->d, b->len};
}

static void bch(Buf *b, u8 c) {
    if (b->len < countof(b->d)) {
        b->d[b->len++] = c;
    }
}

static void bcat(Buf *b, Str s) {
    for (iz i = 0; i < s.len; i++) {
        bch(b, s.data[i]);
    }
}

static void bint(Buf *b, i64 v) {
    u8  t[24];
    i32 n = 0;
    u64 u = v<0 ? 0 - (u64)v : (u64)v;
    do {
        t[n++] = (u8)('0' + u%10);
        u /= 10;
    } while (u);
    if (v < 0) {
        bch(b, '-');
    }
    while (n) {
        bch(b, t[--n]);
    }
}

// Two digits, zero padded.
static void b2(Buf *b, i64 v) {
    bch(b, (u8)('0' + v/10%10));
    bch(b, (u8)('0' + v%10));
}

static void bfix(Buf *b, f64 v, i32 prec) {
    if (!(v >= 0)) {
        v = 0;  // also NaN
    }
    if (v > 1e15) {
        v = 1e15;
    }
    i64 scale = 1;
    for (i32 i = 0; i < prec; i++) {
        scale *= 10;
    }
    i64 x = (i64)(v*(f64)scale + 0.5);
    bint(b, x/scale);
    if (prec > 0) {
        bch(b, '.');
        for (i64 d = scale/10; d; d /= 10) {
            bch(b, (u8)('0' + x/d%10));
        }
    }
}

// K/M/G/T in at most 6 characters: 512K, 12.3M, 1.50G
static void bsize(Buf *b, i64 bytes) {
    f64 v = (f64)max(bytes, (i64)0) / 1024;
    i32 u = 0;
    while (v>=1000 && u<3) {
        v /= 1024;
        u++;
    }
    bfix(b, v, u==0 ? 0 : v<10 ? 2 : v<100 ? 1 : 0);
    bch(b, "KMGT"[u]);
}

// top-style CPU time from 100ns units: m:ss.cc, h:mm:ss, then NNNNd HHh
static void btime(Buf *b, i64 t) {
    i64 cs   = max(t, (i64)0) / 100000;
    i64 secs = cs / 100;
    if (secs < 3600) {
        bint(b, secs/60);
        bch(b, ':');
        b2(b, secs%60);
        bch(b, '.');
        b2(b, cs%100);
        return;
    }
    i64 hours = secs / 3600;
    if (hours < 100) {
        bint(b, hours);
        bch(b, ':');
        b2(b, secs/60%60);
        bch(b, ':');
        b2(b, secs%60);
    } else {
        bint(b, min(hours/24, (i64)9999));
        bcat(b, S("d "));
        b2(b, hours%24);
        bch(b, 'h');
    }
}

// Seconds with at least one decimal: 2.0, 0.5, 0.25
static void bdelay(Buf *b, i32 ms) {
    bint(b, ms/1000);
    bch(b, '.');
    i32 frac = ms % 1000;
    u8  d[3] = {(u8)('0'+frac/100), (u8)('0'+frac/10%10), (u8)('0'+frac%10)};
    i32 n = 3;
    for (; n>1 && d[n-1]=='0'; n--) {}
    for (i32 i = 0; i < n; i++) {
        bch(b, d[i]);
    }
}



// Frame: one row at a time, with column accounting so no row overflows.

typedef struct {
    Output *o;
    i32     width, height;
    i32     row;
    i32     col, max;  // current column, and where this row must stop
    b32     plain;     // batch mode: no escapes, no truncation
} Frame;

// Start the next row, or return 0 when the screen is full. The last row
// stops one column short so we never write the bottom-right cell.
static b32 row_begin(Frame *f) {
    if (f->row >= f->height) {
        return 0;
    }
    if (f->row) {
        print(f->o, f->plain ? S("\n") : S("\r\n"));
    }
    f->max = f->row==f->height-1 && !f->plain ? f->width-1 : f->width;
    f->max = max(f->max, 0);
    f->col = 0;
    f->row++;
    return 1;
}

static void row_end(Frame *f) {
    if (!f->plain) {
        print(f->o, S("\x1b[m"));
        if (f->col < f->width) {
            print(f->o, S("\x1b[K"));  // when full, K would erase the last cell
        }
    }
}

static void sgr(Frame *f, Str s) {
    if (!f->plain) {
        print(f->o, s);
    }
}

static void emit(Frame *f, i32 r) {
    if (f->col >= f->max) {
        return;
    }
    if (r<32 || (r>=0x7f && r<0xa0)) {
        r = '?';
    }
    u8 b[4];
    print(f->o, (Str){b, utf8encode(b, r)});
    f->col++;
}

static void text(Frame *f, Str s) {
    while (s.len && f->col<f->max) {
        Utf8 d = utf8decode(s);
        s = d.tail;
        emit(f, d.rune);
    }
}

static void spaces(Frame *f, i32 n) {
    for (; n>0 && f->col<f->max; n--) {
        emit(f, ' ');
    }
}

static void pad_to(Frame *f, i32 col) {
    spaces(f, min(col, f->max) - f->col);
}

// Right-align (ASCII) s in w columns.
static void putr(Frame *f, Str s, i32 w) {
    spaces(f, w - (i32)s.len);
    text(f, s);
}

// A bold number right-aligned in w columns.
static void val(Frame *f, f64 v, i32 w, i32 prec) {
    Buf b = {0};
    bfix(&b, v, prec);
    sgr(f, S("\x1b[1m"));
    putr(f, str(&b), w);
    sgr(f, S("\x1b[m"));
}


// Columns

static i32 colw(i32 c) {
    switch (c) {
    case COL_PID:  return 7;
    case COL_PRI:  return 3;
    case COL_THR:  return 5;
    case COL_HND:  return 7;  // leaky processes reach millions
    case COL_WS:   return 7;
    case COL_PRIV: return 7;
    case COL_CPU:  return 6;
    case COL_TIME: return 9;
    }
    return 0;
}

static Str colname(i32 c) {
    switch (c) {
    case COL_PID:  return S("PID");
    case COL_PRI:  return S("PRI");
    case COL_THR:  return S("THR");
    case COL_HND:  return S("HND");
    case COL_WS:   return S("WS");
    case COL_PRIV: return S("PRIV");
    case COL_CPU:  return S("CPU%");
    case COL_TIME: return S("TIME+");
    }
    return S("NAME");
}

static f64 cpupct(Ui *ui, Snapshot *s, ProcInfo *p) {
    return ui->irix ? p->cpu*100 : p->cpu*100/(f64)max(s->ncpu, 1);
}

static void cell(Buf *b, Ui *ui, Snapshot *s, ProcInfo *p, i32 c) {
    switch (c) {
    case COL_PID:  bint(b, p->pid);         break;
    case COL_PRI:  bint(b, p->priority);    break;
    case COL_THR:  bint(b, p->threads);     break;
    case COL_HND:  bint(b, p->handles);     break;
    case COL_WS:   bsize(b, p->workingset); break;
    case COL_PRIV: bsize(b, p->privbytes);  break;
    case COL_CPU:  bfix(b, cpupct(ui, s, p), 1); break;
    case COL_TIME: btime(b, p->cputime);    break;
    }
}

static void draw_colheader(Ui *ui, Frame *f) {
    sgr(f, S("\x1b[7m"));
    for (i32 c = 0; c < NCOL; c++) {
        b32 hot = c == ui->sort;
        if (hot) {
            sgr(f, S("\x1b[1;36m"));  // still reversed: cyan background
        }
        if (c == COL_NAME) {
            text(f, colname(c));
        } else {
            putr(f, colname(c), colw(c));
            spaces(f, 1);
        }
        if (hot) {
            sgr(f, S("\x1b[m\x1b[7m"));
        }
    }
    if (!f->plain) {
        pad_to(f, f->max);
    }
}

static void draw_proc(Ui *ui, Snapshot *s, Frame *f, ProcInfo *p) {
    for (i32 c = 0; c < COL_NAME; c++) {
        Buf b = {0};
        cell(&b, ui, s, p, c);
        putr(f, str(&b), colw(c));
        spaces(f, 1);
    }
    text(f, p->name);
}


// Sorting

#define cmp3(a, b) (((a) > (b)) - ((a) < (b)))

static i32 cmpname(Str a, Str b) {
    iz n = min(a.len, b.len);
    for (iz i = 0; i < n; i++) {
        if (lower(a.data[i]) != lower(b.data[i])) {
            return cmp3(lower(a.data[i]), lower(b.data[i]));
        }
    }
    return cmp3(a.len, b.len);
}

// Negative if a belongs before b. Numbers sort descending and PID/NAME
// ascending unless reversed; ties always fall back to ascending pid.
static i32 cmprow(Ui *ui, ProcInfo *a, ProcInfo *b) {
    i32 c    = 0;
    b32 desc = 1;
    switch (ui->sort) {
    case COL_PID:  c = cmp3(a->pid, b->pid);               desc = 0; break;
    case COL_PRI:  c = cmp3(a->priority, b->priority);     break;
    case COL_THR:  c = cmp3(a->threads, b->threads);       break;
    case COL_HND:  c = cmp3(a->handles, b->handles);       break;
    case COL_WS:   c = cmp3(a->workingset, b->workingset); break;
    case COL_PRIV: c = cmp3(a->privbytes, b->privbytes);   break;
    case COL_CPU:  c = cmp3(a->cpu, b->cpu);               break;
    case COL_TIME: c = cmp3(a->cputime, b->cputime);       break;
    case COL_NAME: c = cmpname(a->name, b->name);          desc = 0; break;
    }
    if (desc) {
        c = -c;
    }
    if (ui->reverse) {
        c = -c;
    }
    return c ? c : cmp3(a->pid, b->pid);
}

// Bottom-up merge sort. Returns whichever buffer holds the result.
static ProcInfo **sortrows(Ui *ui, ProcInfo **a, ProcInfo **t, iz n) {
    for (iz w = 1; w < n; w *= 2) {
        for (iz i = 0; i < n; i += 2*w) {
            iz mid = min(i+w, n);
            iz hi  = min(i+2*w, n);
            iz l = i, r = mid, k = i;
            while (l<mid && r<hi) {
                t[k++] = cmprow(ui, a[r], a[l])<0 ? a[r++] : a[l++];
            }
            while (l < mid) {
                t[k++] = a[l++];
            }
            while (r < hi) {
                t[k++] = a[r++];
            }
        }
        ProcInfo **swap = a;
        a = t;
        t = swap;
    }
    return a;
}

// Filtered and sorted process pointers, valid for this frame only.
static ProcInfo **buildrows(Ui *ui, Snapshot *s, Arena *scratch, iz *count) {
    ProcInfo **a = new(scratch, s->nprocs, ProcInfo *);
    ProcInfo **t = new(scratch, s->nprocs, ProcInfo *);
    Str flt = str(&ui->filter);
    iz  n   = 0;
    for (iz i = 0; i < s->nprocs; i++) {
        if (!flt.len || icontains(s->procs[i].name, flt)) {
            a[n++] = s->procs + i;
        }
    }
    *count = n;
    return sortrows(ui, a, t, n);
}


// Rendering

// Number of per-core lines to show, or 0 for the summary line. Leave about
// half the screen for processes.
static i32 percore(Ui *ui, Snapshot *s, i32 height) {
    if (!ui->cores) {
        return 0;
    }
    return min(max(s->ncpu, 0), max((height - 10)/2, 0));
}

// %Cpu(s):  5.9 us,  2.0 sy, 91.8 id,  0.0 hi,  0.3 si
// %Cpu0  :  ...      (core < 0 is the summary)
static void draw_cpu(Frame *f, i32 core, CpuTimes t) {
    Buf l = {0};
    bcat(&l, S("%Cpu"));
    if (core < 0) {
        bcat(&l, S("(s)"));
    } else {
        bint(&l, core);
    }
    while (l.len < 7) {
        bch(&l, ' ');
    }
    bch(&l, ':');
    text(f, str(&l));
    val(f, t.us*100, 5, 1);
    text(f, S(" us,"));
    val(f, t.sy*100, 5, 1);
    text(f, S(" sy,"));
    val(f, t.id*100, 5, 1);
    text(f, S(" id,"));
    val(f, t.hi*100, 5, 1);
    text(f, S(" hi,"));
    val(f, t.si*100, 5, 1);
    text(f, S(" si"));
}

static Str unitname(i32 u) {
    switch (u) {
    case 0:  return S("KiB");
    case 1:  return S("MiB");
    case 2:  return S("GiB");
    }
    return S("TiB");
}

// a - b, saturating so hostile byte counts cannot overflow.
static i64 subsat(i64 a, i64 b) {
    if ((b>0 && a<-0x7fffffffffffffffLL-1 + b) || (b<0 && a>0x7fffffffffffffffLL + b)) {
        return a < 0 ? -0x7fffffffffffffffLL - 1 : 0x7fffffffffffffffLL;
    }
    return a - b;
}

// MiB Mem :  32659.2 total,  20972.1 avail,  11686.4 used
static void draw_mem(Ui *ui, Frame *f, Str label, i64 total, i64 avail, i64 used) {
    f64 div = 1;
    for (i32 i = 0; i <= ui->units; i++) {
        div *= 1024;
    }
    i32 prec = ui->units ? 1 : 0;
    text(f, unitname(ui->units));
    text(f, label);
    val(f, (f64)max(total, (i64)0)/div, 9, prec);
    text(f, S(" total,"));
    val(f, (f64)max(avail, (i64)0)/div, 9, prec);
    text(f, S(" avail,"));
    val(f, (f64)max(used, (i64)0)/div, 9, prec);
    text(f, S(" used"));
}

// The summary block above the status line: time, tasks, CPU, memory.
// Returns 0 if the screen filled up.
static b32 draw_head(Ui *ui, Snapshot *s, Frame *f, i32 ncores) {
    SystemTime now = {0};
    GetLocalTime(&now);
    i64 secs = s->uptime_ms / 1000;
    i64 days = secs / 86400;
    i64 hrs  = secs/3600 % 24;
    if (!row_begin(f)) {
        return 0;
    }
    Buf b = {0};
    bcat(&b, S("top - "));
    b2(&b, now.hour);
    bch(&b, ':');
    b2(&b, now.minute);
    bch(&b, ':');
    b2(&b, now.second);
    bcat(&b, S(" up "));
    if (days) {
        bint(&b, days);
        bcat(&b, days==1 ? S(" day, ") : S(" days, "));
    }
    if (hrs) {
        bint(&b, hrs);
        bch(&b, ':');
        b2(&b, secs/60 % 60);
    } else {
        bint(&b, secs/60 % 60);
        bcat(&b, S(" min"));
    }
    text(f, str(&b));
    row_end(f);

    if (!row_begin(f)) {
        return 0;
    }
    text(f, S("Tasks: "));
    val(f, (f64)s->nprocs, 0, 0);
    text(f, S(" total,  "));
    val(f, (f64)s->nthreads, 0, 0);
    text(f, S(" threads,  "));
    val(f, (f64)s->nhandles, 0, 0);
    text(f, S(" handles"));
    row_end(f);

    for (i32 i = 0; i < max(ncores, 1); i++) {
        if (!row_begin(f)) {
            return 0;
        }
        draw_cpu(f, ncores ? i : -1, ncores ? s->cores[i] : s->cpu);
        row_end(f);
    }

    if (!row_begin(f)) {
        return 0;
    }
    draw_mem(ui, f, S(" Mem :"), s->memtotal, s->memavail, subsat(s->memtotal, s->memavail));
    row_end(f);

    if (!row_begin(f)) {
        return 0;
    }
    draw_mem(ui, f, S(" Cmt :"), s->commitlimit, subsat(s->commitlimit, s->commit), s->commit);
    row_end(f);
    return 1;
}

static void draw_status(Ui *ui, Frame *f) {
    Buf b = {0};
    switch (ui->mode) {
    case PROMPT_FILTER:
        bcat(&b, S("Filter: "));
        bcat(&b, str(&ui->filter));
        break;
    case PROMPT_KILL:
        bcat(&b, S("Kill PID ["));
        bint(&b, ui->sel_pid);
        bch(&b, ' ');
        bcat(&b, str(&ui->sel_name));
        bcat(&b, S("]: "));
        bcat(&b, str(&ui->edit));
        break;
    case PROMPT_DELAY:
        bcat(&b, S("Delay seconds ["));
        bdelay(&b, ui->delay_ms);
        bcat(&b, S("]: "));
        bcat(&b, str(&ui->edit));
        break;
    default:
        if (ui->status.len) {
            sgr(f, S("\x1b[33m"));
            text(f, str(&ui->status));
        } else if (ui->filter.len) {
            bcat(&b, S("filter: \""));
            bcat(&b, str(&ui->filter));
            bcat(&b, S("\" (Esc clears)"));
        }
        break;
    }
    if (ui->mode != PROMPT_NONE) {
        sgr(f, S("\x1b[1m"));
    }
    text(f, str(&b));
    if (ui->mode != PROMPT_NONE) {
        sgr(f, S("\x1b[7m"));
        text(f, S(" "));  // cursor
    }
}

// Move the selection (by pid, else by row index) and copy its name.
static i32 select_row(Ui *ui, ProcInfo **rows, i32 n) {
    i32 idx = -1;
    for (i32 i = 0; i<n && idx<0; i++) {
        if (rows[i]->pid == ui->sel_pid) {
            idx = i;
        }
    }
    if (idx < 0) {
        idx = ui->sel_idx;
    }
    idx      = (i32)max((i64)0, min((i64)idx + ui->move, (i64)n - 1));
    ui->move = 0;

    ui->sel_name.len = 0;
    ui->sel_pid      = 0;
    if (n) {
        ui->sel_pid = rows[idx]->pid;
        Str name = rows[idx]->name;
        while (name.len && ui->sel_name.len < 48) {  // whole code points
            Utf8 d = utf8decode(name);
            bcat(&ui->sel_name, (Str){name.data, name.len - d.tail.len});
            name = d.tail;
        }
    }
    ui->sel_idx = idx;
    return idx;
}

static void draw_main(Ui *ui, Snapshot *s, Frame *f, Arena scratch) {
    iz         count = 0;
    ProcInfo **rows  = buildrows(ui, s, &scratch, &count);
    i32        n     = (i32)count;
    i32        idx   = select_row(ui, rows, n);

    i32 ncores = percore(ui, s, f->height);
    i32 before = 2 + max(ncores, 1) + 2 + 1;  // header, CPU, memory, status
    i32 vis    = max(f->height - before - 1, 0);

    ui->page = max(vis, 1);
    if (idx < ui->scroll) {
        ui->scroll = idx;
    }
    if (vis>0 && idx>=ui->scroll+vis) {
        ui->scroll = idx - vis + 1;
    }
    ui->scroll = max(min(ui->scroll, n - vis), 0);

    if (!draw_head(ui, s, f, ncores)) {
        return;
    }

    if (!row_begin(f)) {
        return;
    }
    draw_status(ui, f);
    row_end(f);

    if (!row_begin(f)) {
        return;
    }
    draw_colheader(ui, f);
    row_end(f);

    for (i32 i = ui->scroll; i<n && i<ui->scroll+vis; i++) {
        if (!row_begin(f)) {
            return;
        }
        if (i == idx) {
            sgr(f, S("\x1b[30;46m"));
        }
        draw_proc(ui, s, f, rows[i]);
        if (i == idx) {
            pad_to(f, f->max);
        }
        row_end(f);
    }
}

static Str helpline(i32 i) {
    switch (i) {
    case  0: return S("top - help");
    case  1: return S("");
    case  2: return S("  Up Down PgUp PgDn Home End   move the selection");
    case  3: return S("  P M T N        sort by CPU%, WS (memory), TIME+, PID");
    case  4: return S("  < >            move the sort column left / right");
    case  5: return S("  R              reverse the sort order");
    case  6: return S("  I              toggle Irix mode (100% = one core)");
    case  7: return S("  1              toggle per-CPU lines");
    case  8: return S("  E              cycle memory units (KiB MiB GiB TiB)");
    case  9: return S("  /              filter by name (Enter keeps, Esc clears)");
    case 10: return S("  k              kill a process (Enter alone: the selected one)");
    case 11: return S("  d              set the refresh delay in seconds");
    case 12: return S("  Esc            cancel a prompt, or clear the filter");
    case 13: return S("  h ?            this help");
    case 14: return S("  q Ctrl+C       quit");
    case 15: return S("");
    case 16: return S("Press any key to return.");
    }
    return (Str){0};
}

static void draw_help(Frame *f) {
    for (i32 i = 0; helpline(i).data; i++) {
        if (!row_begin(f)) {
            return;
        }
        sgr(f, i==0 ? S("\x1b[1m") : S("\x1b[m"));
        text(f, helpline(i));
        row_end(f);
    }
}

static void ui_render(Ui *ui, Snapshot *s, Output *o, i32 width, i32 height, Arena scratch) {
    Frame f = {0};
    f.o      = o;
    f.width  = max(width, 0);
    f.height = max(height, 0);
    print(o, S("\x1b[H"));
    if (ui->mode == PROMPT_HELP) {
        draw_help(&f);
    } else {
        draw_main(ui, s, &f, scratch);
    }
    print(o, S("\x1b[J"));
}

// Plain text, like top -b: nothing truncated, no escapes.
static void ui_batch(Ui *ui, Snapshot *s, Output *o, Arena scratch) {
    Frame f = {0};
    f.o      = o;
    f.plain  = 1;
    f.width  = 0x7fffffff;
    f.height = 0x7fffffff;

    draw_head(ui, s, &f, percore(ui, s, f.height));

    row_begin(&f);  // blank
    row_begin(&f);
    draw_colheader(ui, &f);

    iz         n    = 0;
    ProcInfo **rows = buildrows(ui, s, &scratch, &n);
    for (iz i = 0; i < n; i++) {
        row_begin(&f);
        draw_proc(ui, s, &f, rows[i]);
    }
    print(o, S("\n\n"));
}


// Keys

// Selection moves are applied at the next render, when we have the rows.
static b32 nav(Ui *ui, i32 key) {
    i32 d = 0;
    switch (key) {
    case KEY_UP:   d = -1;         break;
    case KEY_DOWN: d = 1;          break;
    case KEY_PGUP: d = -ui->page;  break;
    case KEY_PGDN: d = ui->page;   break;
    case KEY_HOME:
        ui->sel_idx = 0;
        ui->sel_pid = 0;
        ui->move    = 0;
        return 1;
    case KEY_END:
        ui->sel_idx = 0x7fffffff;
        ui->sel_pid = 0;
        ui->move    = 0;
        return 1;
    default:
        return 0;
    }
    ui->move = max(min(ui->move + d, 1<<20), -(1<<20));
    return 1;
}

static void setsort(Ui *ui, i32 col) {
    ui->sort    = col;
    ui->reverse = 0;
}

static i32 commit(Ui *ui) {
    i32 mode = ui->mode;
    ui->mode = PROMPT_NONE;
    Str in   = str(&ui->edit);
    Buf *st  = &ui->status;
    if (mode == PROMPT_DELAY) {
        i32 ms = 0;
        if (parsedelay(in, &ms)) {
            ui->delay_ms = ms;
            bcat(st, S("Delay set to "));
            bdelay(st, ms);
            bch(st, 's');
        } else {
            bcat(st, S("Invalid delay: "));
            bcat(st, in);
        }
        ui->edit.len = 0;
        return UI_REDRAW | UI_RESAMPLE;
    }
    if (mode == PROMPT_KILL) {
        i32 pid = (i32)ui->sel_pid;
        if (in.len && !parseint(in, &pid)) {
            bcat(st, S("Invalid PID: "));
            bcat(st, in);
        } else if (!pid) {
            bcat(st, S("No process selected"));
        } else if (os_kill((u32)pid)) {
            bcat(st, S("Killed "));
            bint(st, (u32)pid);
            if ((u32)pid == ui->sel_pid) {
                bcat(st, S(" ("));
                bcat(st, str(&ui->sel_name));
                bch(st, ')');
            }
        } else {
            u32 err = GetLastError();
            bcat(st, S("Failed to kill "));
            bint(st, (u32)pid);
            bcat(st, S(" (error "));
            bint(st, err);
            bch(st, ')');
        }
        ui->edit.len = 0;
        return UI_REDRAW | UI_RESAMPLE;
    }
    return UI_REDRAW;  // filter: keep it
}

static i32 prompt_key(Ui *ui, i32 key) {
    Buf *b = ui->mode==PROMPT_FILTER ? &ui->filter : &ui->edit;
    if (key == 27) {
        b->len   = 0;
        ui->mode = PROMPT_NONE;
        return UI_REDRAW;
    } else if (key == 13) {
        return commit(ui);
    } else if (key==8 || key==127) {
        if (b->len) {
            do {
                b->len--;  // a whole UTF-8 sequence
            } while (b->len && (b->d[b->len]&0xc0)==0x80);
        }
        return UI_REDRAW;
    } else if (ui->mode==PROMPT_FILTER && nav(ui, key)) {
        return UI_REDRAW;
    } else if ((key>=32 && key<127) || (key>=0xa0 && key<0x110000)) {
        if (b->len + 4 <= MAXIN) {
            b->len += utf8encode(b->d + b->len, key);
        }
        return UI_REDRAW;
    }
    return 0;
}

static i32 handle_key(Ui *ui, i32 key) {
    if (ui->mode == PROMPT_HELP) {
        ui->mode = PROMPT_NONE;
        return UI_REDRAW;
    } else if (ui->mode != PROMPT_NONE) {
        return prompt_key(ui, key);
    }

    switch (key) {
    case 'q':
        return UI_QUIT;
    case 'h': case '?':
        ui->mode = PROMPT_HELP;
        break;
    case 27:
        if (!ui->filter.len) {
            return 0;
        }
        ui->filter.len = 0;
        break;
    case '/':
        ui->mode = PROMPT_FILTER;
        break;
    case 'k':
        ui->mode     = PROMPT_KILL;
        ui->edit.len = 0;
        break;
    case 'd':
        ui->mode     = PROMPT_DELAY;
        ui->edit.len = 0;
        break;
    case 'I':
        ui->irix = !ui->irix;
        bcat(&ui->status, ui->irix ? S("Irix mode on: 100% is one core")
                                   : S("Irix mode off: 100% is the whole machine"));
        break;
    case '1':
        ui->cores = !ui->cores;
        break;
    case 'E':
        ui->units = (ui->units + 1) % 4;
        bcat(&ui->status, S("Memory units: "));
        bcat(&ui->status, unitname(ui->units));
        break;
    case 'P': setsort(ui, COL_CPU);  break;
    case 'M': setsort(ui, COL_WS);   break;
    case 'T': setsort(ui, COL_TIME); break;
    case 'N': setsort(ui, COL_PID);  break;
    case '<': setsort(ui, max(ui->sort - 1, 0));        break;
    case '>': setsort(ui, min(ui->sort + 1, NCOL - 1)); break;
    case 'R':
        ui->reverse = !ui->reverse;
        break;
    default:
        return nav(ui, key) ? UI_REDRAW : 0;
    }
    return UI_REDRAW;
}

static i32 ui_key(Ui *ui, i32 key) {
    if (key == 3) {
        return UI_QUIT;
    }
    b32 had = ui->status.len > 0;
    ui->status.len = 0;
    i32 r = handle_key(ui, key);
    return had ? r | UI_REDRAW : r;
}
