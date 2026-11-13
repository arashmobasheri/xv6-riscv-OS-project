
#include "kernel/types.h"
#include "kernel/pinfo.h"
#include "user/user.h"
#include "kernel/param.h"

int main(void)
{
  for(int i = 0; i < 4; i++){
    if(fork() == 0){
      pause(100);
      exit(0);
    }
  }

  if(fork() == 0){
    char *argv[] = {"ps", 0};
    exec("ps", argv);
    exit(1);
  }

  wait(0);
  wait(0);
  wait(0);
  wait(0);
  wait(0);

  exit(0);
}