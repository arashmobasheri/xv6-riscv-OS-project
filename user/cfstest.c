#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int pid;
  int p[2];
  int nice_values[] = {-10, -4, 0, 5, 10};

  if (pipe(p) < 0) {
    printf("pipe failed\n");
    exit(1);
  }

  printf("CFS scheduler test started\n");

  for (int i = 0; i < 5; i++) {
    pid = fork();

    if (pid < 0) {
      printf("fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      int start;

      close(p[1]);

      if (read(p[0], &start, sizeof(start)) != sizeof(start))
        exit(1);

      close(p[0]);

      int mypid = getpid();
      setnice(mypid, nice_values[i]);

      volatile uint64 count = 0;

     

	while (uptime() < start + 100) {
	  for (int j = 0; j < 1000; j++)
	    count++;
	}

      printf("PID %d, nice %d, count %ld\n",
             mypid, nice_values[i], count);

      exit(0);
    }
  }

  close(p[0]);

  int start = uptime() + 20;

  for (int i = 0; i < 5; i++)
    write(p[1], &start, sizeof(start));

  close(p[1]);

  for (int i = 0; i < 5; i++)
    wait(0);

  printf("CFS scheduler test finished\n");
  exit(0);
}
