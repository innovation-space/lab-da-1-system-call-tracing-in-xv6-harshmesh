# xv6 filesystem, inodes, and block allocation lab

## Objective

Create a file, inspect its metadata and allocated disk blocks, then verify
all its contents in a separate process after the writer exits. The test
covers 14 blocks, including two in the single-indirect region.

Repository: `/home/harshit/OSGroup/lab-da-1-system-call-tracing-in-xv6-harshmesh`.
Implementation and testing were performed on `main` in Ubuntu 24.04.1 WSL2.
The working tree was clean before implementation. No Git state or history
was changed; all lab edits remain uncommitted.

## File descriptors and inodes

A file descriptor is an integer index into the current process's `ofile[]`
array (`proc.h`), rather than a disk address. Its `struct file` (`file.h`)
holds the reference count, readable/writable flags, current byte offset,
file kind, and inode pointer. `dup()` and `fork()` can share this open-file
structure and its offset.

`struct stat` (`stat.h`) exposes type, filesystem device number, inode
number, link count, and file size to user programs through `fstat()`.
`struct inode` is the in-memory representation: it adds the device/inode
identity, reference count, validity flag, and sleep lock to cached metadata
and block addresses. `struct dinode` (`fs.h`) stores type, device-node
major/minor numbers, link count, size, and block addresses on disk. Its inode
number follows from its position in the inode area; its filesystem device
comes from the disk holding it. `stat.dev` is that filesystem device, not
a device node's major/minor pair.

Directories contain `struct dirent` entries pairing names with inode numbers.
A directory entry keeps a file linked even after every process closes it.

The normal write/read path is:

```text
user open -> sys_open -> create/namei -> inode
                     -> filealloc -> struct file
                     -> fdalloc -> process ofile[fd]
user write/read -> sys_write/sys_read -> argfd -> struct file
                -> filewrite/fileread -> writei/readi
                -> bmap -> bread -> buffer cache -> iderw -> disk
```

`sys_open()` initializes the inode reference, access flags, and offset.
`filewrite()` splits writes into transactions, locks the inode, and updates
the open-file offset. `writei()` changes data buffers and size; `iupdate()`
records inode changes. `log_write()` registers dirty blocks, and `end_op()`
commits when the last outstanding transaction ends. `fileread()` locks the
inode, calls `readi()`, and advances the offset. Closing releases the open-file
reference; a linked inode and its blocks survive process termination.

## Disk layout and block mapping

Actual repository constants are `BSIZE=512`, `NDIRECT=12`,
`NINDIRECT=BSIZE/sizeof(uint)=128`, and `MAXFILE=140` blocks (71,680 bytes).
The 1,000-block image used during testing had this layout:

| Region | Disk blocks |
| --- | --- |
| Boot block | 0 |
| Superblock | 1 |
| Journal/log | 2 through 31 |
| Inodes | 32 through 57 |
| Allocation bitmap | 58 |
| Data area | 59 through 999 |

The superblock records these region boundaries. `balloc()` finds a free
bitmap bit, marks it allocated, and zeroes the new block within a transaction.

`bmap()` in `fs.c` handles logical blocks 0 through 11 with `ip->addrs[bn]`.
For subsequent blocks it subtracts `NDIRECT`, reads the address table at
`ip->addrs[NDIRECT]`, and selects that table entry. It can allocate missing
data blocks and the indirect table. The table block itself holds addresses,
rather than file payload bytes. The existing `bmap()`, allocator, read/write
paths, inode format, and transaction behavior are unchanged.

## Read-only diagnostic

The new syscall 25 has this user interface:

```c
int blockmap(int fd, int logical_block, int indirect_table);
```

Selector 0 returns the actual data block. Selector 1 returns the indirect
address-table block for a valid allocated indirect logical block. Errors
return `-1`. All arguments and results are integers, so the interface needs
no user output buffer and exposes no kernel pointers.

`sys_blockmap()` uses `argfd()`/`argint()`, requires an existing readable inode
descriptor, and locks the inode. The new `blocklookup()` requires a regular
file, checks the logical block against EOF and `MAXFILE`, rejects missing or
out-of-data-area addresses, and uses only `bread()`/`brelse()` for indirect
entries. It never calls `bmap()`, `balloc()`, or `log_write()`. The existing
file reference remains held; the diagnostic neither changes the offset nor
drops an inode reference, so it needs no write transaction.

## Test program and files

`fstest write` replaces only `fstest.dat` because this xv6 has no `O_TRUNC`.
It derives the size from `(NDIRECT+2)*BSIZE`, writes using 549-byte buffers,
handles short writes, prints `fstat()` metadata and every mapping, and closes
the descriptor. An empty-file lookup is checked before writing.

`fstest verify` opens the file without `O_CREATE`, validates metadata, prints
mappings, and reads until EOF with short-read handling. It reconstructs each
byte as `(offset*37 + (offset/BSIZE)*13 + offset/251) & 255`, without writer
memory or saved expected data. Every byte is checked, with extra output at
511/512, 1023/1024, 6143/6144, and the final byte, 7167. It reports the total
bytes read, mismatches, and PASS/FAIL results. Diagnostic checks reject
negative/out-of-range/closed descriptors, pipes, directories, write-only
files, invalid selectors, and invalid logical blocks.

| File | Purpose |
| --- | --- |
| `fstest.c` (new) | Writer, independent verifier, metadata, mappings, and checks. |
| `FILESYSTEM_LAB.md` (new) | Lab explanation, commands, and observed results. |
| `fs.c` | Adds the non-allocating `blocklookup()` helper. |
| `defs.h` | Declares the helper. |
| `sysfile.c` | Adds `sys_blockmap()` using an existing descriptor. |
| `syscall.h` | Assigns syscall number 25, after `vmlookup` at 24. |
| `syscall.c` | Registers the handler and tracing name. |
| `user.h` | Declares the user interface. |
| `usys.S` | Adds the syscall stub. |
| `Makefile` | Adds `_fstest` while preserving every existing user program. |

Build outputs include `_fstest`. The already tracked `_memtest` binary was
rebuilt because the shared `usys.o` changed; `memtest.c` is unchanged.

## Build and demonstration

Run in Ubuntu WSL:

```bash
cd /home/harshit/OSGroup/lab-da-1-system-call-tracing-in-xv6-harshmesh
make -j2 xv6.img fs.img
make qemu-nox
```

At the xv6 shell, run each command after the previous command returns:

```text
fstest write
fstest verify
memtest
tracetest
race
```

For the restart demonstration, exit QEMU with Ctrl+A, then X. Run
`make qemu-nox` again and execute only `fstest verify`. Keep the same `fs.img`:
`make clean` or a rebuild of that image removes the runtime test file.

## Actual observed results

The kernel and all user programs compiled on 2026-10-21. The linker emitted
executable-stack/RWX metadata warnings, but the build completed without errors.
No cleanup was needed. The following results were observed in QEMU:

- Writer PID 5 exited before verifier PID 6 ran. Both reported zero failures.
- Metadata: regular file (type 2), device 1, inode 23, link count 1, size 7,168.
- Logical blocks 0 through 11 mapped to disk blocks 602 through 613 in order.
- Indirect table: disk block 614; logical blocks 12/13 mapped to blocks 615/616.
- All 7,168 bytes and all seven displayed boundary positions matched.
- Invalid diagnostic requests were rejected without a kernel panic.
- Verification before file creation failed without creating the file.
- After normal QEMU exit/restart, verification passed with the same mappings.

Actual terminal excerpt:

```text
Boundary offset 6143: expected=130 actual=130 [PASS]
Boundary offset 6144: expected=180 actual=180 [PASS]
Boundary offset 7167: expected=160 actual=160 [PASS]
PASS: Read reached EOF without error
Bytes verified: 7168
Mismatches: 0
PASS: Entire deterministic data pattern
PASS: All block boundaries verified
PASS: Direct and indirect data verified
PASS: File closed successfully
PASS: File persisted after process termination
Verifier total failures: 0
========== ALL TESTS PASSED ==========
```

An independent host-side decode of `fs.img` matched all 14 reported block
mappings, the indirect table, allocation bitmap bits, inode metadata, and
every data byte. SHA-256 comparisons confirmed that verification and its
invalid-input diagnostics left the entire image byte-for-byte unchanged.

A negative test ran `echo corrupted > fstest.dat` in xv6. Despite the size
remaining 7,168 bytes, verification found 10 mismatches and printed
`TESTS FAILED`. Running the writer and verifier again restored the correct
file and reported zero failures. The image is left with that correct file.

Previous labs also ran: `memtest` reported zero failures; `tracetest` printed
syscall traces; `race` reported expected and actual counts of 400,000.
Source comparison confirmed that previous lab sources and the original
filesystem functions remain unchanged. No commits, pushes, branch changes,
staging, resets, stashes, or other Git mutations were performed.

## Conclusion

The experiment links a process's file descriptor to its open-file object,
inode, and allocated disk blocks. Direct and indirect mappings describe the
same data verified after process exit and QEMU restart. The corruption test
confirms that success depends on the contents, rather than the file size alone.
