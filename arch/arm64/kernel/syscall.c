/* syscall.c COPYRIGHT FUJITSU LIMITED 2015-2016 */
#include <ihk/cpu.h>
#include <ihk/debug.h>
#include <cls.h>
#include <cpulocal.h>
#include <syscall.h>
#include <process.h>
#include <string.h>
#include <errno.h>
#include <kmalloc.h>
#include <uio.h>
#include <ptrace.h>
#include <vdso.h>
#include <mman.h>
#include <shm.h>
#include <elfcore.h>
#include <hw_breakpoint.h>
#include <debug-monitors.h>
#include <irq.h>

extern void clear_single_step(struct thread *thread);
void terminate(int, int);
extern long do_sigaction(int sig, struct k_sigaction *act, struct k_sigaction *oact);
long syscall(int num, ihk_mc_user_context_t *ctx);
extern unsigned long do_fork(int, unsigned long, unsigned long, unsigned long,
	unsigned long, unsigned long, unsigned long);

//#define DEBUG_PRINT_SC

#ifdef DEBUG_PRINT_SC
#define dkprintf kprintf
#define ekprintf(...) do { if (0) kprintf(__VA_ARGS__); } while (0)
#else
#define dkprintf(...) do { if (0) kprintf(__VA_ARGS__); } while (0)
#define ekprintf(...) do { if (0) kprintf(__VA_ARGS__); } while (0)
#endif

#define NOT_IMPLEMENTED()  do { kprintf("%s is not implemented\n", __func__); while(1);} while(0)

uintptr_t debug_constants[] = {
	sizeof(struct cpu_local_var),
	offsetof(struct cpu_local_var, current),
	offsetof(struct cpu_local_var, runq),
	offsetof(struct cpu_local_var, status),
	offsetof(struct cpu_local_var, idle),
	offsetof(struct thread, ctx) + offsetof(struct thread_info, cpu_context),
	offsetof(struct thread, sched_list),
	offsetof(struct thread, proc),
	offsetof(struct thread, status),
	offsetof(struct process, pid),
	offsetof(struct thread, tid),
	-1,
};

static ihk_spinlock_t cpuid_head_lock = SPIN_LOCK_UNLOCKED;
static int cpuid_head = 1;

/* archtecture-depended syscall handlers */
int obtain_clone_cpuid(void) {
	struct ihk_mc_cpu_info *cpu_info = ihk_mc_get_cpu_info();
	int cpuid = -1;
	int i = 0;

	/* cpu_head lock */
	ihk_mc_spinlock_lock_noirq(&cpuid_head_lock);
	while(i < cpu_info->ncpus) {
		int found = 0;

		/* cpuid_head over cpu_info->ncpus, cpuid_head = BSP reset. */
		if (cpuid_head >= cpu_info->ncpus) {
			cpuid_head = 0;
		}

		/* check cpu status */
		if (get_cpu_local_var(cpuid_head)->status == CPU_STATUS_IDLE) {
			get_cpu_local_var(cpuid_head)->status = CPU_STATUS_RESERVED;
			cpuid = cpuid_head;
			found = 1;
		}
		i++;
		cpuid_head++;

		if (found) {
			break;
		}
	}

	/* cpu_head unlock */
	ihk_mc_spinlock_unlock_noirq(&cpuid_head_lock);

	return cpuid;
}

int
arch_clear_host_user_space()
{
	struct thread *th = cpu_local_var(current);

	/* XXX: might be unnecessary */
	clear_host_pte(th->vm->region.user_start,
			(th->vm->region.user_end - th->vm->region.user_start));
	return 0;
}

/* archtecture-depended syscall handlers */
extern unsigned long do_fork(int clone_flags, unsigned long newsp,
			     unsigned long parent_tidptr, unsigned long child_tidptr,
			     unsigned long tlsblock_base, unsigned long curpc,
			     unsigned long cursp);

SYSCALL_DECLARE(arm64_clone)
{
	if ((int)ihk_mc_syscall_arg0(ctx) & CLONE_VFORK) {
		return do_fork(CLONE_VFORK|SIGCHLD, 0, 0, 0, 0, ihk_mc_syscall_pc(ctx), ihk_mc_syscall_sp(ctx));
	} else {
		return do_fork((int)ihk_mc_syscall_arg0(ctx),	/* clone_flags */
			       ihk_mc_syscall_arg1(ctx),	/* newsp */
			       ihk_mc_syscall_arg2(ctx),	/* parent_tidptr */
			       ihk_mc_syscall_arg4(ctx),	/* child_tidptr (swap arg3) */
			       ihk_mc_syscall_arg3(ctx),	/* tlsblock_base (swap arg4) */
			       ihk_mc_syscall_pc(ctx),		/* curpc */
			       ihk_mc_syscall_sp(ctx));		/* cursp */
	}
}

SYSCALL_DECLARE(rt_sigaction)
{
	int sig = ihk_mc_syscall_arg0(ctx);
	const struct sigaction *act = (const struct sigaction *)ihk_mc_syscall_arg1(ctx);
	struct sigaction *oact = (struct sigaction *)ihk_mc_syscall_arg2(ctx);
	size_t sigsetsize = ihk_mc_syscall_arg3(ctx);
	struct k_sigaction new_sa, old_sa;
	int rc;

	if(sig == SIGKILL || sig == SIGSTOP || sig <= 0 || sig > SIGRTMAX)
		return -EINVAL;
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if(act)
		if(copy_from_user(&new_sa.sa, act, sizeof new_sa.sa)){
			goto fault;
		}
	rc = do_sigaction(sig, act? &new_sa: NULL, oact? &old_sa: NULL);
	if(rc == 0 && oact)
		if(copy_to_user(oact, &old_sa.sa, sizeof old_sa.sa)){
			goto fault;
		}

	return rc;
fault:
	return -EFAULT;
}

/*
 * @ref.impl linux-linaro/src/linux-linaro/arch/arm64/kernel/signal.c::struct rt_sigframe
 * @ref.impl mckernel/arch/x86/kernel/syscall.c::struct sigsp
 */
struct sigsp {
	siginfo_t info;
	struct ucontext uc;
	unsigned long fp;
	unsigned long lr;
	unsigned long sigrc;
	int syscallno;
	int restart;
};

static int preserve_fpsimd_context(struct fpsimd_context __user *ctx)
{
	struct fpsimd_state fpsimd;
	int err;

	/* dump the hardware registers to the fpsimd_state structure */
	fpsimd_save_state(&fpsimd);

	/* copy the FP and status/control registers */
	err = copy_to_user(ctx->vregs, fpsimd.vregs, sizeof(fpsimd.vregs));
	ctx->fpsr = fpsimd.fpsr;
	ctx->fpcr = fpsimd.fpcr;

	/* copy the magic/size information */
	ctx->head.magic = FPSIMD_MAGIC;
	ctx->head.size = sizeof(struct fpsimd_context);

	return err ? -EFAULT : 0;
}

static int restore_fpsimd_context(struct fpsimd_context /*__user*/ *ctx)
{
	struct fpsimd_state fpsimd;
	unsigned int magic, size;

	/* check the magic/size information */
	magic = ctx->head.magic;
	size = ctx->head.size;
	if (magic != FPSIMD_MAGIC || size != sizeof(struct fpsimd_context))
		return -EINVAL;

	//  copy the FP and status/control registers 
	copy_from_user(fpsimd.vregs, ctx->vregs, sizeof(fpsimd.vregs));
	fpsimd.fpsr = ctx->fpsr;
	fpsimd.fpcr = ctx->fpcr;

	/* load the hardware registers from the fpsimd_state structure */
	fpsimd_load_state(&fpsimd);

	return 0;
}

SYSCALL_DECLARE(rt_sigreturn)
{
	int i, err = 0;
	struct thread *thread = cpu_local_var(current);
	ihk_mc_user_context_t *regs = ctx;
	struct sigsp *sigsp;
	void *aux;

	/*
	 * Since we stacked the signal on a 128-bit boundary, then 'sp' should
	 * be word aligned here.
	 */
	if (regs->sp & 15)
		goto bad_frame;

	sigsp = (struct sigsp *)regs->sp;

	for (i = 0; i < 31; i++) {
		regs->regs[i] = sigsp->uc.uc_mcontext.regs[i];
	}
	regs->sp = sigsp->uc.uc_mcontext.sp;
	regs->pc = sigsp->uc.uc_mcontext.pc;
	regs->pstate = sigsp->uc.uc_mcontext.pstate;

	// Avoid sys_rt_sigreturn() restarting. 
	regs->syscallno = ~0UL;

	aux = sigsp->uc.uc_mcontext.__reserved;
	struct fpsimd_context *fpsimd_ctx =
		container_of(aux, struct fpsimd_context, head);
	err |= restore_fpsimd_context(fpsimd_ctx);

	if (err)
		goto bad_frame;

	thread->sigmask.__val[0] = sigsp->uc.uc_sigmask.__val[0];
	thread->sigstack.ss_flags = sigsp->uc.uc_stack.ss_flags;
	if(sigsp->restart){
		return syscall(sigsp->syscallno, regs);
	}

	if (thread->ctx.thread->flags & (1 << TIF_SINGLESTEP)) {
		siginfo_t info = {
			.si_code = TRAP_HWBKPT,
		};
		regs->regs[0] = sigsp->sigrc;
		clear_single_step(thread);
		set_signal(SIGTRAP, regs, &info);
		check_signal(0, regs, 0);
		check_need_resched();
	}
	return sigsp->sigrc;

bad_frame:
	ekprintf("Error occurred during the restoration of the signal frame.");
	terminate(0, SIGSEGV);
	return -EINVAL;
}

extern struct cpu_local_var *clv;
extern unsigned long do_kill(struct thread *thread, int pid, int tid, int sig, struct siginfo *info, int ptracecont);
extern void interrupt_syscall(int pid, int tid);
extern int num_processors;

long
ptrace_read_user(struct thread *thread, long addr, unsigned long *value)
{
	return -EIO;
}

long
ptrace_write_user(struct thread *thread, long addr, unsigned long value)
{
	return -EIO;
}

long
alloc_debugreg(struct thread *thread)
{
	struct user_hwdebug_state *hws = NULL;

	/* LOWER:  breakpoint register area. */
	/* HIGHER: watchpoint register area. */
	hws = kmalloc(sizeof(struct user_hwdebug_state) * 2, IHK_MC_AP_NOWAIT);
	if (hws == NULL) {
		kprintf("alloc_debugreg: no memory.\n");
		return -ENOMEM;
	}
	memset(hws, 0, sizeof(struct user_hwdebug_state) * 2);

	/* initialize dbg_info */
	hws[HWS_BREAK].dbg_info = ptrace_hbp_get_resource_info(NT_ARM_HW_BREAK);
	hws[HWS_WATCH].dbg_info = ptrace_hbp_get_resource_info(NT_ARM_HW_WATCH);

	thread->ptrace_debugreg = (unsigned long *)hws;
	return 0;
}

void
save_debugreg(unsigned long *debugreg)
{
	struct user_hwdebug_state *hws = (struct user_hwdebug_state *)debugreg;
	int i = 0;

	/* save DBGBVR<n>_EL1 and DBGBCR<n>_EL1 (n=0-(core_num_brps-1)) */
	for (i = 0; i < core_num_brps; i++) {
		hws[HWS_BREAK].dbg_regs[i].addr = read_wb_reg(AARCH64_DBG_REG_BVR, i);
		hws[HWS_BREAK].dbg_regs[i].ctrl = read_wb_reg(AARCH64_DBG_REG_BCR, i);
	}

	/* save DBGWVR<n>_EL1 and DBGWCR<n>_EL1 (n=0-(core_num_wrps-1)) */
	for (i = 0; i < core_num_wrps; i++) {
		hws[HWS_WATCH].dbg_regs[i].addr = read_wb_reg(AARCH64_DBG_REG_WVR, i);
		hws[HWS_WATCH].dbg_regs[i].ctrl = read_wb_reg(AARCH64_DBG_REG_WCR, i);
	}
}

void
restore_debugreg(unsigned long *debugreg)
{
	struct user_hwdebug_state *hws = (struct user_hwdebug_state *)debugreg;
	unsigned int mdscr;
	int i = 0;

	/* set MDSCR_EL1.MDE */
	mdscr = mdscr_read();
	mdscr |= DBG_MDSCR_MDE;
	mdscr_write(mdscr);

	/* restore DBGBVR<n>_EL1 and DBGBCR<n>_EL1 (n=0-(core_num_brps-1)) */
	for (i = 0; i < core_num_brps; i++) {
		write_wb_reg(AARCH64_DBG_REG_BVR, i, hws[HWS_BREAK].dbg_regs[i].addr);
		write_wb_reg(AARCH64_DBG_REG_BCR, i, hws[HWS_BREAK].dbg_regs[i].ctrl);
	}

	/* restore DBGWVR<n>_EL1 and DBGWCR<n>_EL1 (n=0-(core_num_wrps-1)) */
	for (i = 0; i < core_num_wrps; i++) {
		write_wb_reg(AARCH64_DBG_REG_WVR, i, hws[HWS_WATCH].dbg_regs[i].addr);
		write_wb_reg(AARCH64_DBG_REG_WCR, i, hws[HWS_WATCH].dbg_regs[i].ctrl);
	}
}

void
clear_debugreg(void)
{
	unsigned int mdscr;

	/* clear DBGBVR<n>_EL1 and DBGBCR<n>_EL1 (n=0-(core_num_brps-1)) */
	/* clear DBGWVR<n>_EL1 and DBGWCR<n>_EL1 (n=0-(core_num_wrps-1)) */
	hw_breakpoint_reset();

	/* clear MDSCR_EL1.MDE */
	mdscr = mdscr_read();
	mdscr &= ~DBG_MDSCR_MDE;
	mdscr_write(mdscr);
}

void clear_single_step(struct thread *thread)
{
	clear_regs_spsr_ss(thread->uctx);
	thread->ctx.thread->flags &= ~(1 << TIF_SINGLESTEP);
}

void set_single_step(struct thread *thread)
{
	thread->ctx.thread->flags |= (1 << TIF_SINGLESTEP);
	set_regs_spsr_ss(thread->uctx);
}

long ptrace_read_fpregs(struct thread *thread, void *fpregs)
{
	return -EIO;
}

long ptrace_write_fpregs(struct thread *thread, void *fpregs)
{
	return -EIO;
}

long ptrace_read_regset(struct thread *thread, long type, struct iovec *iov)
{
	long rc = -EINVAL;

	switch (type) {
	case NT_PRSTATUS:
		if (iov->iov_len > sizeof(struct user_pt_regs)) {
			iov->iov_len = sizeof(struct user_pt_regs);
		}
		rc = copy_to_user(iov->iov_base, &thread->uctx->user_regs, iov->iov_len);
		break;

	case NT_PRFPREG:
		if (thread->fp_regs == NULL) {
			return -ENOMEM;
		}
		if (iov->iov_len > sizeof(struct user_fpsimd_state)) {
			iov->iov_len = sizeof(struct user_fpsimd_state);
		}
		rc = copy_to_user(iov->iov_base, &thread->fp_regs->user_fpsimd, iov->iov_len);
		break;

	case NT_ARM_TLS:
		if (iov->iov_len > sizeof(thread->tlsblock_base)) {
			iov->iov_len = sizeof(thread->tlsblock_base);
		}
		rc = copy_to_user(iov->iov_base, &thread->tlsblock_base, iov->iov_len);
		break;

	case NT_ARM_HW_BREAK:
	case NT_ARM_HW_WATCH:
	{
		struct user_hwdebug_state *hws = (struct user_hwdebug_state *)thread->ptrace_debugreg;
		int bw = (type == NT_ARM_HW_BREAK ? HWS_BREAK : HWS_WATCH);

		if (hws == NULL) {
			return -ENOMEM;
		}

		if (iov->iov_len > sizeof(struct user_hwdebug_state)) {
			iov->iov_len = sizeof(struct user_hwdebug_state);
		}
		rc = copy_to_user(iov->iov_base, &hws[bw], iov->iov_len);
		break;
	}

	case NT_ARM_SYSTEM_CALL:
		if (iov->iov_len > sizeof(thread->uctx->syscallno)) {
			iov->iov_len = sizeof(thread->uctx->syscallno);
		}
		rc = copy_to_user(iov->iov_base, &thread->uctx->syscallno, iov->iov_len);
		break;

	default:
		kprintf("ptrace_read_regset: not supported type 0x%x\n", type);
		break;
	}
	return rc;
}

long ptrace_write_regset(struct thread *thread, long type, struct iovec *iov)
{
	long rc = -EINVAL;

	switch (type) {
	case NT_PRSTATUS:
		if (iov->iov_len > sizeof(struct user_pt_regs)) {
			iov->iov_len = sizeof(struct user_pt_regs);
		}
		rc = copy_from_user(&thread->uctx->user_regs, iov->iov_base, iov->iov_len);
		break;

	case NT_PRFPREG:
		if (thread->fp_regs == NULL) {
			return -ENOMEM;
		}
		if (iov->iov_len > sizeof(struct user_fpsimd_state)) {
			iov->iov_len = sizeof(struct user_fpsimd_state);
		}
		rc = copy_from_user(&thread->fp_regs->user_fpsimd, iov->iov_base, iov->iov_len);
		break;

	case NT_ARM_TLS:
		if (iov->iov_len > sizeof(thread->tlsblock_base)) {
			iov->iov_len = sizeof(thread->tlsblock_base);
		}
		rc = copy_from_user(&thread->tlsblock_base, iov->iov_base, iov->iov_len);
		break;

	case NT_ARM_HW_BREAK:
	case NT_ARM_HW_WATCH:
	{
		struct user_hwdebug_state *hws = (struct user_hwdebug_state *)thread->ptrace_debugreg;
		struct user_hwdebug_state kwork;
		int bw = (type == NT_ARM_HW_BREAK ? HWS_BREAK : HWS_WATCH);
		int max_size = (type == NT_ARM_HW_BREAK ? max_br_size : max_wr_size);
		int ret;
		unsigned long offset = offsetof(struct user_hwdebug_state, dbg_regs);

		/* ptrace_debugreg is NULL. */
		if (hws == NULL) {
			return -ENOMEM;
		}

		/* clear kernel work space. */
		memset(&kwork, 0, sizeof(kwork));

		/* iov_len check and fix. */
		if (iov->iov_len > sizeof(struct user_hwdebug_state)) {
			iov->iov_len = sizeof(struct user_hwdebug_state);
		}

		/* over support regnum check */
		if (iov->iov_len > max_size) {
			return -ENOSPC;
		}

		/* copy from user, kernel work space. */
		rc = copy_from_user(&kwork, iov->iov_base, iov->iov_len);

		/* check iov_base value. */
		ret = arch_validate_hwbkpt_settings(type, &kwork, iov->iov_len);
		if (ret) {
			rc = ret;
		} else {
			/* check is OK, copy to ptrace_debugreg. */
			/* dbg_info is no copy. */
			memcpy(hws[bw].dbg_regs, kwork.dbg_regs, iov->iov_len - offset);
		}
		break;
	}

	case NT_ARM_SYSTEM_CALL:
		if (iov->iov_len > sizeof(thread->uctx->syscallno)) {
			iov->iov_len = sizeof(thread->uctx->syscallno);
		}
		rc = copy_from_user(&thread->uctx->syscallno, iov->iov_base, iov->iov_len);
		break;

	default:
		kprintf("ptrace_write_regset: not supported type 0x%x\n", type);
		break;
	}
	return rc;
}

extern void coredump(struct thread *thread, void *regs);

void ptrace_report_signal(struct thread *thread, int sig)
{
	struct mcs_rwlock_node_irqsave lock;
	struct process *proc = thread->proc;
	int parent_pid;
	siginfo_t info;
	struct thread_info tinfo;

	dkprintf("ptrace_report_signal,pid=%d\n", thread->proc->pid);

	/* save thread_info, if called by ptrace_report_exec() */
	if (sig == ((SIGTRAP | (PTRACE_EVENT_EXEC << 8)))) {
		memcpy(&tinfo, thread->ctx.thread, sizeof(struct thread_info));
	}

	mcs_rwlock_writer_lock(&proc->update_lock, &lock);	
	if(!(proc->ptrace & PT_TRACED)){
		mcs_rwlock_writer_unlock(&proc->update_lock, &lock);
		return;
	}
	proc->exit_status = sig;
	/* Transition thread state */
#ifdef POSTK_DEBUG_TEMP_FIX_41 /* early to wait4() wakeup for ptrace, fix. */
	proc->status = PS_DELAY_TRACED;
#else /* POSTK_DEBUG_TEMP_FIX_41 */
	proc->status = PS_TRACED;
#endif /* POSTK_DEBUG_TEMP_FIX_41 */
	thread->status = PS_TRACED;
	proc->ptrace &= ~PT_TRACE_SYSCALL_MASK;
	if (sig == SIGSTOP || sig == SIGTSTP ||
			sig == SIGTTIN || sig == SIGTTOU) {
		proc->signal_flags |= SIGNAL_STOP_STOPPED;
	} else {
		proc->signal_flags &= ~SIGNAL_STOP_STOPPED;
	}
	parent_pid = proc->parent->pid;
	save_debugreg(thread->ptrace_debugreg);
	mcs_rwlock_writer_unlock(&proc->update_lock, &lock);

	memset(&info, '\0', sizeof info);
	info.si_signo = SIGCHLD;
	info.si_code = CLD_TRAPPED;
	info._sifields._sigchld.si_pid = thread->proc->pid;
	info._sifields._sigchld.si_status = thread->proc->exit_status;
	do_kill(cpu_local_var(current), parent_pid, -1, SIGCHLD, &info, 0);
#ifndef POSTK_DEBUG_TEMP_FIX_41 /* early to wait4() wakeup for ptrace, fix. */
	/* Wake parent (if sleeping in wait4()) */
	waitq_wakeup(&proc->parent->waitpid_q);
#endif /* !POSTK_DEBUG_TEMP_FIX_41 */

	dkprintf("ptrace_report_signal,sleeping\n");
	/* Sleep */
	schedule();
	dkprintf("ptrace_report_signal,wake up\n");

	/* restore thread_info, if called by ptrace_report_exec() */
	if (sig == ((SIGTRAP | (PTRACE_EVENT_EXEC << 8)))) {
		memcpy(thread->ctx.thread, &tinfo, sizeof(struct thread_info));
	}
}

long
arch_ptrace(long request, int pid, long addr, long data)
{
	return -EIO;
}

static int
isrestart(int syscallno, unsigned long rc, int sig, int restart)
{
	if(syscallno == 0 || rc != -EINTR)
		return 0;

	/* 
	 * The following interfaces are never restarted after being interrupted 
	 * by a signal handler, regardless of the use of SA_RESTART
	 * Interfaces used to wait for signals: 
	 * 	pause(2), sigsuspend(2), sigtimedwait(2), and sigwaitinfo(2).
	 * File descriptor multiplexing interfaces: 
	 * 	epoll_wait(2), epoll_pwait(2), poll(2), ppoll(2), select(2), and pselect(2).
	 * System V IPC interfaces: 
	 * 	msgrcv(2), msgsnd(2), semop(2), and semtimedop(2).
	 * Sleep interfaces: 
	 * 	clock_nanosleep(2), nanosleep(2), and usleep(3).
	 * io_getevents(2).
	 *
	 * Note: following functions will issue another systemcall.
	 *   pause(2)      -> rt_sigsuspend
	 *   epoll_wait(2) -> epoll_pwait
	 *   poll(2)       -> ppoll
	 *   select(2)     -> pselect6
	 */
	switch(syscallno){
		case __NR_rt_sigsuspend:
		case __NR_rt_sigtimedwait:
		case __NR_epoll_pwait:
		case __NR_ppoll:
		case __NR_pselect6:
		case __NR_msgrcv:
		case __NR_msgsnd:
		case __NR_semop:
		case __NR_semtimedop:
		case __NR_clock_nanosleep:
		case __NR_nanosleep:
		case __NR_io_getevents:
			return 0;
	}

	if(sig == SIGCHLD)
		return 1;
	if(restart)
		return 1;
	return 0;
}

void
do_signal(unsigned long rc, void *regs0, struct thread *thread, struct sig_pending *pending, int syscallno)
{
	struct pt_regs *regs = regs0;
	struct k_sigaction *k;
	int	sig;
	__sigset_t w;
	struct process *proc = thread->proc;
	int	orgsig;
	int	ptraceflag = 0;
	struct mcs_rwlock_node_irqsave lock;
	unsigned long irqstate;

	for(w = pending->sigmask.__val[0], sig = 0; w; sig++, w >>= 1);
	dkprintf("do_signal,pid=%d,sig=%d\n", thread->proc->pid, sig);
	orgsig = sig;

	if((proc->ptrace & PT_TRACED) &&
	   pending->ptracecont == 0 &&
	   sig != SIGKILL) {
		ptraceflag = 1;
		sig = SIGSTOP;
	}

	if(regs == NULL){ /* call from syscall */
		regs = thread->uctx;
	}
	else{
		rc = regs->regs[0];
	}

	irqstate = ihk_mc_spinlock_lock(&thread->sigcommon->lock);
	k = thread->sigcommon->action + sig - 1;

	if(k->sa.sa_handler == SIG_IGN){
		kfree(pending);
		ihk_mc_spinlock_unlock(&thread->sigcommon->lock, irqstate);
		return;
	}
	else if(k->sa.sa_handler){
		/* save signal frame */
		int i, err = 0, to_restart = 0;
		struct sigsp *sigsp;
		unsigned long *usp; /* user stack */
		uintptr_t addr;

		// check syscall to have restart ?
		to_restart = isrestart(syscallno, rc, sig, k->sa.sa_flags & SA_RESTART);
		if(syscallno != 0 && rc == -EINTR && sig == SIGCHLD) {
			to_restart = 1;
		}
		if(to_restart == 1) {
			/* Prepare for system call restart. */
			regs->regs[0] = regs->orig_x0;
		}

		// get signal frame
		if((k->sa.sa_flags & SA_ONSTACK) &&
		   !(thread->sigstack.ss_flags & SS_DISABLE) &&
		   !(thread->sigstack.ss_flags & SS_ONSTACK)){
			unsigned long lsp;
			lsp = ((unsigned long)(((char *)thread->sigstack.ss_sp) + thread->sigstack.ss_size)) & ~15UL;
			usp = (unsigned long *)lsp;
			thread->sigstack.ss_flags |= SS_ONSTACK;
		}
		else{
			usp = (unsigned long *)regs->sp;
		}

		sigsp = ((struct sigsp *)usp) - 1;
		sigsp = (struct sigsp *)((unsigned long)sigsp & ~15);
		sigsp = (struct sigsp *)page_align(sigsp);

		// To ensure the assignment of the physical page, call page_fault_thread_vm()
		for (addr = (uintptr_t)sigsp; addr < (uintptr_t)(sigsp) + sizeof(struct sigsp); addr += PAGE_SIZE) {
			err = page_fault_process_vm(thread->vm, (void *)addr, PF_POPULATE | PF_WRITE | PF_USER);
			if (err) {
				kfree(pending);
				ihk_mc_spinlock_unlock(&thread->sigcommon->lock, irqstate);
				kprintf("do_signal,page_fault_thread_vm failed\n");
				terminate(0, sig);
				return;
			}
		}

		// init non use data.
		sigsp->uc.uc_flags = 0;
		sigsp->uc.uc_link = NULL;

		// save alternate stack infomation.
		sigsp->uc.uc_stack.ss_sp = usp;
		sigsp->uc.uc_stack.ss_flags = thread->sigstack.ss_size;
		sigsp->uc.uc_stack.ss_size = thread->sigstack.ss_flags;

		// save signal frame.
		sigsp->fp = regs->regs[29];
		sigsp->lr = regs->regs[30];
		sigsp->sigrc = rc;

		for (i = 0; i < 31; i++)
		{
			sigsp->uc.uc_mcontext.regs[i] = regs->regs[i];
		}
		sigsp->uc.uc_mcontext.sp = regs->sp;
		sigsp->uc.uc_mcontext.pc = regs->pc;
		sigsp->uc.uc_mcontext.pstate = regs->pstate;

		sigsp->uc.uc_mcontext.fault_address = current_thread_info()->fault_address;

		err |= copy_to_user(&sigsp->uc.uc_sigmask, &thread->sigmask, sizeof(sigset_t));

		if(err == 0) {
			struct _aarch64_ctx *end;
			void *aux = sigsp->uc.uc_mcontext.__reserved;

	 		// save fp simd context.
			struct fpsimd_context *fpsimd_ctx =
				container_of(aux, struct fpsimd_context, head);
			err |= preserve_fpsimd_context(fpsimd_ctx);
			aux += sizeof(*fpsimd_ctx);

			// save esr context.
			if (current_thread_info()->fault_code) {
				struct esr_context *esr_ctx =
					container_of(aux, struct esr_context, head);
				esr_ctx->head.magic = ESR_MAGIC;
				esr_ctx->head.size = sizeof(*esr_ctx);
				esr_ctx->esr = current_thread_info()->fault_code;
				aux += sizeof(*esr_ctx);
			}

			// set the "end" magic
			end = aux;
			end->magic = 0;
			end->size = 0;
 		}

		// save syscall infomation to restart.
		sigsp->syscallno = syscallno;
		sigsp->restart = to_restart;

		/* set sig handler context */
		// set restart context
		regs->regs[0] = sig;
		regs->sp = (unsigned long)sigsp;
		regs->regs[29] = regs->sp + offsetof(struct sigsp, fp);
		regs->pc = (unsigned long)k->sa.sa_handler;

		if (k->sa.sa_flags & SA_RESTORER){
			regs->regs[30] = (unsigned long)k->sa.sa_restorer;
		} else {
			regs->regs[30] = (unsigned long)VDSO_SYMBOL(thread->vm->vdso_addr, sigtramp);
		}

		if(k->sa.sa_flags & SA_SIGINFO){
			err |= copy_to_user(&sigsp->info, &pending->info, sizeof(siginfo_t));
			regs->regs[1] = (unsigned long)&sigsp->info;
			regs->regs[2] = (unsigned long)&sigsp->uc;
		}

		// check signal handler is ONESHOT
		if(k->sa.sa_flags & SA_RESETHAND) {
			k->sa.sa_handler = SIG_DFL; 
		}

		if(!(k->sa.sa_flags & SA_NODEFER))
			thread->sigmask.__val[0] |= pending->sigmask.__val[0];
		kfree(pending);
		ihk_mc_spinlock_unlock(&thread->sigcommon->lock, irqstate);

		if (thread->ctx.thread->flags & (1 << TIF_SINGLESTEP)) {
			siginfo_t info = {
				.si_code = TRAP_HWBKPT,
			};
			clear_single_step(thread);
			set_signal(SIGTRAP, regs, &info);
			check_signal(0, regs, 0);
			check_need_resched();
		}
	}
	else {
		int	coredumped = 0;
		siginfo_t info;

		if(ptraceflag){
			if(thread->ptrace_recvsig)
				kfree(thread->ptrace_recvsig);
			thread->ptrace_recvsig = pending;
			if(thread->ptrace_sendsig)
				kfree(thread->ptrace_sendsig);
			thread->ptrace_sendsig = NULL;
		}
		else
			kfree(pending);
		ihk_mc_spinlock_unlock(&thread->sigcommon->lock, irqstate);
		switch (sig) {
		case SIGSTOP:
		case SIGTSTP:
		case SIGTTIN:
		case SIGTTOU:
			if(ptraceflag){
				ptrace_report_signal(thread, orgsig);
			}
			else{
				memset(&info, '\0', sizeof info);
				info.si_signo = SIGCHLD;
				info.si_code = CLD_STOPPED;
				info._sifields._sigchld.si_pid = thread->proc->pid;
				info._sifields._sigchld.si_status = (sig << 8) | 0x7f;
				do_kill(cpu_local_var(current), thread->proc->parent->pid, -1, SIGCHLD, &info, 0);
				dkprintf("do_signal,SIGSTOP,changing state\n");

				/* Update thread state in fork tree */
				mcs_rwlock_writer_lock(&proc->update_lock, &lock);	
				proc->group_exit_status = SIGSTOP;

				/* Reap and set new signal_flags */
				proc->signal_flags = SIGNAL_STOP_STOPPED;

#ifdef POSTK_DEBUG_TEMP_FIX_41 /* early to wait4() wakeup for ptrace, fix. */
				proc->status = PS_DELAY_STOPPED;
#else /* POSTK_DEBUG_TEMP_FIX_41 */
				proc->status = PS_STOPPED;
#endif /* POSTK_DEBUG_TEMP_FIX_41 */
				thread->status = PS_STOPPED;
				mcs_rwlock_writer_unlock(&proc->update_lock, &lock);	

#ifndef POSTK_DEBUG_TEMP_FIX_41 /* early to wait4() wakeup for ptrace, fix. */
				/* Wake up the parent who tried wait4 and sleeping */
				waitq_wakeup(&proc->parent->waitpid_q);
#endif /* !POSTK_DEBUG_TEMP_FIX_41 */

				dkprintf("do_signal,SIGSTOP,sleeping\n");
				/* Sleep */
				schedule();
				dkprintf("SIGSTOP(): woken up\n");
			}
			break;
		case SIGTRAP:
			dkprintf("do_signal,SIGTRAP\n");
			if(!(proc->ptrace & PT_TRACED)) {
				goto core;
			}

			/* Update thread state in fork tree */
			mcs_rwlock_writer_lock(&proc->update_lock, &lock);	
			proc->exit_status = SIGTRAP;
#ifdef POSTK_DEBUG_TEMP_FIX_41 /* early to wait4() wakeup for ptrace, fix. */
			proc->status = PS_DELAY_TRACED;
#else /* POSTK_DEBUG_TEMP_FIX_41 */
			proc->status = PS_TRACED;
#endif /* POSTK_DEBUG_TEMP_FIX_41 */
			thread->status = PS_TRACED;
			mcs_rwlock_writer_unlock(&proc->update_lock, &lock);	

#ifndef POSTK_DEBUG_TEMP_FIX_41 /* early to wait4() wakeup for ptrace, fix. */
			/* Wake up the parent who tried wait4 and sleeping */
			waitq_wakeup(&thread->proc->parent->waitpid_q);
#endif /* !POSTK_DEBUG_TEMP_FIX_41 */

			/* Sleep */
			dkprintf("do_signal,SIGTRAP,sleeping\n");

			schedule();
			dkprintf("SIGTRAP(): woken up\n");
			break;
		case SIGCONT:
			memset(&info, '\0', sizeof info);
			info.si_signo = SIGCHLD;
			info.si_code = CLD_CONTINUED;
			info._sifields._sigchld.si_pid = proc->pid;
			info._sifields._sigchld.si_status = 0x0000ffff;
			do_kill(cpu_local_var(current), proc->parent->pid, -1, SIGCHLD, &info, 0);
			proc->signal_flags = SIGNAL_STOP_CONTINUED;
			proc->status = PS_RUNNING;
			dkprintf("do_signal,SIGCONT,do nothing\n");
			break;
		case SIGQUIT:
		case SIGILL:
		case SIGABRT:
		case SIGFPE:
		case SIGSEGV:
		case SIGBUS:
		case SIGSYS:
		case SIGXCPU:
		case SIGXFSZ:
		core:
			dkprintf("do_signal,default,core,sig=%d\n", sig);
			coredump(thread, regs);
			coredumped = 0x80;
			terminate(0, sig | coredumped);
			break;
		case SIGCHLD:
		case SIGURG:
			break;
		default:
			dkprintf("do_signal,default,terminate,sig=%d\n", sig);
			terminate(0, sig);
			break;
		}
	}
}

static struct sig_pending *
getsigpending(struct thread *thread, int delflag){
	struct list_head *head;
	ihk_spinlock_t *lock;
	struct sig_pending *next;
	struct sig_pending *pending;
	__sigset_t w;
	int	irqstate;

	w = thread->sigmask.__val[0];

	lock = &thread->sigcommon->lock;
	head = &thread->sigcommon->sigpending;
	for(;;){
		irqstate = ihk_mc_spinlock_lock(lock);
		list_for_each_entry_safe(pending, next, head, list){
			if(!(pending->sigmask.__val[0] & w)){
				if(delflag)
					list_del(&pending->list);
				ihk_mc_spinlock_unlock(lock, irqstate);
				return pending;
			}
		}
		ihk_mc_spinlock_unlock(lock, irqstate);

		if(lock == &thread->sigpendinglock)
			return NULL;
		lock = &thread->sigpendinglock;
		head = &thread->sigpending;
	}

	return NULL;
}

struct sig_pending *
hassigpending(struct thread *thread)
{
	return getsigpending(thread, 0);
}

int
interrupt_from_user(void *regs0)
{
	struct pt_regs *regs = regs0;

	return((regs->pstate & PSR_MODE_MASK) == PSR_MODE_EL0t);
}

void
check_signal(unsigned long rc, void *regs0, int num)
{
	ihk_mc_user_context_t *regs = regs0;
	struct thread *thread;
	struct sig_pending *pending;
	int	irqstate;

	if(clv == NULL)
		return;
	thread = cpu_local_var(current);

	/**
	 * If check_signal is called from syscall(), 
	 * then save syscall return value.
	 */
	if((regs == NULL)&&(num != __NR_rt_sigsuspend)){ /* It's call from syscall! */
	 	// Get user context through current thread
	 	// and update syscall return.
	 	ihk_mc_syscall_arg0(thread->uctx) = rc;
	}

	if(thread == NULL || thread->proc->pid == 0){
		struct thread *t;
		irqstate = ihk_mc_spinlock_lock(&(cpu_local_var(runq_lock)));
		list_for_each_entry(t, &(cpu_local_var(runq)), sched_list){
			if(t->proc->pid <= 0)
				continue;
			if(t->status == PS_INTERRUPTIBLE &&
			   hassigpending(t)){
				t->status = PS_RUNNING;
				break;
			}
		}
		ihk_mc_spinlock_unlock(&(cpu_local_var(runq_lock)), irqstate);
		return;
	}

	if(regs != NULL && !interrupt_from_user(regs)) {
		return;
	}

	for(;;){
		pending = getsigpending(thread, 1);
		if(!pending) {
			dkprintf("check_signal,queue is empty\n");
			return;
		}
		do_signal(rc, regs, thread, pending, num);
	}
}

unsigned long
do_kill(struct thread * thread, int pid, int tid, int sig, siginfo_t *info, int ptracecont)
{
	dkprintf("do_kill,pid=%d,tid=%d,sig=%d\n", pid, tid, sig);
	struct thread *t;
	struct process *tproc;
	struct process *proc = thread? thread->proc: NULL;
	struct thread *tthread = NULL;
	int i;
	__sigset_t mask;
	ihk_spinlock_t *savelock = NULL;
	struct list_head *head = NULL;
	int rc;
	unsigned long irqstate = 0;
	struct k_sigaction *k;
	int doint;
	int found = 0;
	siginfo_t info0;
	struct resource_set *rset = cpu_local_var(resource_set);
	int hash;
	struct thread_hash *thash = rset->thread_hash;
	struct process_hash *phash = rset->process_hash;
	struct mcs_rwlock_node lock;
	struct mcs_rwlock_node updatelock;

	if(sig > SIGRTMAX || sig < 0)
		return -EINVAL;

	if(info == NULL){
		memset(&info0, '\0', sizeof info0);
		info = &info0;
		info0.si_signo = sig;
		info0.si_code = SI_KERNEL;
	}

	if(tid == -1 && pid <= 0){
		struct process *p;
		struct mcs_rwlock_node_irqsave slock;
		int	pgid = -pid;
		int	rc = -ESRCH;
		int	*pids;
		int	n = 0;
		int	sendme = 0;

		if(pid == 0){
			if(thread == NULL || thread->proc->pid <= 0)
				return -ESRCH;
			pgid = thread->proc->pgid;
		}
		pids = kmalloc(sizeof(int) * num_processors, IHK_MC_AP_NOWAIT);
		if(!pids)
			return -ENOMEM;
		for(i = 0; i < HASH_SIZE; i++){
			mcs_rwlock_reader_lock(&phash->lock[i], &slock);
			list_for_each_entry(p, &phash->list[i], hash_list){
				if(pgid != 1 && p->pgid != pgid)
					continue;

				if(thread && p->pid == thread->proc->pid){
					sendme = 1;
					continue;
				}

				pids[n] = p->pid;
				n++;
			}
			mcs_rwlock_reader_unlock(&phash->lock[i], &slock);
		}
		for(i = 0; i < n; i++)
			rc = do_kill(thread, pids[i], -1, sig, info, ptracecont);
		if(sendme)
			rc = do_kill(thread, thread->proc->pid, -1, sig, info, ptracecont);

		kfree(pids);
		return rc;
	}

	irqstate = cpu_disable_interrupt_save();
	mask = __sigmask(sig);
	if(tid == -1){
		struct thread *tthread0 = NULL;
		struct mcs_rwlock_node plock;
		struct mcs_rwlock_node updatelock;

		found = 0;
		hash = process_hash(pid);
		mcs_rwlock_reader_lock_noirq(&phash->lock[hash], &plock);
		list_for_each_entry(tproc, &phash->list[hash], hash_list){
			if(tproc->pid == pid){
				found = 1;
				break;
		}
	}
		if(!found){
			mcs_rwlock_reader_unlock_noirq(&phash->lock[hash], &plock);
			cpu_restore_interrupt(irqstate);
			return -ESRCH;
	}

		mcs_rwlock_reader_lock_noirq(&tproc->update_lock, &updatelock);
		if(tproc->status == PS_EXITED || tproc->status == PS_ZOMBIE){
			goto done;
		}
		mcs_rwlock_reader_lock_noirq(&tproc->threads_lock, &lock);
		list_for_each_entry(t, &tproc->threads_list, siblings_list){
			if(t->tid == pid || tthread == NULL){
				if(t->status == PS_EXITED){
					continue;
				}
				if(!(mask & t->sigmask.__val[0])){
					tthread = t;
					found = 1;
				}
				else if(tthread == NULL && tthread0 == NULL){
					tthread0 = t;
					found = 1;
			}
		}
	}
		if(tthread == NULL){
			tthread = tthread0;
		}
		if(tthread && tthread->status != PS_EXITED){
			savelock = &tthread->sigcommon->lock;
			head = &tthread->sigcommon->sigpending;
			hold_thread(tthread);
		}
		else
			tthread = NULL;
		mcs_rwlock_reader_unlock_noirq(&tproc->threads_lock, &lock);
done:
		mcs_rwlock_reader_unlock_noirq(&tproc->update_lock, &updatelock);
		mcs_rwlock_reader_unlock_noirq(&phash->lock[hash], &plock);
       }
       else{
		found = 0;
		hash = thread_hash(tid);
		mcs_rwlock_reader_lock_noirq(&thash->lock[hash], &lock);
		list_for_each_entry(tthread, &thash->list[hash], hash_list){
			if(pid != -1 && tthread->proc->pid != pid){
				continue;
		}
			if(tthread->tid == tid){
				found = 1;
			break;
		}
	}
		if(!found){
			mcs_rwlock_reader_unlock_noirq(&thash->lock[hash], &lock);
			cpu_restore_interrupt(irqstate);
			return -ESRCH;
		}

		tproc = tthread->proc;
		mcs_rwlock_reader_lock_noirq(&tproc->update_lock, &updatelock);
		savelock = &tthread->sigpendinglock;
		head = &tthread->sigpending;
		if(sig == SIGKILL ||
		   (tproc->status != PS_EXITED &&
		    tproc->status != PS_ZOMBIE &&
		    tthread->status != PS_EXITED)){
			hold_thread(tthread);
		}
		else{
			tthread = NULL;
		}
		mcs_rwlock_reader_unlock_noirq(&tproc->update_lock, &updatelock);
		mcs_rwlock_reader_unlock_noirq(&thash->lock[hash], &lock);
	}


       if(sig != SIGCONT &&
	  proc &&
	  proc->euid != 0 &&
	  proc->ruid != tproc->ruid &&
	  proc->euid != tproc->ruid &&
	  proc->ruid != tproc->suid &&
	  proc->euid != tproc->suid){
		if(tthread)
			release_thread(tthread);
	cpu_restore_interrupt(irqstate);
	return -EPERM;
	}

	if(sig == 0 || tthread == NULL || tthread->status == PS_EXITED){
		if(tthread)
			release_thread(tthread);
		cpu_restore_interrupt(irqstate);
		return 0;
	}

	doint = 0;

	ihk_mc_spinlock_lock_noirq(savelock);

	/* Put signal event even when handler is SIG_IGN or SIG_DFL
	   because target ptraced thread must call ptrace_report_signal 
	   in check_signal */
	rc = 0;
	k = tthread->sigcommon->action + sig - 1;
	if((sig != SIGKILL && (tproc->ptrace & PT_TRACED)) ||
			(k->sa.sa_handler != SIG_IGN &&
			 (k->sa.sa_handler != NULL ||
			  (sig != SIGCHLD && sig != SIGURG)))){
		struct sig_pending *pending = NULL;
		if (sig < SIGRTMIN) { // SIGRTMIN - SIGRTMAX
			list_for_each_entry(pending, head, list){
				if(pending->sigmask.__val[0] == mask &&
				   pending->ptracecont == ptracecont)
					break;
			}
			if(&pending->list == head)
				pending = NULL;
		}
		if(pending == NULL){
			doint = 1;
			pending = kmalloc(sizeof(struct sig_pending), IHK_MC_AP_NOWAIT);
			if(!pending){
				rc = -ENOMEM;
			}
			else{
				pending->sigmask.__val[0] = mask;
				memcpy(&pending->info, info, sizeof(siginfo_t));
				pending->ptracecont = ptracecont;
				if(sig == SIGKILL || sig == SIGSTOP)
					list_add(&pending->list, head);
				else
					list_add_tail(&pending->list, head);
				tthread->sigevent = 1;
			}
		}
	}

	ihk_mc_spinlock_unlock_noirq(savelock);
	cpu_restore_interrupt(irqstate);

	if (doint && !(mask & tthread->sigmask.__val[0])) {
		int tid = tthread->tid;
		int pid = tproc->pid;
		int status = tthread->status;

		if (thread != tthread) {
			dkprintf("do_kill,ipi,pid=%d,cpu_id=%d\n",
				 tproc->pid, tthread->cpu_id);
#define IPI_CPU_NOTIFY 0
			ihk_mc_interrupt_cpu(tthread->cpu_id, INTRID_CPU_NOTIFY);
		}

#ifdef POSTK_DEBUG_TEMP_FIX_48 /* nohost flag missed fix */
		if(tthread->proc->status != PS_EXITED)
			interrupt_syscall(pid, tid);
#else /* POSTK_DEBUG_TEMP_FIX_48 */
		if(!tthread->proc->nohost)
			interrupt_syscall(pid, tid);
#endif /* POSTK_DEBUG_TEMP_FIX_48 */

		if (status != PS_RUNNING) {
			if(sig == SIGKILL){
				/* Wake up the target only when stopped by ptrace-reporting */
				sched_wakeup_thread(tthread, PS_TRACED | PS_STOPPED | PS_INTERRUPTIBLE);
			}
			else if(sig == SIGCONT || ptracecont == 1){
				/* Wake up the target only when stopped by SIGSTOP */
				sched_wakeup_thread(tthread, PS_STOPPED);
				tthread->proc->status = PS_RUNNING;
			}
		}
	}
	release_thread(tthread);
	return rc;
}

void
set_signal(int sig, void *regs0, siginfo_t *info)
{
	ihk_mc_user_context_t *regs = regs0;
	struct thread *thread = cpu_local_var(current);

	if(thread == NULL || thread->proc->pid == 0)
		return;

	if((__sigmask(sig) & thread->sigmask.__val[0]) ||
	   !interrupt_from_user(regs)){
		coredump(thread, regs0);
		terminate(0, sig | 0x80);
	}

	do_kill(thread, thread->proc->pid, thread->tid, sig, info, 0);
}

SYSCALL_DECLARE(mmap)
{
	const int supported_flags = 0
		| MAP_SHARED		// 0x01
		| MAP_PRIVATE		// 0x02
		| MAP_FIXED		// 0x10
		| MAP_ANONYMOUS		// 0x20
		| MAP_LOCKED		// 0x2000
		| MAP_POPULATE		// 0x8000
		| MAP_HUGETLB		// 00040000
		| (0x3F << MAP_HUGE_SHIFT) // FC000000
		;
	const int ignored_flags = 0
		| MAP_DENYWRITE		// 0x0800
		| MAP_NORESERVE		// 0x4000
		| MAP_STACK		// 0x20000
		;
	const int error_flags = 0
		| MAP_GROWSDOWN		// 0x0100
		| MAP_EXECUTABLE	// 0x1000
		| MAP_NONBLOCK		// 0x10000
		;

	const intptr_t addr0 = ihk_mc_syscall_arg0(ctx);
	const size_t len0 = ihk_mc_syscall_arg1(ctx);
	const int prot = ihk_mc_syscall_arg2(ctx);
	const int flags0 = ihk_mc_syscall_arg3(ctx);
	const int fd = ihk_mc_syscall_arg4(ctx);
	const off_t off0 = ihk_mc_syscall_arg5(ctx);
	struct thread *thread = cpu_local_var(current);
	struct vm_regions *region = &thread->vm->region;
	int error;
	intptr_t addr;
	size_t len;
	int flags = flags0;
	size_t pgsize;

	dkprintf("sys_mmap(%lx,%lx,%x,%x,%d,%lx)\n",
			addr0, len0, prot, flags0, fd, off0);

	/* check constants for flags */
	if (1) {
		int dup_flags;

		dup_flags = (supported_flags & ignored_flags);
		dup_flags |= (ignored_flags & error_flags);
		dup_flags |= (error_flags & supported_flags);

		if (dup_flags) {
			ekprintf("sys_mmap:duplicate flags: %lx\n", dup_flags);
			ekprintf("s-flags: %08x\n", supported_flags);
			ekprintf("i-flags: %08x\n", ignored_flags);
			ekprintf("e-flags: %08x\n", error_flags);
			panic("sys_mmap:duplicate flags\n");
			/* no return */
		}
	}

	/* check arguments */
	pgsize = PAGE_SIZE;
	if (flags & MAP_HUGETLB) {
		switch (flags & (0x3F << MAP_HUGE_SHIFT)) {
		case 0:
			flags |= MAP_HUGE_SECOND_BLOCK;	/* default hugepage size */
			break;

		case MAP_HUGE_SECOND_BLOCK:
		case MAP_HUGE_FIRST_BLOCK:
			break;

		default:
			ekprintf("sys_mmap(%lx,%lx,%x,%x,%x,%lx):"
					"not supported page size.\n",
					addr0, len0, prot, flags0, fd, off0);
			error = -EINVAL;
			goto out;
		}

		pgsize = (size_t)1 << ((flags >> MAP_HUGE_SHIFT) & 0x3F);
	}

#define	VALID_DUMMY_ADDR	((region->user_start + PTL3_SIZE - 1) & ~(PTL3_SIZE - 1))
	addr = (flags & MAP_FIXED)? addr0: VALID_DUMMY_ADDR;
	len = (len0 + pgsize - 1) & ~(pgsize - 1);
	if ((addr & (pgsize - 1))
			|| (len == 0)
			|| !(flags & (MAP_SHARED | MAP_PRIVATE))
			|| ((flags & MAP_SHARED) && (flags & MAP_PRIVATE))
			|| (off0 & (pgsize - 1))) {
		ekprintf("sys_mmap(%lx,%lx,%x,%x,%x,%lx):EINVAL\n",
				addr0, len0, prot, flags0, fd, off0);
		error = -EINVAL;
		goto out;
	}

	if ((flags & MAP_FIXED) && ((addr < region->user_start)
			|| (region->user_end <= addr))) {
		ekprintf("sys_mmap(%lx,%lx,%x,%x,%x,%lx):ENOMEM\n",
				addr0, len0, prot, flags0, fd, off0);
		error = -ENOMEM;
		goto out;
	}

	/* check not supported requests */
	if ((flags & error_flags)
			|| (flags & ~(supported_flags | ignored_flags))) {
		ekprintf("sys_mmap(%lx,%lx,%x,%x,%x,%lx):unknown flags %x\n",
				addr0, len0, prot, flags0, fd, off0,
				(flags & ~(supported_flags | ignored_flags)));
		error = -EINVAL;
		goto out;
	}

	addr = do_mmap(addr, len, prot, flags, fd, off0);

	error = 0;
out:
	dkprintf("sys_mmap(%lx,%lx,%x,%x,%d,%lx): %ld %lx\n",
			addr0, len0, prot, flags0, fd, off0, error, addr);
	return (!error)? addr: error;
}

SYSCALL_DECLARE(clone)
{
    return do_fork((int)ihk_mc_syscall_arg0(ctx), ihk_mc_syscall_arg1(ctx),
                   ihk_mc_syscall_arg2(ctx), ihk_mc_syscall_arg3(ctx),
                   ihk_mc_syscall_arg4(ctx), ihk_mc_syscall_pc(ctx),
                   ihk_mc_syscall_sp(ctx));
}

SYSCALL_DECLARE(shmget)
{
	const key_t key = ihk_mc_syscall_arg0(ctx);
	const size_t size = ihk_mc_syscall_arg1(ctx);
	const int shmflg0 = ihk_mc_syscall_arg2(ctx);
	int shmid;
	int error;
	int shmflg = shmflg0;

	dkprintf("shmget(%#lx,%#lx,%#x)\n", key, size, shmflg0);

	if (shmflg & SHM_HUGETLB) {
		switch (shmflg & (0x3F << SHM_HUGE_SHIFT)) {
		case 0:
			shmflg |= SHM_HUGE_SECOND_BLOCK;	/* default hugepage size */
			break;

		case SHM_HUGE_SECOND_BLOCK:
		case SHM_HUGE_FIRST_BLOCK:
			break;

		default:
			error = -EINVAL;
			goto out;
		}
	}

	shmid = do_shmget(key, size, shmflg);

	error = 0;
out:
	dkprintf("shmget(%#lx,%#lx,%#x): %d %d\n", key, size, shmflg0, error, shmid);
	return (error)?: shmid;
} /* sys_shmget() */
