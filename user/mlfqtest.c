#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "user/user.h"

static struct pinfo snap[2][NPROC];
static int snapn[2];
static int snaptick[2];
static char *states[] = {"UNUSED", "USED", "SLEEP", "RUNNABLE", "RUN", "ZOMBIE"};

// No printing while the parent hogs the CPU: a blocking console write would
// let the queue-3 workers run to completion before they age.
static void
take(int k)
{
  snapn[k] = getprocs(snap[k], NPROC);
  snaptick[k] = uptime();
}

static void
print(int k, char *title)
{
  printf("--- %s (tick %d) ---\n", title, snaptick[k]);
  printf("PID PPID STATE QUEUE WAIT BURST CONF NAME\n");
  for (int i = 0; i < snapn[k]; i++) {
    struct pinfo *p = &snap[k][i];
    printf("%d %d %s %d %d %d %d %s\n", p->pid, p->ppid,
           (p->state >= 0 && p->state <= 5) ? states[p->state] : "?",
           p->queue, p->waiting, p->burst, p->confidence, p->name);
  }
}

// Counts loop iterations in a fixed wall-clock window; the process that is
// scheduled more often gets a larger count.
static void
worker(int window)
{
  unsigned long cnt = 0;
  int t0 = uptime();

  for (;;) {
    if ((++cnt & 255) == 0 && uptime() - t0 >= window)
      break;
  }
  printf("worker pid %d: %d k iterations\n", getpid(), (int)(cnt / 1000));
}

int
main(int argc, char *argv[])
{
  int aging = argc > 1 ? atoi(argv[1]) : 800;
  int window = 100;
  int pa, pb;

  if ((pa = fork()) == 0) {
    worker(window);
    exit(0);
  }
  if ((pb = fork()) == 0) {
    worker(window);
    exit(0);
  }

  setmlfq(pa, 5, 70);
  setmlfq(pb, 20, 70);

  // The parent is in queue 3 (FCFS) and is not preempted by the clock,
  // so with CPUS=1 the children wait RUNNABLE and age.
  take(0);
  int t0 = uptime();
  while (uptime() - t0 < aging + 20)
    ;
  take(1);

  printf("worker A=%d (burst 5), worker B=%d (burst 20)\n", pa, pb);
  print(0, "just after fork");
  print(1, "after aging limit");

  wait(0);
  wait(0);
  exit(0);
}