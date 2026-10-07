#include "types.h"
#include "stat.h"
#include "user.h"

#define NPROC 4
#define NINC 100000

int
main(void)
{
  int i;

  counter(0, 0);

  printf(1, "Starting race condition test...\n");
  printf(1, "Processes: %d\n", NPROC);
  printf(1, "Increments per process: %d\n", NINC);

  for(i = 0; i < NPROC; i++){
    if(fork() == 0){
      counter(1, NINC);
      exit();
    }
  }

  for(i = 0; i < NPROC; i++)
    wait();

  printf(1, "Expected counter = %d\n", NPROC * NINC);
  printf(1, "Actual counter   = %d\n", counter(2, 0));

  exit();
}
