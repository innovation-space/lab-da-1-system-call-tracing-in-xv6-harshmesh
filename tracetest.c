#include "types.h"
#include "stat.h"
#include "user.h"

int
main(void)
{
  // Enable tracing
  trace(1);

  // Test getpid()
  getpid();

  // Test fork()
  if(fork() == 0){
    // Child process
    getpid();
    exit();
  }

  // Parent waits for child
  wait();

  // Disable tracing
  trace(0);

  // This should NOT be traced
  getpid();

  exit();
}
