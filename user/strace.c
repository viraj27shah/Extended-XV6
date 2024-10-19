#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
 
int 
main(int argc, char *argv[]) {
    if(argc > 1)
    {
        uint64 mask = atoi(argv[1]);
        // trace(32);
        // return 0;
        if(mask >= 0)
        {
            if(mask > 0)
            {
                if(trace(mask)<0)
                {
                    printf("hahaha\n");
                    printf("execution failed\n");
                    exit(0);
                }
                // trace(mask);
            }
            if(argc>2)
            {
                exec(argv[2],argv+2);
            }
        }
        else
        {
            printf("excution failed - invalid mask\n");
        }
    }
    else
    {
        printf("excution failed - insufficient arguments\n");
    }
    exit(0);
 }