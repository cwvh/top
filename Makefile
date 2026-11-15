CC       = gcc
CFLAGS   = -std=c11 -fno-asynchronous-unwind-tables -nostartfiles \
           -Wall -Wextra -Wconversion -Wno-sign-conversion -Wno-unused-function \
           -Wshadow -Wnull-dereference -Wcast-align -Wdouble-promotion
DEBUG    = -g3 -DDEBUG -fsanitize=undefined -fsanitize=bounds-strict \
           -fsanitize-trap -ftrivial-auto-var-init=pattern
LDLIBS   = -lkernel32 -lntdll
TCC      = tcc
SRC      = base.c win32.c sample.c ui.c

top.exe: main.c $(SRC)
	$(CC) $(CFLAGS) -O2 -s -o $@ main.c $(LDLIBS)

top-debug.exe: main.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ main.c $(LDLIBS)

# tcc's bundled kernel32.def predates GetTickCount64 and friends, so make a
# complete one from the installed DLL.
kernel32.def:
	$(TCC) -impdef kernel32.dll -o $@

top-tcc.exe: main.c $(SRC) kernel32.def
	$(TCC) -nostdlib -Wl,-e,mainCRTStartup -Wl,-subsystem=console -o $@ main.c -L. -lkernel32 -lntdll

top-test.exe: test.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ test.c $(LDLIBS)

top-test-tcc.exe: test.c $(SRC) kernel32.def
	$(TCC) -DDEBUG -nostdlib -Wl,-e,mainCRTStartup -Wl,-subsystem=console -o $@ test.c -L. -lkernel32 -lntdll

test: top-test.exe
	./top-test.exe

clean:
	rm -f top.exe top-debug.exe top-tcc.exe top-test.exe top-test-tcc.exe kernel32.def

.PHONY: test clean
