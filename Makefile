CC       = gcc
CFLAGS   = -std=c11 -nostdlib \
           -Wall -Wextra -Wconversion -Wno-sign-conversion -Wno-unused-function \
           -Wdouble-promotion -Wshadow -Wnull-dereference -Wstrict-prototypes -Wvla
# Release only: debuggers and profilers need the unwind tables to walk the
# stack on x64 Windows.
RELEASE  = -O2 -s -fno-asynchronous-unwind-tables
DEBUG    = -g3 -DDEBUG -fsanitize=undefined -fsanitize=bounds-strict \
           -fsanitize-trap -ftrivial-auto-var-init=pattern
# libgcc for ___chkstk_ms, which frames over a page call. -nostdlib keeps
# msvcrt out, so a stray libc call is a link error rather than an import.
LDLIBS   = -lkernel32 -lntdll -lgcc
TCC      = tcc
SRC      = base.c win32.c sample.c ui.c

top.exe: main.c $(SRC)
	$(CC) $(CFLAGS) $(RELEASE) -o $@ main.c $(LDLIBS)

top-debug.exe: main.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ main.c $(LDLIBS)

# tcc's bundled kernel32.def predates GetTickCount64 and friends, so make a
# complete one from the installed DLL.
kernel32.def:
	$(TCC) -impdef kernel32.dll -o $@

top-tcc.exe: main.c $(SRC) kernel32.def
	$(TCC) -nostdlib -Wl,-e,mainCRTStartup -Wl,-subsystem=console -o $@ main.c -L. -lkernel32 -ltcc1 -lntdll

top-test.exe: test.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ test.c $(LDLIBS)

top-test-tcc.exe: test.c $(SRC) kernel32.def
	$(TCC) -DDEBUG -nostdlib -Wl,-e,mainCRTStartup -Wl,-subsystem=console -o $@ test.c -L. -lkernel32 -ltcc1 -lntdll

test: top-test.exe
	./top-test.exe

clean:
	rm -f top.exe top-debug.exe top-tcc.exe top-test.exe top-test-tcc.exe kernel32.def

.PHONY: test clean
