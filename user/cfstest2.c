#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NCHILD 4
#define RUN_TICKS 100

struct test_window {
  int start;
  int end;
};

struct test_result {
  int pid;
  int nice;
  int setnice_rc;
  uint64 count;
};

int
main(void)
{
  int pid;
  int ready[2];
  int go[2];
  int results[2];

  int nice_values[NCHILD] = {-10, -4, 0, 5};

  char token = 'R';

  if (pipe(ready) < 0 ||
      pipe(go) < 0 ||
      pipe(results) < 0) {
    printf("pipe failed\n");
    exit(1);
  }

  printf("CFS scheduler test started\n");

  // Create four child processes.
  for (int i = 0; i < NCHILD; i++) {
    pid = fork();

    if (pid < 0) {
      printf("fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      struct test_window window;
      struct test_result result;

      close(ready[0]);
      close(go[1]);
      close(results[0]);

      result.pid = getpid();
      result.nice = nice_values[i];

      // Set this child's nice value.
      result.setnice_rc =
          setnice(result.pid, result.nice);

      // Tell the parent that this child is ready.
      if (write(ready[1], &token, 1) != 1)
        exit(1);

      close(ready[1]);

      // Wait for the common start/end times.
      if (read(go[0], &window,
               sizeof(window)) != (int)sizeof(window))
        exit(1);

      close(go[0]);

      // Wait until the common start time.
      while (uptime() < window.start)
        ;

      // CPU-bound workload.
      volatile uint64 count = 0;

      while (uptime() < window.end) {
        for (int j = 0; j < 1000; j++)
          count++;
      }

      result.count = count;

      // Send result to parent instead of printing here.
      if (write(results[1], &result,
                sizeof(result)) != (int)sizeof(result))
        exit(1);

      close(results[1]);
      exit(0);
    }
  }

  // Parent keeps only these pipe ends.
  close(ready[1]);
  close(go[0]);
  close(results[1]);

  // Wait until all four children have set their nice values.
  for (int i = 0; i < NCHILD; i++) {
    if (read(ready[0], &token, 1) != 1) {
      printf("ready pipe error\n");
      exit(1);
    }
  }

  close(ready[0]);

  // One common time window for all children.
  struct test_window window;

  window.start = uptime() + 10;
  window.end = window.start + RUN_TICKS;

  // Send the same time window to all four children.
  for (int i = 0; i < NCHILD; i++) {
    if (write(go[1], &window,
              sizeof(window)) != (int)sizeof(window)) {
      printf("start pipe error\n");
      exit(1);
    }
  }

  close(go[1]);

  // Wait for all children to finish.
  for (int i = 0; i < NCHILD; i++)
    wait(0);

  // Print results from the parent only.
  printf("\n--- CFS Results ---\n");

  for (int i = 0; i < NCHILD; i++) {
    struct test_result result;

    if (read(results[0], &result,
             sizeof(result)) != (int)sizeof(result)) {
      printf("result pipe error\n");
      exit(1);
    }

    printf("PID %d, nice %d, setnice %d, count %ld\n",
           result.pid,
           result.nice,
           result.setnice_rc,
           result.count);
  }

  close(results[0]);

  printf("CFS scheduler test finished\n");

  exit(0);
}
