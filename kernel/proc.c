#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

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
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
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
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
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
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
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

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  p->cpuRunTime = 0;          // Run time inside cpu initialization
  p->endTime = 0;             // Process end time initalization
  p->creationTime = ticks;    // Process creation time initialization
  p->traceMask = 0;           // Initialize trace mask with 0
  p->noOfTimesGotCpu = 0;     // No of time process comes inside cpu initialization

  #ifdef PBS
    p->staticPriority = DEFAULT_STATIC_PRIORITY;          // Set default static priority
    p->sleepStartTime = 0;                                // Initialize start sleep time
    p->sleepTime = 0;                                     // Initialize sleep time
  #endif

  #ifdef MLFQ
    p->entryTimeInCurrentQ = ticks;                       // Entry time initialization in a queue
    p->currentQ = 0;                                      // For the first time every process is placed in 0th queue
    for (int i = 0; i < 5; i++)   
      p->qTicks[i] = 0;                                   // Time passed in every queue is 0 at start
  #endif

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
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
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
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

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

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
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  //Copy the parent traceMask into child tracMask
  np->traceMask = p->traceMask;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
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

  p->endTime = ticks;               // Define end time when process exits

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// // Per-CPU process scheduler.
// // Each CPU calls scheduler() after setting itself up.
// // Scheduler never returns.  It loops, doing:
// //  - choose a process to run.
// //  - swtch to start running that process.
// //  - eventually that process transfers control
// //    via swtch back to the scheduler.
// void
// scheduler(void)
// {
//   struct proc *p;
//   struct cpu *c = mycpu();
  
//   c->proc = 0;
//   for(;;){
//     // Avoid deadlock by ensuring that devices can interrupt.
//     intr_on();

//     for(p = proc; p < &proc[NPROC]; p++) {
//       acquire(&p->lock);
//       if(p->state == RUNNABLE) {
//         // Switch to chosen process.  It is the process's job
//         // to release its lock and then reacquire it
//         // before jumping back to us.
//         p->state = RUNNING;
//         c->proc = p;
//         swtch(&c->context, &p->context);

//         // Process is done running for now.
//         // It should have changed its p->state before coming back.
//         c->proc = 0;
//       }
//       release(&p->lock);
//     }
//   }
// }

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
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;

  #ifdef DEFAULT
  // printf("Inside default\n");
  for(;;){
      // Avoid deadlock by ensuring that devices can interrupt.
      intr_on();

      for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          // printf("Process selected\n");
          p->noOfTimesGotCpu++;
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context);

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
        }
        release(&p->lock);
      }
    }
  #endif

  // If user input is fcfs then scheduler behave as firstcome first serve
  #ifdef FCFS
  // printf("Inside fcfs\n");
    for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    struct proc *selectedProcess = 0;

    //selecting process which have minimum creation time
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p->state == RUNNABLE)
      {
        if(selectedProcess == 0)
        {
          selectedProcess = p;
        }
        else if(selectedProcess->creationTime > p->creationTime)
        {
          selectedProcess = p;
        }
      }
    }

    //Run selected process
    if(selectedProcess != 0)
    {
      acquire(&selectedProcess->lock);
      if(selectedProcess->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        selectedProcess->state = RUNNING;
        
        c->proc = selectedProcess;
        selectedProcess->noOfTimesGotCpu++;

        swtch(&c->context, &selectedProcess->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&selectedProcess->lock);
    }
  }
  #endif

  // Non - preemptive priority base scheduler
  #ifdef PBS
  // printf("Inside pbs\n");
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    struct proc *choosenProcess = 0;
    int min_dynamic_priority = 101;

    // Find the highest priority (least DP value) process.
    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->state == RUNNABLE) {
        // Calculating niceness.
        int niceness = 5;
        if (p->cpuRunTime + p->sleepTime != 0)
          niceness = (int)((p->sleepTime / (p->cpuRunTime + p->sleepTime)) * 10);

        // Calculating dynamic priority.
        int value = (p->staticPriority - niceness + 5 < 100 ? p->staticPriority - niceness + 5 : 100);
        int dp = (0 < value ? value : 0);

        // Selecting process to run.
        if (choosenProcess == 0) {
          min_dynamic_priority = dp;
          choosenProcess = p;
        }
        else if (dp < min_dynamic_priority) {
          min_dynamic_priority = dp;
          choosenProcess = p;
        }
        else if (dp == min_dynamic_priority) {
          if (choosenProcess->noOfTimesGotCpu > p->noOfTimesGotCpu) {
            choosenProcess = p;
          }
          else if (choosenProcess->noOfTimesGotCpu == p->noOfTimesGotCpu) {
            if (choosenProcess->creationTime > p->creationTime) {
              choosenProcess = p;
            }
          }
        }
      }
    }

    // Schedule the selected highest-priority process.
    if (choosenProcess != 0) {
      acquire(&choosenProcess->lock);
      if (choosenProcess->state == RUNNABLE) {
        choosenProcess->noOfTimesGotCpu++;

        // Running the process.
        choosenProcess->state = RUNNING;
        c->proc = choosenProcess;
        swtch(&c->context, &choosenProcess->context);

        // Process is done running.
        c->proc = 0;
      }
      release(&choosenProcess->lock);
    }
  }

  #endif

  //Multilevel feedback queue scheduler
  #ifdef MLFQ
  // printf("Inside MLFQ\n");
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    struct proc *choosenProcess = 0;
    int highest_queue = 5;

    // Aging the processes
    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->state == RUNNABLE) {
        if ((ticks - p->entryTimeInCurrentQ > WAITING_LIMIT) && p->currentQ > 0) {
          acquire(&p->lock);
          p->qTicks[p->currentQ] += (ticks - p->entryTimeInCurrentQ);
          p->currentQ--;
          p->entryTimeInCurrentQ = ticks;
          release(&p->lock);
        }
      }
    }

    // Selecting the process to be scheduled
    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->state == RUNNABLE) {
        if (choosenProcess == 0) {
          choosenProcess = p;
          highest_queue = choosenProcess->currentQ;
        }
        else if (p->currentQ < highest_queue) {
          choosenProcess = p;
          highest_queue = choosenProcess->currentQ;
        }
        else if (p->currentQ == highest_queue && p->entryTimeInCurrentQ < choosenProcess->entryTimeInCurrentQ) {
          choosenProcess = p;
        }
      }
    }

    // Schedule the chosen process
    if (choosenProcess != 0) {
      acquire(&choosenProcess->lock);
      if (choosenProcess->state == RUNNABLE) {
        choosenProcess->noOfTimesGotCpu++;
        choosenProcess->entryTimeInCurrentQ = ticks;

        // Running the process.
        choosenProcess->state = RUNNING;
        c->proc = choosenProcess;
        swtch(&c->context, &choosenProcess->context);

        // Process is done running.
        c->proc = 0;
        choosenProcess->qTicks[choosenProcess->currentQ] += (ticks - choosenProcess->entryTimeInCurrentQ);
      }
      release(&choosenProcess->lock);
    }
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

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
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
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  #ifdef PBS
    // printf("goes inside sleep funnction when pbs\n");
    p->sleepStartTime = ticks;                    // Process is going for sleep, and noting sleep start time
  #endif

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        #ifdef PBS
          // printf("goes inside wakeup funnction when pbs\n");
          p->sleepTime = ticks - p->sleepStartTime;         // Process is comming out of sleep to runnable, calculating total sleep time
        #endif
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
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
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
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
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// // Print a process listing to console.  For debugging.
// // Runs when user types ^P on console.
// // No lock to avoid wedging a stuck machine further.
// void
// procdump(void)
// {
//   static char *states[] = {
//   [UNUSED]    "unused",
//   [USED]      "used",
//   [SLEEPING]  "sleep ",
//   [RUNNABLE]  "runble",
//   [RUNNING]   "run   ",
//   [ZOMBIE]    "zombie"
//   };
//   struct proc *p;
//   char *state;

//   printf("\n");
//   for(p = proc; p < &proc[NPROC]; p++){
//     if(p->state == UNUSED)
//       continue;
//     if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
//       state = states[p->state];
//     else
//       state = "???";
//     printf("%d %s %s", p->pid, state, p->name);
//     printf("\n");
//   }
// }

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  // static char *states[] = {
  // [UNUSED]    "unused",
  // [SLEEPING]  "sleep ",
  // [RUNNABLE]  "runable",
  // [RUNNING]   "run   ",
  // [ZOMBIE]    "zombie"
  // };
  // struct proc *p;
  // char *state;

  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  #if defined(DEFAULT) || defined(FCFS)
    printf("\nPID\tState\trtime\twtime\tnrun");
  #endif

  #ifdef PBS
    printf("\nPID\tPrio\tState\trtime\twtime\tnrun");
  #endif

  #ifdef MLFQ
    printf("\nPID\tPrio\tState\trtime\twtime\tnrun\tq0\tq1\tq2\tq3\tq4");
  #endif

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    
    #if defined(DEFAULT) || defined(FCFS)
      int end_time = p->endTime;
      if (end_time == 0)
        end_time = ticks;

      printf("%d\t%s\t%d\t%d\t%d", p->pid, state, p->cpuRunTime, end_time - p->creationTime - p->cpuRunTime, p->noOfTimesGotCpu);
      printf("\n");
    #endif

    #ifdef PBS
      int end_time = p->endTime;
      if (end_time == 0)
        end_time = ticks;

      int niceness = 5;
      if (p->cpuRunTime + p->sleepTime != 0)
        niceness = (int)((p->sleepTime / (p->cpuRunTime + p->sleepTime)) * 10);
      int value = (p->staticPriority - niceness + 5 < 100 ? p->staticPriority - niceness + 5 : 100);
      int dp = (0 < value ? value : 0);

      printf("%d\t%d\t%s\t%d\t%d\t%d", p->pid, dp, state, p->cpuRunTime, end_time - p->creationTime - p->cpuRunTime, p->noOfTimesGotCpu);
      printf("\n");
    #endif

    #ifdef MLFQ
      int end_time = p->endTime;
      if (end_time == 0)
        end_time = ticks;

      int current_queue = p->currentQ;
      if (p->state == ZOMBIE)
        current_queue = -1;
      printf("%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d", p->pid, current_queue, state, p->cpuRunTime, end_time - p->creationTime - p->cpuRunTime, p->noOfTimesGotCpu, p->qTicks[0], p->qTicks[1], p->qTicks[2], p->qTicks[3], p->qTicks[4]);
      printf("\n");
    #endif
  }
}

// Copy the user trace mask into process info
void trace(uint64 mask)
{
  // printf("inside trace\n"); 
  struct proc *p = myproc();
  acquire(&p->lock);
  p->traceMask = mask;
  release(&p->lock);
}

// Add one tick time to all running processes
void
updateTime(void)
{
  struct proc* p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->cpuRunTime++;
    }
    release(&p->lock); 
  }
}

// Same as wait function
// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
waitx(uint64 addr, uint* cpuRunTime, uint* waitTime)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          
          *cpuRunTime = pp->cpuRunTime;
          *waitTime = pp->endTime - pp->creationTime - pp->cpuRunTime;

          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

int set_priority(uint64 priority, uint64 pid)
{
  struct proc *p;
  int old_sp = -1;

  for (p = proc; p < &proc[NPROC]; p++) {
    #ifdef PBS
      if (p->pid == pid) {
        old_sp = p->staticPriority;
        p->staticPriority = priority;

        // Old dynamic priority.
        int niceness = 5;
        if (p->cpuRunTime + p->sleepTime != 0)
          niceness = (int)((p->sleepTime / (p->cpuRunTime + p->sleepTime)) * 10);

        int value = (old_sp - niceness + 5 < 100 ? old_sp - niceness + 5 : 100);
        int dp_old = (0 < value ? value : 0);

        p->cpuRunTime = 0;
        p->sleepTime = 0;

        // New dynamic priority.
        value = (priority < 100 ? priority : 100);
        int dp_new = (0 < value ? value : 0);

        if (dp_old > dp_new)
          yield();

        break;
      }
    #endif
  }

  return old_sp;
}