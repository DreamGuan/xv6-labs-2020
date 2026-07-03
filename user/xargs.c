#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int 
is_blank(char c)
{
    return c == ' ' || c == '\t';
}

void
run(int base_argc,char *base_argv[],char *line)
{
    char *argv[MAXARG];
    int argc = 0;
    for(int i = 0;i < base_argc;i++)
    {
        if(argc >= MAXARG -1)
        {
            fprintf(2,"xargs: too many arguments\n");
            return;
        }
        argv[argc++] = base_argv[i];
    }

    char *p = line;

    while(*p)
    {
        while(*p && is_blank(*p)) p++;

        if(*p == 0) break;

        if(argc >= MAXARG-1)
        {
            fprintf(2,"xargs:too many arguments\n");
            return;
        }
        argv[argc++] = p;

        while(*p && !is_blank(*p)) p++;

        if(*p)
        {
            *p = 0;
            p++;
        }
    }
    argv[argc] = 0;
    
    if(argv[0] == 0) return;

    int pid = fork();

    if(pid < 0)
    {
        fprintf(2,"xargs:fork failed\n");
        return;
    }

    if(pid == 0)
    {
        exec(argv[0],argv);
    /*
     * 如果 exec 成功，它不会返回。
     * 如果执行到这里，说明 exec 失败。
     */
        fprintf(2,"xargs:exec %s failed\n",argv[0]);
        exit(1);
    }else{
        wait(0);
    }
}

int
main(int argc,char *argv[])
{
    if(argc < 2)
    {
        fprintf(2,"usage:xargs command [args...]\n");
        exit(1);
    }

    char *base_argv[MAXARG];
    int base_argc = 0;

    for(int i=1;i < argc;i++)
    {
        if(base_argc >= MAXARG-1)
        {
            fprintf(2,"xargs:too many arguments\n");
            exit(1);
        }
        base_argv[base_argc++] = argv[i];
    }

    char line[512];
    int idx = 0;
    char c;

    while(read(0,&c,1) == 1)
    {
        if(c == '\n')
        {
            line[idx] = 0;

            if(idx > 0)
            {
                run(base_argc,base_argv,line);
            }

            idx = 0;
        } else {
            if(idx < sizeof(line) - 1)
            {
                line[idx++] = c;
            } else {
                fprintf(2,"xargs:line too long\n");
                exit(1);
            }
        }
    }

    if(idx > 0){
        line[idx] = 0;
        run(base_argc,base_argv,line);
    }

    exit(0);
}