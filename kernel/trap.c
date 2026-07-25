#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  // } else if((which_dev = devintr()) != 0){
  //   // ok
  // } else {
  //   printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
  //   printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
  //   p->killed = 1;
  // }
  } else if(r_scause() == 13 || r_scause() == 15){
    /*
     * scause == 13：Load page fault
     * scause == 15：Store/AMO page fault
     *
     * 这里只处理用户读写 lazy 页面产生的缺页异常。
     * 指令缺页 scause == 12 不在这里处理。
     */

    /*
     * stval 保存发生缺页的具体用户虚拟地址。
     */
    uint64 faultva = r_stval();

    /*
     * 内存分配和映射必须以页面为单位。
     *
     * 例如：
     *   faultva = 0x4008
     *   va      = 0x4000
     */
    uint64 va = PGROUNDDOWN(faultva);

    /*
     * 情况一：
     * 故障地址已经超过通过 sbrk() 获得的地址空间。
     *
     * 这种地址不是合法 lazy 页面，应该终止进程。
     *
     * 情况二：
     * 故障地址位于当前用户栈页面下方。
     *
     * 这可能是在访问栈保护页，同样不能为它分配页面。
     */
    if(faultva >= p->sz ||
       faultva < PGROUNDDOWN(p->trapframe->sp)){
      p->killed = 1;
    } else {
      /*
       * 查询该地址是否已经存在有效的页表项。
       *
       * 如果已经有有效 PTE，却仍然发生 page fault，
       * 说明这更可能是页面权限问题，而不是 lazy 页面尚未分配。
       *
       * 例如栈保护页可能拥有有效 PTE，
       * 但没有 PTE_U 用户访问权限。
       */
      pte_t *pte = walk(p->pagetable, va, 0);

      if(pte != 0 && (*pte & PTE_V)){
        /*
         * 已经存在有效映射，不能再次 mappages()，
         * 否则会触发 panic: remap。
         */
        p->killed = 1;
      } else {
        /*
         * 这是一个合法但尚未映射的 lazy 页面。
         * 从空闲物理页链表中分配一页。
         */
        char *mem = kalloc();

        if(mem == 0){
          /*
           * 物理内存耗尽。
           * 只终止当前用户进程，不让内核 panic。
           */
          p->killed = 1;
        } else {
          /*
           * 新分配的用户页面必须清零。
           */
          memset(mem, 0, PGSIZE);

          /*
           * 建立虚拟地址 va 到物理页面 mem 的映射。
           *
           * 权限：
           *   PTE_R：可读
           *   PTE_W：可写
           *   PTE_U：用户态可访问
           *
           * 不添加 PTE_X，因为 sbrk() 扩展的是数据页面，
           * 不是程序代码页面。
           */
          if(mappages(p->pagetable,
                      va,
                      PGSIZE,
                      (uint64)mem,
                      PTE_R | PTE_W | PTE_U) != 0){
            /*
             * 映射失败时，释放刚分配的物理页，
             * 避免内存泄漏。
             */
            kfree(mem);
            p->killed = 1;
          }
        }
      }
    }
  } else if((which_dev = devintr()) != 0){
    // ok
} else {
  printf("usertrap(): unexpected scause %p pid=%d\n",
         r_scause(), p->pid);
  printf("            sepc=%p stval=%p\n",
         r_sepc(), r_stval());
  p->killed = 1;
}

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

