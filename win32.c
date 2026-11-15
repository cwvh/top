// Hand-written Win32 and NT declarations (no windows.h).

#define W32(r) __declspec(dllimport) r

enum {
    STD_INPUT_HANDLE  = -10,
    STD_OUTPUT_HANDLE = -11,
    STD_ERROR_HANDLE  = -12,
};

enum {
    // Console input modes
    ENABLE_PROCESSED_INPUT  = 0x0001,
    ENABLE_LINE_INPUT       = 0x0002,
    ENABLE_ECHO_INPUT       = 0x0004,
    ENABLE_WINDOW_INPUT     = 0x0008,
    ENABLE_MOUSE_INPUT      = 0x0010,
    ENABLE_QUICK_EDIT_MODE  = 0x0040,
    ENABLE_EXTENDED_FLAGS   = 0x0080,

    // Console output modes
    ENABLE_PROCESSED_OUTPUT            = 0x0001,
    ENABLE_WRAP_AT_EOL_OUTPUT          = 0x0002,
    ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004,
    DISABLE_NEWLINE_AUTO_RETURN        = 0x0008,

    // InputRecord.type
    KEY_EVENT                = 0x0001,
    MOUSE_EVENT              = 0x0002,
    WINDOW_BUFFER_SIZE_EVENT = 0x0004,

    // Virtual keys
    VK_PRIOR  = 0x21,
    VK_NEXT   = 0x22,
    VK_END    = 0x23,
    VK_HOME   = 0x24,
    VK_LEFT   = 0x25,
    VK_UP     = 0x26,
    VK_RIGHT  = 0x27,
    VK_DOWN   = 0x28,
    VK_DELETE = 0x2e,

    WAIT_OBJECT_0 = 0x000,
    WAIT_TIMEOUT  = 0x102,

    PROCESS_TERMINATE = 0x0001,

    MEM_COMMIT     = 0x1000,
    MEM_RESERVE    = 0x2000,
    PAGE_NOACCESS  = 0x01,
    PAGE_READWRITE = 0x04,
};

#define INFINITE 0xffffffffu  // does not fit in an enum

typedef struct {
    i16 x;
    i16 y;
} Coord;

typedef struct {
    i16 left;
    i16 top;
    i16 right;
    i16 bottom;
} SmallRect;

typedef struct {
    Coord     size;
    Coord     cursor;
    u16       attributes;
    SmallRect window;     // visible region, inclusive
    Coord     maxsize;
} ConsoleScreenBufferInfo;
_Static_assert(sizeof(ConsoleScreenBufferInfo) == 22, "");

typedef struct {
    b32 keydown;
    u16 repeat;
    u16 vkey;
    u16 vscan;
    c16 ch;
    u32 ctrlstate;
} KeyEventRecord;

typedef struct {
    u16 type;
    union {
        KeyEventRecord key;
        Coord          size;     // WINDOW_BUFFER_SIZE_EVENT
        u8             raw[16];
    };
} InputRecord;
_Static_assert(sizeof(InputRecord) == 20, "");

typedef struct {
    u32 length;  // must be set to sizeof(MemoryStatusEx)
    u32 load;
    u64 totalphys;
    u64 availphys;
    u64 totalpagefile;  // commit limit
    u64 availpagefile;  // commit limit minus committed
    u64 totalvirtual;
    u64 availvirtual;
    u64 availextended;
} MemoryStatusEx;
_Static_assert(sizeof(MemoryStatusEx) == 64, "");

typedef struct {
    u16 year;
    u16 month;
    u16 dayofweek;
    u16 day;
    u16 hour;
    u16 minute;
    u16 second;
    u16 ms;
} SystemTime;
_Static_assert(sizeof(SystemTime) == 16, "");

W32(void *) VirtualAlloc(void *, uz, u32, u32);
W32(b32)    VirtualProtect(void *, uz, u32, u32 *);
W32(uptr)   GetStdHandle(i32);
W32(b32)    GetConsoleMode(uptr, u32 *);
W32(b32)    SetConsoleMode(uptr, u32);
W32(b32)    GetConsoleScreenBufferInfo(uptr, ConsoleScreenBufferInfo *);
W32(b32)    WriteConsoleW(uptr, c16 const *, u32, u32 *, void *);
W32(b32)    WriteFile(uptr, u8 const *, u32, u32 *, void *);
W32(b32)    ReadConsoleInputW(uptr, InputRecord *, u32, u32 *);
W32(u32)    WaitForSingleObject(uptr, u32);
W32(void)   Sleep(u32);
W32(u64)    GetTickCount64(void);
W32(void)   GetLocalTime(SystemTime *);
W32(b32)    GlobalMemoryStatusEx(MemoryStatusEx *);
W32(uptr)   OpenProcess(u32, b32, u32);
W32(b32)    TerminateProcess(uptr, u32);
W32(b32)    CloseHandle(uptr);
W32(u32)    GetLastError(void);
W32(c16 *)  GetCommandLineW(void);
__declspec(dllimport) __attribute((noreturn))
void ExitProcess(u32);


// ntdll

enum {
    SystemProcessInformation              = 5,
    SystemProcessorPerformanceInformation = 8,

    STATUS_SUCCESS                        = 0,
};

#define STATUS_INFO_LENGTH_MISMATCH ((i32)0xc0000004)  // does not fit in an enum

typedef struct {
    u16  length;     // in bytes, not units
    u16  maxlength;
    c16 *buffer;
} UnicodeString;

// SYSTEM_PROCESS_INFORMATION (x64). Each entry is followed by thread
// records. Walk the list with the next offset, which is 0 on the last
// entry.
typedef struct {
    u32           next;
    u32           nthreads;
    i64           wsprivate;
    u32           hardfaults;
    u32           nthreadspeak;
    u64           cycletime;
    i64           createtime;       // 100ns since 1601
    i64           usertime;         // 100ns
    i64           kerneltime;       // 100ns
    UnicodeString name;             // empty for pid 0
    i32           basepriority;
    uptr          pid;
    uptr          ppid;
    u32           nhandles;
    u32           session;
    uptr          key;
    uz            peakvirtual;
    uz            virtualsize;
    u32           pagefaults;
    uz            peakworkingset;
    uz            workingset;       // bytes
    uz            peakpagedpool;
    uz            pagedpool;
    uz            peaknonpagedpool;
    uz            nonpagedpool;
    uz            pagefileusage;
    uz            peakpagefileusage;
    uz            privatebytes;     // "PrivatePageCount", actually bytes
    i64           readops;
    i64           writeops;
    i64           otherops;
    i64           readbytes;
    i64           writebytes;
    i64           otherbytes;
} NtProcessInfo;
_Static_assert(offsetof(NtProcessInfo, name)         ==  56, "");
_Static_assert(offsetof(NtProcessInfo, pid)          ==  80, "");
_Static_assert(offsetof(NtProcessInfo, workingset)   == 144, "");
_Static_assert(offsetof(NtProcessInfo, privatebytes) == 200, "");
_Static_assert(sizeof(NtProcessInfo) == 256, "");

// SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, one per logical processor
// in the current processor group.
typedef struct {
    i64 idletime;
    i64 kerneltime;     // includes idle time
    i64 usertime;
    i64 dpctime;
    i64 interrupttime;
    u32 interruptcount;
} NtProcessorPerf;
_Static_assert(sizeof(NtProcessorPerf) == 48, "");

W32(i32) NtQuerySystemInformation(i32, void *, u32, u32 *);
