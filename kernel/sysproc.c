#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
 sys_trace(void) {
    // printf("Inside sys Trace \n");
    int mask;
    argint(0, &mask);
    trace(mask);
    return 0;
 }

uint64
sys_waitx(void)
{
  uint64 addr, addr1, addr2;
  uint waitTime, cpuRuntime;
  // if(argaddr(0, &addr) < 0)
  //   return -1;
  // if(argaddr(1, &addr1) < 0) 
  //   return -1;
  // if(argaddr(2, &addr2) < 0)
  //   return -1;
  argaddr(0, &addr);
  argaddr(1, &addr1);
  argaddr(2, &addr2);

  int retValue = waitx(addr, &cpuRuntime, &waitTime);
  struct proc* p = myproc();
  if (copyout(p->pagetable, addr1,(char*)&waitTime, sizeof(int)) < 0)
    return -1;
  if (copyout(p->pagetable, addr2,(char*)&cpuRuntime, sizeof(int)) < 0)
    return -1;
  return retValue;
}

uint64
sys_set_priority(void)
{
  int priority, pid;
  // if (argint(0, &priority) < 0)
  //   return -1;
  // if (argint(1, &pid) < 0)
  //   return -1;
  argint(0, &priority);
  argint(1, &pid);
  return set_priority(priority, pid);
}