#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "pinfo.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

#ifdef MLFQ
static struct spinlock rand_lock;
static uint32 rand_state = 2463534242U;
static int rr_last = 0; // index of the last process picked in queue 1
#endif

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  #ifdef MLFQ
    initlock(&rand_lock, "rand");
  #endif
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

//CFS standard weights table (nice = -20, -19, ...., 19)
static const int nice_weights[40] = {
  88761, 71755, 56483, 46273, 36291,
  29154, 23254, 18705, 14949, 11916,
  9548, 7620, 6100, 4904, 3906,
  3121, 2501, 1991, 1586, 1277,
  1024, 820, 655, 526, 423,
  335, 272, 215, 172, 137,
  110, 87, 70, 56, 45,
  36, 29, 23, 18, 15
};

//nice -> weight
static int
nice_to_weight(int nice)
{
  return nice_weights[nice + 20];
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Initialize MLFQ fields
  p->queue_level = 3;
  p->queue_enter_tick = ticks;
  p->waiting_ticks = 0;
  p->burst_time = 2;
  p->confidence = 50;
  p->quantum_ticks = 0;

  // Initialize CFS fields
  p->nice = 0;
  p->weight = 1024;
  p->vruntime = 0;
  p->cfs_ticks = 0;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  // Initialize init process in MLFQ queue 1
  p->queue_level = 1;
  p->queue_enter_tick = ticks;
  p->waiting_ticks = 0;
  p->quantum_ticks = 0;

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

#ifdef CFS
static uint64 min_vruntime(void);
#endif
// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  #ifdef CFS
  uint64 min = min_vruntime();
  #endif

  acquire(&np->lock);
  np->queue_enter_tick = ticks;
  #ifdef CFS
  if (np->vruntime < min)
    np->vruntime = min;
  #endif

  // init only forks sh, so children of init start in queue 1
  if (p == initproc)
    np->queue_level = 1;
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

int
setnice(int pid, int nice)
{
  struct proc *p;

  if (nice < -20 || nice > 19)
    return -1;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);

    if (p->pid == pid && p->state != UNUSED) {
      p->nice = nice;
      p->weight = nice_to_weight(nice);
      release(&p->lock);
      return 0;
    }

    release(&p->lock);
  }

  return -1;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent == p) {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // Found one.
          pid = pp->pid;
          if (addr != 0 &&
              copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          pp->parent = 0;
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

#ifdef CFS
static struct proc *
pick_cfs(void)
{
  struct proc *best = 0;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);

    if (p->state == RUNNABLE) {
      if (best == 0 || p->vruntime < best->vruntime ||
          (p->vruntime == best->vruntime && p->pid < best->pid)) {

        if (best != 0)
          release(&best->lock);

        best = p;
        continue;
      }
    }

    release(&p->lock);
  }

  return best;
}
#endif

#ifdef MLFQ
// LCG: x' = (a*x + c) mod 2^32, a = 1664525, c = 1013904223.
// c is odd and a-1 is divisible by 4, so by Hull-Dobell the period is the
// full 2^32. Low bits of a power-of-two LCG are weak, so use the high bits.
static int
rand100(void)
{
  int r;

  acquire(&rand_lock);
  rand_state = rand_state * 1664525U + 1013904223U;
  r = (rand_state >> 16) % 100;
  release(&rand_lock);
  return r;
}

// Each pick_*() returns with p->lock held, or 0 if its queue has no
// RUNNABLE process. Scanning is done without holding locks, so a candidate
// is re-validated after locking (another CPU may have taken it meanwhile).
static struct proc *
pick_rr(void)
{
  for (int k = 1; k <= NPROC; k++) {
    int i = (rr_last + k) % NPROC;
    struct proc *p = &proc[i];

    acquire(&p->lock);
    if (p->state == RUNNABLE && p->queue_level == 1) {
      rr_last = i;
      return p;
    }
    release(&p->lock);
  }
  return 0;
}

static struct proc *
pick_sjf(void)
{
  struct proc *cand[NPROC];
  int bt[NPROC], cf[NPROC];

  for (;;) {
    int n = 0;

    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE && p->queue_level == 2) {
        int j = n++;
        // stable insertion sort by burst_time
        while (j > 0 && bt[j - 1] > p->burst_time) {
          cand[j] = cand[j - 1];
          bt[j] = bt[j - 1];
          cf[j] = cf[j - 1];
          j--;
        }
        cand[j] = p;
        bt[j] = p->burst_time;
        cf[j] = p->confidence;
      }
      release(&p->lock);
    }
    if (n == 0)
      return 0;

    // The last candidate runs regardless of its confidence.
    int i;
    for (i = 0; i < n - 1; i++)
      if (rand100() <= cf[i])
        break;

    struct proc *p = cand[i];
    acquire(&p->lock);
    if (p->state == RUNNABLE && p->queue_level == 2)
      return p;
    release(&p->lock);
  }
}

static struct proc *
pick_fcfs(void)
{
  for (;;) {
    struct proc *best = 0;
    uint bt = 0;
    int bpid = 0;

    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE && p->queue_level == 3 &&
          (best == 0 || p->queue_enter_tick < bt ||
           (p->queue_enter_tick == bt && p->pid < bpid))) {
        best = p;
        bt = p->queue_enter_tick;
        bpid = p->pid;
      }
      release(&p->lock);
    }
    if (best == 0)
      return 0;

    acquire(&best->lock);
    if (best->state == RUNNABLE && best->queue_level == 3)
      return best;
    release(&best->lock);
  }
}
#endif

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();

  c->proc = 0;

#ifdef CFS
  for (;;) {
    intr_on();
    intr_off();

    struct proc *p = pick_cfs();
    if (p == 0) {
      asm volatile("wfi");
      continue;
    }

    p->cfs_ticks = 0;
    p->state = RUNNING;
    c->proc = p;
    swtch(&c->context, &p->context);

    mycpu()->intena = 0;
    c->proc = 0;
    release(&p->lock);
  }
#elif defined(MLFQ)
  for (;;) {
    intr_on();
    intr_off();

    struct proc *p = pick_rr();
    if (p == 0)
      p = pick_sjf();
    if (p == 0)
      p = pick_fcfs();

    if (p == 0) {
      asm volatile("wfi");
      continue;
    }

    p->state = RUNNING;
    p->waiting_ticks = 0;
    p->quantum_ticks = 0;
    c->proc = p;
    swtch(&c->context, &p->context);

    mycpu()->intena = 0;
    c->proc = 0;
    release(&p->lock);
  }
#else
  struct proc *p;

  for (;;) {
    intr_on();
    intr_off();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        mycpu()->intena = 0;
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0)
      asm volatile("wfi");
  }
#endif
}

// Called on a timer interrupt for the running process: should it yield?
// Q1: when its quantum expires. Q2: every tick, so the random SJF choice is
// re-run. Q3 (FCFS): never preempted by the clock.
int
should_yield(void)
{
#ifdef CFS
  struct proc *p = myproc();
  uint weight;
  uint64 total_weight = 0;
  uint slice;
  int r;

  // Read the current process weight
  acquire(&p->lock);
  weight = p->weight;
  release(&p->lock);

  // Calculate total weight of runnable and running processes
  for (struct proc *q = proc; q < &proc[NPROC]; q++) {
    acquire(&q->lock);
    if (q->state == RUNNABLE || q->state == RUNNING)
      total_weight += q->weight;

    release(&q->lock);
  }

  // Calculate time slice: L = 12, minimum granularity = 2
  slice = (12ULL * weight) / total_weight;
  if (slice < 2)
    slice = 2;
  acquire(&p->lock);
  p->cfs_ticks++;

  // Update virtual runtime
  p->vruntime += (1024ULL * 1000) / p->weight;
  r = (p->cfs_ticks >= slice);
  release(&p->lock);
  return r;

#elif defined(MLFQ)
  struct proc *p = myproc();
  int r = 0;
  int ended = 0;                       // TEMP

  acquire(&p->lock);
  if (p->queue_level == 1) {
    if (++p->quantum_ticks >= QUANTUM) {
      r = 1;
      ended = p->pid;                  // TEMP
    }
  } else if (p->queue_level == 2) {
    r = 1;
  }
  release(&p->lock);

  if (ended)                           // TEMP
    printk("[RR] pid %d ran %d ticks, now tick %d\n", ended, QUANTUM, (int)ticks);
  return r;
#else
  return 1;
#endif
}

// Aging. Called from clockintr on CPU 0 only, after tickslock is released
// (clockintr takes tickslock -> p->lock in wakeup, so never the reverse).
void
aging_tick(void)
{
#ifdef MLFQ
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNABLE) {
      p->waiting_ticks++;
      if (p->queue_level > 1 && p->waiting_ticks >= AGING_LIMIT) {
        p->queue_level--;
        p->waiting_ticks = 0;
        p->queue_enter_tick = ticks;
      }
    }
    release(&p->lock);
  }
#endif
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->queue_enter_tick = ticks;
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    first = 0;

    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}

//Find min vruntime
#ifdef CFS
static uint64
min_vruntime(void)
{
  uint64 min = ~0ULL;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);

    if ((p->state == RUNNABLE || p->state == RUNNING) &&
        p->vruntime < min) {
      min = p->vruntime;
    }

    release(&p->lock);
  }

  return min;
}
#endif

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
#ifdef CFS
    uint64 min = min_vruntime();
#endif

    acquire(&p->lock);
    if (p->chan == chan) {
      p->chan = 0;

      if (p->state == SLEEPING) {
#ifdef CFS
        if (p->vruntime < min)
          p->vruntime = min;
#endif
        p->state = RUNNABLE;
        p->queue_enter_tick = ticks;
      }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  if (pid == 0)
    return -1;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // Wake process from sleep().
        p->state = RUNNABLE;
        p->queue_enter_tick = ticks;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

#ifdef CFS
    printk("%d %s %s nice=%d vruntime=%ld", p->pid, state, p->name, p->nice, p->vruntime);
#else
    printk("%d %s %s", p->pid, state, p->name);
#endif
    printk("\n");
  }
}

int
getprocsinfo(uint64 buf, int max)
{
  struct pinfo pi;
  struct proc *p, *me = myproc();
  int count = 0;

  if (max < 0 || max > NPROC)
    return -1;

  for (p = proc; p < &proc[NPROC] && count < max; p++) {
    acquire(&wait_lock);
    acquire(&p->lock);

    if (p->state == UNUSED) {
      release(&p->lock);
      release(&wait_lock);
      continue;
    }

    pi.pid = p->pid;
    pi.ppid = (p->parent == 0) ? 0 : p->parent->pid;
    pi.state = p->state;
    pi.sz = p->sz;
    safestrcpy(pi.name, p->name, sizeof(pi.name));
    pi.queue = p->queue_level;
    pi.waiting = p->waiting_ticks;
    pi.burst = p->burst_time;
    pi.confidence = p->confidence;

    release(&p->lock);
    release(&wait_lock);

    // Copy one entry at a time to keep kernel stack usage small.
    if (copyout(me->pagetable, me->sz, buf + count * sizeof(pi),
                (char *)&pi, sizeof(pi)) < 0)
      return -1;
    count++;
  }

  return count;
}

int
ksetmlfq(int pid, int burst, int conf)
{
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state != UNUSED && p->pid == pid) {
      p->burst_time = burst;
      p->confidence = conf;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}
