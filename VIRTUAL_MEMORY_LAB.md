# xv6 virtual memory and process isolation lab

## Objective

Inspect global, stack, and heap mappings in xv6-x86, decode page permissions,
and demonstrate that `fork()` gives the child separate physical memory.
Invalid requests must return `-1` while the program and kernel keep running.

## Existing xv6 functions

- `walkpgdir()` finds the PTE for a virtual address. Passing `alloc=0` prevents
  allocation of page-table pages.
- `mappages()` installs virtual-to-physical page mappings and permissions.
- `copyuvm()` allocates new physical pages and copies the parent's contents
  into them during `fork()`, preserving virtual addresses and PTE flags.

These functions and the existing `fork()` implementation are unchanged.

## Implementation

| File | Change |
| --- | --- |
| `vminfo.h` (new) | Shared structure containing physical page base and raw PTE flags. |
| `vm.c` | Adds `lookupuvm()`, a read-only inspection helper for the calling process. |
| `defs.h` | Declares the helper and structure type. |
| `sysproc.c` | Adds `sys_vmlookup()` with argument, range, mapping, and output checks. |
| `syscall.h` | Assigns the previously unused syscall number 24. |
| `syscall.c` | Adds the syscall handler and its tracing name. |
| `user.h` | Declares `int vmlookup(uint, struct vminfo*)`. |
| `usys.S` | Adds the user syscall stub. |
| `memtest.c` (new) | Runs mapping, permission, fork-isolation, and invalid-input checks. |
| `Makefile` | Adds `_memtest` to `UPROGS`, preserving all existing programs. |
| `VIRTUAL_MEMORY_LAB.md` (new) | Documents the experiment and observed results. |

`sys_vmlookup()` obtains arguments with xv6's `argint()` and `argptr()`.
The requested address must be below both the process break (`proc->sz`) and
`KERNBASE`. Its PDE and PTE must be present and user-accessible. The helper
calls `walkpgdir(..., 0)` and returns `PTE_ADDR()` and `PTE_FLAGS()`.

The result is first held in a kernel-local `struct vminfo`. Before calling
this repository's existing `copyout()`, the syscall validates the entire
output range with subtraction to avoid overflow, then checks every touched
page for present, user, and write permissions (including PDE write permission).
This also rejects the stack guard page and prevents partial copies into an
invalid cross-page buffer. The kernel never directly dereferences the supplied
output pointer. Success returns `0`; invalid requests return `-1`.

## Working principle

A virtual address belongs to a process's address space. An x86 page directory
and page table translate it to a physical RAM page. Pages are 4096 bytes:
`physical address = physical page base + (virtual address & 0xfff)`.

PTE flags include P (present), W (writable), and U (user-accessible).
Basic x86 PTEs have no separate read-permission bit, and standard xv6-x86 does
not independently control execute permission. Raw flags may also contain
hardware-maintained accessed and dirty bits; the observed `0x67` includes both.

The global variable belongs to the program's data region, the local variable
to its stack, and the allocated integer to its heap. `memtest` looks up the
heap allocation itself, rather than the stack variable holding its pointer.

The parent records its initial values and mappings before `fork()`. The child
records its inherited state, changes its values to 999/888/777, and sends both
snapshots to the parent through a pipe. The parent reads the complete report
and calls `wait()` before printing. Comparisons use the reported values and
addresses; the parent never dereferences a child's pointer. Eager copying in
`copyuvm()` explains why matching virtual addresses have different physical
pages even before the child writes them.

## Execution instructions

Run these commands in Ubuntu WSL:

```bash
cd /home/harshit/OSGroup/lab-da-1-system-call-tracing-in-xv6-harshmesh
make -j2 xv6.img fs.img
make qemu-nox
```

The repository's `qemu-nox` target was verified and used for testing. At the
xv6 shell prompt, run:

```text
memtest
tracetest
race
```

Exit QEMU by pressing Ctrl+A, then X.

## Results

Verified on 2026-10-21 in Ubuntu 24.04.1 LTS under WSL2, using the existing
32-bit GCC toolchain and `qemu-system-i386`. `make -j2 xv6.img fs.img` compiled
the kernel and every user program, including `tracetest` and `race`, without
errors or cleanup. The linker emitted executable-stack and RWX-segment metadata
warnings; these did not prevent the build or execution.

Observed mapping values from the QEMU run (all flags were `P W U`, raw `0x67`):

| Variable | Same VA in both processes | Parent physical page | Child physical page |
| --- | --- | --- | --- |
| Global | `0x1DF8` | `0xDEE0000` | `0xDF74000` |
| Local (stack) | `0x3F98` | `0xDEDE000` | `0xDEDC000` |
| Heap | `0xBFF8` | `0xDFC4000` | `0xDED4000` |

Parent PID 3 and child PID 4 initially held 100/200/300. The child then held
999/888/777, while the parent retained 100/200/300. Physical addresses and PIDs
are runtime observations and may change on another run.

Actual terminal output excerpt:

```text
[4] INVALID ADDRESS AND OUTPUT BUFFER TESTS
PASS: Address at process break rejected
PASS: Unmapped user address rejected
PASS: Kernel address rejected
PASS: High invalid address rejected
PASS: Stack guard mapping rejected
PASS: Kernel output buffer rejected
PASS: Guard output buffer rejected
PASS: Buffer spanning user page and guard rejected
PASS: Partly out-of-range output buffer rejected
PASS: Wrapping output buffer rejected
PASS: Valid output buffer spanning two pages
PASS: Scratch allocation released
PASS: Valid lookup still works after invalid requests

[5] VERIFICATION SUMMARY
PASS: Memory mapping
PASS: Permission flags
PASS: Parent-child report and wait
PASS: Same virtual addresses
PASS: Different physical pages for parent and child
PASS: Parent-child memory isolation
PASS: Invalid address and output buffer handling
Total failures: 0
========== LAB COMPLETED ==========
```

`tracetest` printed traces for `trace`, `getpid`, `fork`, and `wait`. `race`
printed `Expected counter = 400000` and `Actual counter   = 400000`.
All three programs returned to the shell; no kernel panic occurred.

Final review confirmed that `proc.c`, `proc.h`, `mmu.h`, `tracetest.c`, and
`race.c` are unchanged, and removing the added helper/include from `vm.c`
reproduces its original contents exactly. No existing program was removed.

## Conclusion

The observations demonstrate address translation and page permissions for
three memory regions. Matching virtual addresses in parent and child map to
separate physical pages, so the child's writes leave the parent's values
unchanged. Invalid lookups and output buffers are rejected safely.
