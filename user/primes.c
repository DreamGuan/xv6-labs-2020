#include "kernel/types.h"
#include "user/user.h"

/*
 * sieve 函数表示一个“筛子进程”的工作逻辑。
 *
 * 参数 left_fd：
 *   当前进程从左边管道读取数字。
 *
 * 每一个 sieve 进程都会：
 *   1. 从 left_fd 读取第一个数字；
 *   2. 把这个数字当作当前素数 prime；
 *   3. 打印 prime；
 *   4. 创建一个右边管道；
 *   5. fork 出下一个筛子进程；
 *   6. 当前进程负责过滤数字，把不能被 prime 整除的数写到右边管道。
 */
void sieve(int left_fd)
{
    int prime;
    int num;

    /*
    从左边管道读取第一个整数。
   
    read 的返回值表示实际读到的字节数。
    如果返回 0，说明管道的写端已经全部关闭，并且没有数据可读。
   
    当前实验中，每次传递的是 int，
    所以正常情况下应该读到 sizeof(int) 个字节。
   */

   if(read(left_fd,&prime,sizeof(int)) == 0)
   {
    /*
    如果没有读到任何数字，说明当前管道已经结束。
    当前进程不需要继续创建新的筛子进程，直接退出。
    */
    close(left_fd);
    exit(0);
   }
   
   /*
   * 能读到的第一个数字一定是素数。
   *
   * 原因：
   *   它已经经过左边所有筛子进程的过滤，
   *   不可能再是前面那些 prime 的倍数。
   */
   printf("prime %d\n",prime);

  /*
   * 创建一个新的管道，用来连接当前筛子进程和右边的下一个筛子进程。
   *
   * right_fd[0] 是读端；
   * right_fd[1] 是写端。
   *
   * 当前进程会向 right_fd[1] 写入过滤后的数字；
   * 子进程会从 right_fd[0] 读取这些数字。
   */   
 
   int right_fd[2];

   if(pipe(right_fd) < 0)
   {
    fprintf(2,"pipe failed\n");
    close(left_fd);
    exit(1);
   }

  /*
   * fork 创建右边的下一个筛子进程。
   *
   * fork 返回值：
   *   pid < 0：创建失败
   *   pid == 0：当前在子进程中
   *   pid > 0：当前在父进程中
   */
   int pid = fork();
   if(pid < 0)
   {
    fprintf(2,"fork failed\n");
    close(left_fd);
    close(right_fd[0]);
    close(right_fd[1]);
    exit(1);
   }

   if(pid == 0)
   {
    /*
     * 子进程逻辑：
     *
     * 子进程是“右边的下一个筛子”。
     * 它只需要从 right_fd[0] 读取数据。
     *
     * 因此：
     *   1. right_fd[1] 是写端，子进程不用，关闭；
     *   2. left_fd 是父进程当前使用的左边管道，子进程不用，关闭；
     *   3. 调用 sieve(right_fd[0])，继续处理右边管道中的数字。
     */
    close(left_fd);
    close(right_fd[1]);

    sieve(right_fd[0]);

    exit(0);
   }else{
     /*
     * 父进程逻辑：
     *
     * 当前父进程负责使用 prime 过滤左边管道传来的数字。
     *
     * 它会：
     *   1. 从 left_fd 不断读取 num；
     *   2. 如果 num 不能被 prime 整除，就写入 right_fd[1]；
     *   3. 如果 num 能被 prime 整除，就丢弃。
     */
    close(right_fd[0]);

    while(read(left_fd,&num,sizeof(int)) == sizeof(int)){
        if(num % prime !=0)
        {
            write(right_fd[1],&num,sizeof(int));
        }
    }

    close(left_fd);
    close(right_fd[1]);

    wait(0);

    exit(0);
   }
}

int main(int argc,char *argv[])
{
    int fd[2];

    if(pipe(fd) < 0)
    {
        fprintf(2,"pipe failed\n");
        exit(1);
    }

    int pip = fork();

    if(pip < 0)
    {
        fprintf(2,"fork failed\n");
        close(fd[0]);
        close(fd[1]);
        exit(1);
    }

    if(pip == 0)
    {
        close(fd[1]);
        sieve(fd[0]);
        exit(0);
    }else{
        close(fd[0]);
        
        for(int i=2;i<=35;i++)
        {
            write(fd[1],&i,sizeof(int));
        }

        close(fd[1]);

        wait(0);
        exit(0);
    }
}