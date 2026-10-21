#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"
#include "param.h"

#define TESTFILE "fstest.dat"
#define TESTBLOCKS (NDIRECT + 2)
#define TESTBYTES (TESTBLOCKS * BSIZE)

// Deliberately unaligned I/O sizes exercise reads/writes across blocks.
static uchar buffer[BSIZE + 37];

static uchar
pattern(uint offset)
{
  // Include the block number so swapping whole blocks is detectable.
  return (uchar)(offset * 37 + (offset / BSIZE) * 13 + offset / 251);
}

static int
check(char *label, int ok)
{
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", label);
  return ok ? 0 : 1;
}

static void
metadata(int fd, struct stat *st)
{
  printf(1, "\n[FILE METADATA]\n");
  printf(1, "File descriptor: %d (index into this process's ofile[])\n", fd);
  printf(1, "Type         : %s (%d)\n",
         st->type == T_FILE ? "Regular file" : "Other", st->type);
  printf(1, "Device       : %d\n", st->dev);
  printf(1, "Inode number : %d\n", st->ino);
  printf(1, "Link count   : %d\n", st->nlink);
  printf(1, "File size    : %d bytes\n", st->size);
}

static int
mappings(int fd)
{
  int bn, disk, table, i, failures = 0;
  int blocks[TESTBLOCKS];

  printf(1, "\n[DISK BLOCK MAPPING]\n");
  table = blockmap(fd, NDIRECT, 1);
  printf(1, "Indirect address table -> Disk Block %d\n", table);
  failures += check("Allocated indirect address table", table > 0);
  for(bn = 0; bn < TESTBLOCKS; bn++){
    disk = blockmap(fd, bn, 0);
    blocks[bn] = disk;
    printf(1, "Logical Block %d -> Disk Block %d [%s]\n",
           bn, disk, bn < NDIRECT ? "DIRECT" : "INDIRECT");
    if(disk <= 0 || disk == table)
      failures++;
    for(i = 0; i < bn; i++)
      if(disk == blocks[i])
        failures++;
    if(bn >= NDIRECT && blockmap(fd, bn, 1) != table)
      failures++;
  }
  failures += check("Distinct direct and indirect data blocks", failures == 0);
  return failures;
}

static int
invalid_tests(int fd)
{
  int failures = 0, other, pipefd[2];
  struct stat before, after;

  printf(1, "\n[DIAGNOSTIC VALIDATION]\n");
  if(fstat(fd, &before) < 0)
    return check("Metadata before diagnostic tests", 0);
  failures += check("Negative descriptor rejected", blockmap(-1, 0, 0) == -1);
  failures += check("Out-of-range descriptor rejected",
                    blockmap(NOFILE, 0, 0) == -1);
  failures += check("Negative logical block rejected", blockmap(fd, -1, 0) == -1);
  failures += check("Block at EOF rejected", blockmap(fd, TESTBLOCKS, 0) == -1);
  failures += check("Block above mapping limit rejected",
                    blockmap(fd, MAXFILE, 0) == -1);
  failures += check("Negative selector rejected", blockmap(fd, 0, -1) == -1);
  failures += check("Unknown selector rejected", blockmap(fd, 0, 2) == -1);
  failures += check("Direct block has no indirect table",
                    blockmap(fd, 0, 1) == -1);
  failures += check("Indirect table query beyond EOF rejected",
                    blockmap(fd, TESTBLOCKS, 1) == -1);

  other = dup(fd);
  if(other < 0){
    failures += check("Closed descriptor rejected", 0);
  } else {
    int closed = close(other) == 0;
    failures += check("Closed descriptor rejected",
                      closed && blockmap(other, 0, 0) == -1);
  }
  if(pipe(pipefd) < 0){
    failures += check("Pipe descriptor rejected", 0);
  } else {
    failures += check("Pipe descriptor rejected",
                      blockmap(pipefd[0], 0, 0) == -1);
    close(pipefd[0]);
    close(pipefd[1]);
  }
  other = open(".", O_RDONLY);
  if(other < 0){
    failures += check("Directory descriptor rejected", 0);
  } else {
    failures += check("Directory descriptor rejected", blockmap(other, 0, 0) == -1);
    close(other);
  }
  other = open(TESTFILE, O_WRONLY);
  if(other < 0){
    failures += check("Write-only descriptor rejected", 0);
  } else {
    failures += check("Write-only descriptor rejected", blockmap(other, 0, 0) == -1);
    close(other);
  }
  failures += check("Diagnostic leaves metadata unchanged",
                    fstat(fd, &after) == 0 && before.type == after.type &&
                    before.dev == after.dev && before.ino == after.ino &&
                    before.nlink == after.nlink && before.size == after.size);
  return failures;
}

static int
write_mode(void)
{
  int fd, n, i, count, done, failures = 0;
  struct stat st;
  uint total = 0;

  printf(1, "========== XV6 FILE SYSTEM LAB ==========\n");
  printf(1, "Writer PID: %d\n", getpid());
  printf(1, "BSIZE=%d NDIRECT=%d NINDIRECT=%d Test blocks=%d\n",
         BSIZE, NDIRECT, NINDIRECT, TESTBLOCKS);
  // This xv6 has no O_TRUNC. Replace only the lab's named test file.
  if(stat(TESTFILE, &st) == 0 &&
     (st.type != T_FILE || unlink(TESTFILE) < 0)){
    printf(1, "FAIL: Cannot replace %s\n", TESTFILE);
    return 1;
  }
  fd = open(TESTFILE, O_CREATE | O_RDWR);
  if(fd < 0){
    printf(1, "FAIL: Cannot create %s\n", TESTFILE);
    return 1;
  }
  printf(1, "\n[FILE CREATION]\nFile created: %s\n", TESTFILE);
  failures += check("New file is empty",
                    fstat(fd, &st) == 0 && st.type == T_FILE && st.size == 0);
  failures += check("Unallocated block rejected without allocation",
                    blockmap(fd, 0, 0) == -1 && blockmap(fd, NDIRECT, 1) == -1);
  while(total < TESTBYTES){
    count = TESTBYTES - total;
    if(count > sizeof(buffer))
      count = sizeof(buffer);
    for(i = 0; i < count; i++)
      buffer[i] = pattern(total + i);
    done = 0;
    while(done < count){
      n = write(fd, buffer + done, count - done);
      if(n <= 0 || n > count - done){
        printf(1, "FAIL: Write stopped after %d bytes\n", total + done);
        close(fd);
        return 1;
      }
      done += n;
    }
    total += done;
  }
  printf(1, "Bytes written: %d\n", total);
  if(fstat(fd, &st) < 0){
    printf(1, "FAIL: fstat\n");
    close(fd);
    return 1;
  }
  metadata(fd, &st);
  failures += check("Writer size and metadata",
                    total == TESTBYTES && st.size == TESTBYTES &&
                    st.type == T_FILE && st.nlink == 1);
  failures += mappings(fd);
  failures += check("File closed successfully", close(fd) == 0);
  failures += check("File created and written", failures == 0);
  printf(1, "Writer total failures: %d\n", failures);
  printf(1, "Run fstest verify after this writer exits.\n");
  return failures;
}

static int
verify_mode(void)
{
  int fd, n, i, failures = 0;
  uint total = 0, mismatches = 0, boundary_bytes = 0;
  struct stat st;

  printf(1, "========== FILE PERSISTENCE VERIFICATION ==========\n");
  printf(1, "Verifier PID: %d\n", getpid());
  // No O_CREATE and no unlink: verification must preserve the file.
  fd = open(TESTFILE, O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL: Cannot reopen %s; run fstest write first\n", TESTFILE);
    return 1;
  }
  printf(1, "File reopened successfully.\n");
  if(fstat(fd, &st) < 0){
    printf(1, "FAIL: fstat\n");
    close(fd);
    return 1;
  }
  metadata(fd, &st);
  printf(1, "Expected size: %d bytes\n", TESTBYTES);
  failures += check("Expected regular-file size and link count",
                    st.type == T_FILE && st.nlink == 1 && st.size == TESTBYTES);
  failures += mappings(fd);
  failures += invalid_tests(fd);
  // Read until EOF, handling short reads and checking every byte.
  while((n = read(fd, buffer, sizeof(buffer))) > 0){
    for(i = 0; i < n; i++){
      uint offset = total + i;
      int good = offset < TESTBYTES && buffer[i] == pattern(offset);
      if(!good){
        if(mismatches < 4)
          printf(1, "Mismatch at offset %d: expected=%d actual=%d\n",
                 offset, pattern(offset), buffer[i]);
        mismatches++;
      }
      if(offset == BSIZE - 1 || offset == BSIZE ||
         offset == 2 * BSIZE - 1 || offset == 2 * BSIZE ||
         offset == NDIRECT * BSIZE - 1 || offset == NDIRECT * BSIZE ||
         offset == TESTBYTES - 1){
        printf(1, "Boundary offset %d: expected=%d actual=%d [%s]\n",
               offset, pattern(offset), buffer[i], good ? "PASS" : "FAIL");
        if(good)
          boundary_bytes++;
      }
    }
    total += n;
  }
  failures += check("Read reached EOF without error", n == 0);
  printf(1, "Bytes verified: %d\nMismatches: %d\n", total, mismatches);
  failures += check("Entire deterministic data pattern",
                    total == TESTBYTES && mismatches == 0);
  failures += check("All block boundaries verified", boundary_bytes == 7);
  failures += check("Direct and indirect data verified",
                    total == TESTBYTES && mismatches == 0 && boundary_bytes == 7);
  failures += check("File closed successfully", close(fd) == 0);
  failures += check("File persisted after process termination", failures == 0);
  printf(1, "Verifier total failures: %d\n", failures);
  printf(1, "========== %s ==========\n",
         failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
  return failures;
}

int
main(int argc, char **argv)
{
  if(TESTBLOCKS < 14 || TESTBLOCKS > MAXFILE){
    printf(1, "FAIL: Test block count does not fit this filesystem\n");
    exit();
  }
  if(argc != 2){
    printf(1, "Usage: fstest write | fstest verify\n");
    exit();
  }
  if(strcmp(argv[1], "write") == 0)
    write_mode();
  else if(strcmp(argv[1], "verify") == 0)
    verify_mode();
  else
    printf(1, "Usage: fstest write | fstest verify\n");
  exit();
}
