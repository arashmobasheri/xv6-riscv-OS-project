#ifndef PINFO_H
#define PINFO_H

#include "kernel/types.h"

struct pinfo {
  int pid;
  int ppid;
  int state;
  uint64 sz;
  char name[16];
  int queue;       // MLFQ queue level (1..3)
  int waiting;     // ticks spent waiting while RUNNABLE
  int burst;       // suggested burst time (SJF)
  int confidence;  // confidence in burst time (0..100)
};

#endif
