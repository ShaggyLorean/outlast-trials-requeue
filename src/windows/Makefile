ROOT := ../..
LOCAL_CROSS := $(ROOT)/work/mingw-root/usr/bin/x86_64-w64-mingw32-
CROSS ?= $(if $(wildcard $(LOCAL_CROSS)gcc),$(LOCAL_CROSS),x86_64-w64-mingw32-)
CC := $(CROSS)gcc
WINDRES := $(CROSS)windres
STRIP := $(CROSS)strip
OUT := $(ROOT)/build/windows

CPPFLAGS := -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 \
	-I$(ROOT)/src/common
CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror -Wpedantic -ffunction-sections -fdata-sections
LDFLAGS := -municode -mwindows -Wl,--gc-sections -static-libgcc
LDLIBS := -luser32 -lshell32 -lole32 -luuid -ladvapi32 -lcomctl32 -ldwmapi -lgdi32

.PHONY: all clean audit

all: $(OUT)/OutlastRequeue.exe audit

$(OUT):
	mkdir -p $@

$(OUT)/outlast_requeue.res: outlast_requeue.rc resource.h outlast_requeue.manifest $(ROOT)/assets/outlast-requeue.ico | $(OUT)
	$(WINDRES) -I. $< -O coff -o $@

$(OUT)/OutlastRequeue.exe: outlast_requeue_windows.c $(ROOT)/src/common/requeue_engine.c $(ROOT)/src/common/requeue_engine.h $(OUT)/outlast_requeue.res | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(ROOT)/src/common/requeue_engine.c $(OUT)/outlast_requeue.res $(LDFLAGS) $(LDLIBS) -o $@
	$(STRIP) $@

audit: $(OUT)/OutlastRequeue.exe
	@! strings $< | grep -E 'SetForegroundWindow|SetActiveWindow|SetFocus|BringWindowToTop|AttachThreadInput|SendInput|keybd_event|mouse_event|GetCursorPos|SetCursorPos|ChangeDisplaySettings|MessageBeep|PlaySound|FlashWindow' \
		|| { echo 'Forbidden focus/input/display API found' >&2; exit 1; }

clean:
	rm -rf $(OUT)
