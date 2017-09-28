/* 
 * mvee_infinite_loop:
 * this function is used for both thread transfering and signal delivery 
 * 
 * 1) to transfer threads to a new monitor, the original monitor (i.e. the 
 * monitor that monitors the childs that instigated the fork event) needs to
 * detach from the threads first. While the threads are detached, they can
 * run freely, without the intervention of a debugger.
 * As such, we have to move the program counter to an infinite loop while
 * the threads are detached. This way, the threads will all be in an equivalent
 * state when the new monitor attaches to them.
 * Because we're going to replace the registers by their original contents
 * when the new monitor attaches, we can use sys_pause calls in the infinite
 * loop.
 * 
 * 2) delivering signals through the ptrace API happens asynchronously 
 * (I found out the hard way). As such, we should wait for the threads to be
 * in equivalent states (e.g. stopped on the same syscall). Then the registers
 * should be backed up and the syscall nr should be replaced by a harmless
 * syscall that doesn't modify the program state. We use sys_getpid for this
 * purpose. When that replaced syscall returns, we change the pc to this
 * infinite loop while we wait for async signal delivery.
 * We probably cannot use syscalls while waiting for signal delivery. 
 * One possible exception is sys_sched_yield. Our modified MVEE kernel does
 * not report this syscall to the ptracer
 * 
 * the with_syscalls parameter is passed through the ecx register!
 */

		.globl mvee_infinite_loop
		.type mvee_infinite_loop,@function

mvee_infinite_loop:
		nop
		nop
		nop
		nop
		nop
		jmp mvee_infinite_loop
