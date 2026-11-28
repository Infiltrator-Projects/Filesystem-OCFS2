# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2
BASE = -std=c11 -Wall -Wextra -Werror -I Libraries/include
SRC = native/filesystems/ocfs2/ocfs2.c
HDR = native/filesystems/ocfs2/reader.h Libraries/include/infiltratr/filesystem_session_v2.h
.PHONY: all check clean
all: build/OCFS2.so build/freestanding/OCFS2.so
build/OCFS2.so: $(SRC) $(HDR) Makefile
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -fPIC -fvisibility=hidden -shared -Wl,-z,defs -o $@ $(SRC)
build/freestanding/OCFS2.so: $(SRC) $(HDR) Makefile
	mkdir -p build/freestanding
	$(CC) $(BASE) -O2 -march=x86-64 -DIFS_FREESTANDING -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -fPIC -fvisibility=hidden -shared -nostdlib -Wl,-z,defs,-Bsymbolic -o $@ $(SRC)
build/test_ocfs2: tests/test_ocfs2.c Libraries/tests/filesystem_reader_lifetime.h
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl -pthread
check: all build/test_ocfs2
	test "$$(nm -D --defined-only build/OCFS2.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test "$$(nm -D --defined-only build/freestanding/OCFS2.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test -z "$$(nm -D --undefined-only build/freestanding/OCFS2.so)"
	! readelf -d build/freestanding/OCFS2.so | grep -q NEEDED
	build/test_ocfs2 ./build/OCFS2.so
	build/test_ocfs2 ./build/freestanding/OCFS2.so
clean:
	rm -rf build
