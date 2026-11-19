#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "user/user.h"

static struct pinfo procs[NPROC];

static int
myqueue(void)
{
  int n = getprocs(procs, NPROC);

  for (int i = 0; i < n; i++)
    if (procs[i].pid == getpid())
      return procs[i].queue;
  return -1;
}

static void
worker(int window, int burst)
{
  unsigned long cnt = 0;
  int t0 = uptime();

  for (;;) {
    if ((++cnt & 255) == 0 && uptime() - t0 >= window)
      break;
  }
  printf("worker pid %d (burst %d): ends in queue %d, %d k iterations\n",
         getpid(), burst, myqueue(), (int)(cnt / 1000));
}

int
main(int argc, char *argv[])
{
  int aging = argc > 1 ? atoi(argv[1]) : 20;
  int burst[3] = {1, 10, 10};
  int conf[3] = {100, 50, 50};
  int pid;

  for (int i = 0; i < 3; i++) {
    if ((pid = fork()) == 0) {
      worker(150, burst[i]);
      exit(0);
    }
    setmlfq(pid, burst[i], conf[i]);
  }

  // The parent stays in queue 3 (FCFS, not preempted), so with CPUS=1 the
  // children age to queue 2 while it spins. No printing here: a blocking
  // console write would let the queue-3 children run before they age.
  int t0 = uptime();
  while (uptime() - t0 < aging + 10)
    ;

  for (int i = 0; i < 3; i++)
    wait(0);
  exit(0);
}