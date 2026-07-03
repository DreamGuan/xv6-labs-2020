#include "kernel/types.h"
#include "user/user.h"

int main(int argc,char *argv[])
{
    //定义两个管道
    int p2c[2];
    int c2p[2];
    //buf用来保存管道中的第一个字节，具体内容并不关心
    char buf = 'x';
    //创建并判断管道（2个）
    if(pipe(p2c) < 0)
    {
        fprintf(2,"pipe p2c failed\n");
        exit(1);
    }
    if(pipe(c2p) < 0)
    {
        fprintf(2,"pipe c2p failed\n");
        exit(1);
    }

    //创建子进程
    int pip = fork();
    if(pip < 0) //返回值小于0创建失败
    {
        fprintf(2,"fork failed\n");
        exit(1);
    }

    if(pip == 0)//返回值等于0 子进程正在执行
    {
        //子进程只读p2c，只写c2p
        close(p2c[1]);
        close(c2p[0]);

        read(p2c[0],&buf,1);

        //getpid()返回当前进程的pid。
        printf("%d: received ping\n",getpid());

        write(c2p[1],&buf,1);

        close(p2c[0]);
        close(c2p[1]);

        exit(0);
    } else { //返回值大于0 当前正在父进程中执行，返回值是子进程pid
        //父进程只读c2p，只写p2c
        close(p2c[0]);
        close(c2p[1]);

        write(p2c[1],&buf,1);
        read(c2p[0],&buf,1);

        printf("%d: received pong\n",getpid());

        close(p2c[1]);
        close(c2p[0]);

        wait(0);

        exit(0);
    }
}