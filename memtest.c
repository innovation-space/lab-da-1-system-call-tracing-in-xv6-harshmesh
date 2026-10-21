#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"
#include "memlayout.h"
#include "vminfo.h"

int global_var = 100;

struct snapshot {
  int pid;
  int value[3];
  uint va[3];
  struct vminfo map[3];
  int mapped;
};

struct child_report {
  struct snapshot before;
  struct snapshot after;
};

static char *names[] = {"GLOBAL", "LOCAL (stack)", "HEAP"};

static void
capture(struct snapshot *s, int *local, int *heap)
{
  int i;

  memset(s, 0, sizeof(*s));
  s->pid = getpid();
  s->value[0] = global_var;
  s->value[1] = *local;
  s->value[2] = *heap;
  s->va[0] = (uint)&global_var;
  s->va[1] = (uint)local;
  s->va[2] = (uint)heap;
  s->mapped = 1;
  for(i = 0; i < 3; i++)
    if(vmlookup(s->va[i], &s->map[i]) < 0)
      s->mapped = 0;
}

static void
show(char *label, struct snapshot *s)
{
  int i;
  uint flags;

  printf(1, "\n%s: PID = %d\n", label, s->pid);
  for(i = 0; i < 3; i++){
    flags = s->map[i].flags;
    printf(1, "%s: value=%d VA=0x%x\n", names[i], s->value[i], s->va[i]);
    printf(1, "  Physical page=0x%x Flags=%s %s %s (raw 0x%x)\n",
           s->map[i].physical_page, flags & PTE_P ? "P" : "-",
           flags & PTE_W ? "W" : "-", flags & PTE_U ? "U" : "-", flags);
    // A page base plus the original offset gives this byte's PA.
    printf(1, "  Physical address=0x%x\n",
           s->map[i].physical_page + (s->va[i] & (PGSIZE - 1)));
  }
}

static int
permissions(struct snapshot *s)
{
  int i;

  if(!s->mapped)
    return 0;
  for(i = 0; i < 3; i++)
    if((s->map[i].flags & (PTE_P | PTE_W | PTE_U)) !=
       (PTE_P | PTE_W | PTE_U) || s->map[i].physical_page % PGSIZE != 0)
      return 0;
  return 1;
}

static int
values(struct snapshot *s, int g, int l, int h)
{
  return s->value[0] == g && s->value[1] == l && s->value[2] == h;
}

static int
check(char *label, int ok)
{
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", label);
  return ok ? 0 : 1;
}

static int
readall(int fd, void *buf, int size)
{
  int n, done = 0;

  while(done < size){
    n = read(fd, (char*)buf + done, size - done);
    if(n <= 0)
      return -1;
    done += n;
  }
  return 0;
}

static int
invalid_tests(uint valid, uint guard)
{
  struct vminfo info;
  uint end = (uint)sbrk(0);
  char *space;
  int failures = 0;

  // The byte at the break is outside the permitted range even if its
  // containing page is present. end+PGSIZE is an unmapped user address.
  failures += check("Address at process break rejected",
                    vmlookup(end, &info) == -1);
  failures += check("Unmapped user address rejected",
                    vmlookup(end + PGSIZE, &info) == -1);
  failures += check("Kernel address rejected",
                    vmlookup(KERNBASE, &info) == -1);
  failures += check("High invalid address rejected",
                    vmlookup(0xffffffff, &info) == -1);
  // exec.c puts a present, non-user guard page below the stack.
  failures += check("Stack guard mapping rejected",
                    vmlookup(guard, &info) == -1);
  failures += check("Kernel output buffer rejected",
                    vmlookup(valid, (struct vminfo*)KERNBASE) == -1);
  failures += check("Guard output buffer rejected",
                    vmlookup(valid, (struct vminfo*)guard) == -1);
  failures += check("Buffer spanning user page and guard rejected",
                    vmlookup(valid, (struct vminfo*)(guard - 4)) == -1);
  failures += check("Partly out-of-range output buffer rejected",
                    vmlookup(valid, (struct vminfo*)(end - 4)) == -1);
  failures += check("Wrapping output buffer rejected",
                    vmlookup(valid, (struct vminfo*)0xfffffffc) == -1);

  // Allocate scratch pages directly, beyond malloc's existing arena.
  space = sbrk(2 * PGSIZE);
  if(space == (char*)-1){
    failures += check("Cross-page output buffer", 0);
  } else {
    struct vminfo expected, result;
    uint cross = PGROUNDUP((uint)space + 1) - 4;
    int ok;

    ok = vmlookup(valid, &expected) == 0 &&
         vmlookup(valid, (struct vminfo*)cross) == 0;
    // memmove avoids assumptions about the alignment of cross.
    memmove(&result, (void*)cross, sizeof(result));
    failures += check("Valid output buffer spanning two pages",
                      ok && result.physical_page == expected.physical_page &&
                      (result.flags & 7) == (expected.flags & 7));
    failures += check("Scratch allocation released",
                      sbrk(-2 * PGSIZE) != (char*)-1);
  }
  failures += check("Valid lookup still works after invalid requests",
                    vmlookup(valid, &info) == 0);
  return failures;
}

int
main(void)
{
  int local_var = 200;
  int *heap_var = malloc(sizeof(int));
  struct snapshot initial, parent;
  struct child_report child;
  int fd[2], pid, received, reaped, i;
  int mapped, perms, sameva, separate, isolated, invalid, failures = 0;
  uint guard = PGROUNDDOWN((uint)&local_var) - PGSIZE;

  if(heap_var == 0){
    printf(1, "FAIL: Memory allocation failed\n");
    exit();
  }
  *heap_var = 300;
  printf(1, "========== XV6 VIRTUAL MEMORY LAB ==========\n");
  printf(1, "\n[1] MEMORY MAPPING AND PERMISSIONS\n");
  capture(&initial, &local_var, heap_var);
  show("BEFORE FORK", &initial);
  printf(1, "P=present W=writable U=user-accessible; pages are 4096 bytes.\n");
  printf(1, "Basic x86 PTEs have no separate read bit; this xv6 setup\n");
  printf(1, "does not independently control execute permission.\n");
  printf(1, "\n[2] PROCESS ISOLATION\n");

  if(pipe(fd) < 0){
    printf(1, "FAIL: pipe failed\n");
    free(heap_var);
    exit();
  }
  pid = fork();
  if(pid < 0){
    close(fd[0]);
    close(fd[1]);
    printf(1, "FAIL: fork failed\n");
    free(heap_var);
    exit();
  }
  if(pid == 0){
    close(fd[0]);
    capture(&child.before, &local_var, heap_var);
    show("CHILD INITIAL", &child.before);
    global_var = 999;
    local_var = 888;
    *heap_var = 777;
    capture(&child.after, &local_var, heap_var);
    show("CHILD MODIFIED", &child.after);
    if(write(fd[1], &child, sizeof(child)) != sizeof(child))
      printf(1, "FAIL: child report write\n");
    close(fd[1]);
    free(heap_var);
    exit();
  }

  // Only the child prints until its report is sent and wait completes.
  close(fd[1]);
  memset(&child, 0, sizeof(child));
  received = readall(fd[0], &child, sizeof(child)) == 0;
  close(fd[0]);
  reaped = wait() == pid;
  capture(&parent, &local_var, heap_var);
  show("PARENT AFTER CHILD", &parent);

  mapped = initial.mapped && parent.mapped && received &&
           child.before.mapped && child.after.mapped;
  perms = permissions(&initial) && permissions(&parent) && received &&
          permissions(&child.before) && permissions(&child.after);
  sameva = received;
  separate = mapped;
  printf(1, "\n[3] PHYSICAL MEMORY COMPARISON\n");
  for(i = 0; i < 3; i++){
    printf(1, "%s: parent page=0x%x child page=0x%x\n", names[i],
           parent.map[i].physical_page, child.before.map[i].physical_page);
    if(initial.va[i] != parent.va[i] ||
       parent.va[i] != child.before.va[i] ||
       parent.va[i] != child.after.va[i])
      sameva = 0;
    if(parent.map[i].physical_page == child.before.map[i].physical_page ||
       child.before.map[i].physical_page != child.after.map[i].physical_page ||
       initial.map[i].physical_page != parent.map[i].physical_page)
      separate = 0;
  }
  isolated = received && reaped && child.before.pid == pid &&
             child.after.pid == pid && parent.pid != pid &&
             values(&initial, 100, 200, 300) &&
             values(&child.before, 100, 200, 300) &&
             values(&child.after, 999, 888, 777) &&
             values(&parent, 100, 200, 300);

  printf(1, "\n[4] INVALID ADDRESS AND OUTPUT BUFFER TESTS\n");
  invalid = invalid_tests((uint)&global_var, guard);
  printf(1, "\n[5] VERIFICATION SUMMARY\n");
  failures += check("Memory mapping", mapped);
  failures += check("Permission flags", perms);
  failures += check("Parent-child report and wait", received && reaped);
  failures += check("Same virtual addresses", sameva);
  failures += check("Different physical pages for parent and child", separate);
  failures += check("Parent-child memory isolation", isolated);
  failures += check("Invalid address and output buffer handling", invalid == 0);
  failures += invalid;
  free(heap_var);
  printf(1, "Total failures: %d\n", failures);
  printf(1, "========== LAB %s ==========\n",
         failures == 0 ? "COMPLETED" : "FAILED");
  exit();
}
