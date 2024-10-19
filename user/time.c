#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int 
main(int argc, char ** argv) 
{
  int pid = fork();
  if(pid < 0) {
    printf("fork(): failed\n");
    exit(1);
  } else if(pid == 0) {
    if(argc == 1) {
      sleep(10);
      exit(0);
    } else {
      exec(argv[1], argv + 1);
      printf("exec(): failed\n");
      exit(1);
    }  
  } else {
    int cpuRtime, waitTime;
    waitx(0, &waitTime, &cpuRtime);
    printf("\nwaiting:%d\nrunning:%d\n", waitTime, cpuRtime);
  }
  exit(0);
}