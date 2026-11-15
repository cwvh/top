// System sampling: per-core times, process list, memory.

typedef struct {
    Str  name;        // image name, UTF-8 ("svchost.exe", "System", ...)
    u32  pid;
    u32  ppid;
    i64  created;     // 100ns since 1601; (pid, created) identifies a process
    i64  cputime;     // user + kernel, 100ns units
    f64  cpu;         // cores busy over the interval (1.0 = one full core)
    i64  workingset;  // bytes
    i64  privbytes;   // private committed bytes
    i32  threads;
    i32  handles;
    i32  priority;    // base priority
    u32  session;
} ProcInfo;

// Fractions 0..1 of an interval. Kernel time is split into sy (the rest),
// id (idle), hi (interrupts) and si (DPCs).
typedef struct {
    f64 us, sy, id, hi, si;
} CpuTimes;

typedef struct {
    ProcInfo *procs;        // unordered; excludes System Idle Process (pid 0)
    iz        nprocs;
    i64       nthreads;     // totals over procs
    i64       nhandles;
    i32       ncpu;         // logical processors (current processor group)
    CpuTimes *cores;        // [ncpu] breakdown over the interval; 0 on the first
    CpuTimes  cpu;          // the same, over all cores
    i64       interval;     // 100ns units; 0 on the first snapshot
    i64       uptime_ms;
    i64       memtotal;     // physical, bytes
    i64       memavail;     // physical, bytes
    i64       commit;       // committed, bytes
    i64       commitlimit;  // bytes

    // Raw per-core counters for the next delta
    NtProcessorPerf *raw;   // [ncpu]
} Snapshot;

enum {
    MAX_CPUS = 256,
    PROC_BUF = 1 << 20,   // initial process buffer; grown on demand
};

static NtProcessorPerf perfdelta(NtProcessorPerf a, NtProcessorPerf b) {
    NtProcessorPerf r = {0};
    r.idletime      = max(a.idletime - b.idletime, (i64)0);
    r.kerneltime    = max(a.kerneltime - b.kerneltime, (i64)0);
    r.usertime      = max(a.usertime - b.usertime, (i64)0);
    r.dpctime       = max(a.dpctime - b.dpctime, (i64)0);
    r.interrupttime = max(a.interrupttime - b.interrupttime, (i64)0);
    return r;
}

static void perfadd(NtProcessorPerf *a, NtProcessorPerf b) {
    a->idletime      += b.idletime;
    a->kerneltime    += b.kerneltime;
    a->usertime      += b.usertime;
    a->dpctime       += b.dpctime;
    a->interrupttime += b.interrupttime;
}

// Kernel time includes idle, DPC, and interrupt time, so what is left
// after subtracting them is time in the kernel proper.
static CpuTimes cputimes(NtProcessorPerf d) {
    CpuTimes r     = {0};
    i64      total = d.kerneltime + d.usertime;
    if (total > 0) {
        f64 t = (f64)total;
        i64 sy = max(d.kerneltime - d.idletime - d.dpctime - d.interrupttime, (i64)0);
        r.us = (f64)d.usertime / t;
        r.sy = (f64)sy / t;
        r.id = (f64)d.idletime / t;
        r.hi = (f64)d.interrupttime / t;
        r.si = (f64)d.dpctime / t;
    }
    return r;
}

static u32 hashpid(u32 pid) {
    return (pid * 2654435761u) >> 8;  // Fibonacci hashing; PIDs are multiples of 4
}

// Open-addressing table over prev->procs: slot holds index+1, 0 is empty.
typedef struct {
    u32 *slots;
    u32  mask;
} PidTable;

static PidTable pidtable(Arena *a, Snapshot *prev) {
    PidTable t = {0};
    iz cap = 16;
    for (; cap < 2*prev->nprocs; cap *= 2) {}
    t.slots = new(a, cap, u32);
    t.mask  = (u32)cap - 1;
    for (iz i = 0; i < prev->nprocs; i++) {
        u32 h = hashpid(prev->procs[i].pid) & t.mask;
        for (; t.slots[h]; h = (h+1) & t.mask) {}
        t.slots[h] = (u32)i + 1;
    }
    return t;
}

// Find the previous incarnation of a process. Both pid and creation time
// must match since PIDs are reused.
static ProcInfo *findprev(PidTable *t, Snapshot *prev, u32 pid, i64 created) {
    u32 h = hashpid(pid) & t->mask;
    for (; t->slots[h]; h = (h+1) & t->mask) {
        ProcInfo *p = prev->procs + (t->slots[h] - 1);
        if (p->pid==pid && p->created==created) {
            return p;
        }
    }
    return 0;
}

// The bytes NtQuerySystemInformation actually returned.
typedef struct {
    u8 *beg;
    u8 *end;
} ProcList;

// Fetch the process list into a buffer from scratch. Empty on failure.
static ProcList queryprocs(Arena *scratch) {
    ProcList l = {0};
    iz size = PROC_BUF;
    for (i32 tries = 0; tries < 4; tries++) {
        // Leave half of scratch for the pid table
        if (size > (scratch->end - scratch->beg)/2) {
            return l;
        }
        Arena save = *scratch;
        u64  *buf  = new(scratch, size/8, u64);
        u32   len  = 0;
        i32   r    = NtQuerySystemInformation(
            SystemProcessInformation, buf, (u32)size, &len
        );
        if (r == STATUS_SUCCESS) {
            l.beg = (u8 *)buf;
            l.end = l.beg + min((iz)len, size);
            return l;
        }
        *scratch = save;
        if (r != STATUS_INFO_LENGTH_MISMATCH) {
            return l;
        }
        // The list may grow between calls, so add slack.
        size = max((iz)len + (1<<16), size*2) & ~(iz)7;
    }
    return l;
}

// The entry at byte offset off, or null if it (or its name) would fall
// outside the returned bytes. Don't trust the kernel's offsets blindly.
static NtProcessInfo *procat(ProcList l, iz off) {
    iz avail = l.end - l.beg;
    if (!l.beg || off<0 || off%8 || avail-off < (iz)sizeof(NtProcessInfo)) {
        return 0;
    }
    NtProcessInfo *p    = (NtProcessInfo *)(l.beg + off);
    uptr           name = (uptr)p->name.buffer;
    if (p->name.length) {
        if (name<(uptr)l.beg || name>(uptr)l.end ||
                (uptr)l.end-name < p->name.length || name%2) {
            return 0;
        }
    }
    return p;
}

// Take a snapshot, allocated entirely in perm. prev is the previous
// snapshot (or null) for computing CPU deltas; it lives in a different
// arena and stays valid for the duration of the call.
static Snapshot *sample(Arena *perm, Arena scratch, Snapshot *prev) {
    Snapshot *s = new(perm, 1, Snapshot);

    // Per-core times
    NtProcessorPerf *perf = new(&scratch, MAX_CPUS, NtProcessorPerf);
    u32 len = 0;
    i32 r   = NtQuerySystemInformation(
        SystemProcessorPerformanceInformation,
        perf, (u32)(MAX_CPUS*(iz)sizeof(*perf)), &len
    );
    s->ncpu      = r==STATUS_SUCCESS ? (i32)(len / sizeof(*perf)) : 0;
    s->cores     = new(perm, s->ncpu, CpuTimes);
    s->raw       = new(perm, s->ncpu, NtProcessorPerf);
    for (i32 i = 0; i < s->ncpu; i++) {
        s->raw[i] = perf[i];
    }
    if (prev && prev->ncpu==s->ncpu && s->ncpu>0) {
        NtProcessorPerf sum = {0};
        for (i32 i = 0; i < s->ncpu; i++) {
            NtProcessorPerf d = perfdelta(s->raw[i], prev->raw[i]);
            perfadd(&sum, d);
            s->cores[i] = cputimes(d);
        }
        s->cpu      = cputimes(sum);
        s->interval = (sum.kerneltime + sum.usertime) / s->ncpu;
    }

    // Processes, queried right after the core times to keep them in step
    ProcList list = queryprocs(&scratch);
    if (list.beg) {
        iz n = 0;
        for (iz off = 0;;) {
            NtProcessInfo *p = procat(list, off);
            if (!p) {
                break;
            }
            n += p->pid != 0;
            if (!p->next) {
                break;
            }
            off += p->next;
        }

        PidTable table = {0};
        b32 delta = prev && prev->nprocs>0 && s->interval>0;
        if (delta) {
            table = pidtable(&scratch, prev);
        }

        s->procs = new(perm, n, ProcInfo);
        for (iz off = 0;;) {
            NtProcessInfo *p = procat(list, off);
            if (!p) {
                break;
            }
            if (p->pid) {
                ProcInfo *o = s->procs + s->nprocs++;
                iz units    = p->name.length / 2;
                o->name       = units ? fromutf16(perm, p->name.buffer, units) : S("?");
                o->pid        = (u32)p->pid;
                o->ppid       = (u32)p->ppid;
                o->created    = p->createtime;
                o->cputime    = p->usertime + p->kerneltime;
                o->workingset = (i64)p->workingset;
                o->privbytes  = (i64)p->privatebytes;
                o->threads    = (i32)p->nthreads;
                o->handles    = (i32)p->nhandles;
                o->priority   = p->basepriority;
                o->session    = p->session;
                s->nthreads  += o->threads;
                s->nhandles  += o->handles;
                if (delta) {
                    ProcInfo *old = findprev(&table, prev, o->pid, o->created);
                    if (old) {
                        o->cpu = max((f64)(o->cputime - old->cputime) /
                                     (f64)s->interval, 0.0);
                    }
                }
            }
            if (!p->next) {
                break;
            }
            off += p->next;
        }
    }

    // Memory
    MemoryStatusEx mem = {0};
    mem.length = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        s->memtotal    = (i64)mem.totalphys;
        s->memavail    = (i64)mem.availphys;
        s->commitlimit = (i64)mem.totalpagefile;
        s->commit      = (i64)(mem.totalpagefile - mem.availpagefile);
    }
    s->uptime_ms = (i64)GetTickCount64();
    return s;
}
