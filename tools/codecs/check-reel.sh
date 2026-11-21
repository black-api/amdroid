#!/bin/bash
# Lists the functions in Reel's final binary that would break in ring 3 on am-droid:
#   FS  reads the thread pointer (%fs) — Reel has no TLS, so this faults at address ~0
#   SC  executes a Linux `syscall` instruction — the kernel takes int $0x80 only (#UD)
# Expected leftovers, all off Reel's path: locale/time-zone code (strftime, nl_langinfo,
# lctrans, mbrtowc/wcrtomb for %lc/%ls), real FILE streams (__lockfile, __stdio_*), and
# musl's file mapping (__map_file, mmap) used by time zones. Anything new in the list
# needs a look (and usually a replacement in src/user/reel/glue.c).
ELF=${1:-out/user/reel.elf}
objdump -d --no-show-raw-insn "$ELF" | awk '/^[0-9a-f]+ <.*>:$/{fn=$2} /%fs:/{print "FS " fn} /\tsyscall/{print "SC " fn}' | sort -u
