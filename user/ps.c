#include "kernel/types.h"
#include "kernel/pinfo.h"
#include "user/user.h"
#include "kernel/param.h"

// xv6 printf has no width support, so pad numbers by hand.
static void
num(int n, int width)
{
  int d = 1;

  for (int t = n; t >= 10; t /= 10)
    d++;
  printf("%d", n);
  for (; d < width; d++)
    printf(" ");
}

static void
str(char *s, int width)
{
  printf("%s", s);
  for (int l = strlen(s); l < width; l++)
    printf(" ");
}

// static: the user stack is only one page.
static struct pinfo procs[NPROC];

int
main(void)
{
  int n = getprocs(procs, NPROC);

  if (n < 0) {
    printf("ps: getprocs failed\n");
    exit(1);
  }

  printf("PID  PPID STATE     QUEUE WAIT  BURST CONF SIZE     NAME\n");
  printf("-----------------------------------------------------------\n");

  for (int i = 0; i < n; i++) {
    char *state;

    switch (procs[i].state) {
      case 1: state = "USED"; break;
      case 2: state = "SLEEPING"; break;
      case 3: state = "RUNNABLE"; break;
      case 4: state = "RUNNING"; break;
      case 5: state = "ZOMBIE"; break;
      default: state = "UNKNOWN"; break;
    }

    num(procs[i].pid, 5);
    num(procs[i].ppid, 5);
    str(state, 10);
    num(procs[i].queue, 6);
    num(procs[i].waiting, 6);
    num(procs[i].burst, 6);
    num(procs[i].confidence, 5);
    num((int)procs[i].sz, 9);
    printf("%s\n", procs[i].name);
  }

  exit(0);
}