#include "kernel/types.h"
#include "kernel/pinfo.h"
#include "user/user.h"
#include "kernel/param.h"

int
main(void)
{
  struct pinfo procs[NPROC];
  int n;

  n = getprocs(procs, NPROC);

  if (n < 0) {
    printf("ps: getprocs failed\n");
    exit(1);
  }

  printf("PID    PPID   STATE       SIZE       NAME\n");
  printf("----------------------------------------------------\n");

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

    printf("%d      %d      ", procs[i].pid, procs[i].ppid);

    printf("%s", state);

    int len = strlen(state);
    for (int j = len; j < 12; j++)
        printf(" ");

    printf("%d      %s\n",
          (int)procs[i].sz,
          procs[i].name);
  }

  exit(0);
}