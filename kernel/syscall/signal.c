#include "kernel/asm/stub.h"
#include "kernel/asmFunc.h"
#include "kernel/assert.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/mm/access.h"
#include "kernel/syscall_nr.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "arch/x86/interrupt/interrupt.h"
#include "kernel/sched/thread.h"
static const uint8_t sig_default[NSIG] = {
    [SIGHUP] = SIG_ACT_TERM,  [SIGINT] = SIG_ACT_TERM,
    [SIGQUIT] = SIG_ACT_TERM, [SIGILL] = SIG_ACT_TERM,
    [SIGTRAP] = SIG_ACT_TERM, [SIGABRT] = SIG_ACT_TERM,
    [SIGBUS] = SIG_ACT_TERM,  [SIGFPE] = SIG_ACT_TERM,
    [SIGKILL] = SIG_ACT_TERM, [SIGUSR1] = SIG_ACT_TERM,
    [SIGSEGV] = SIG_ACT_TERM, [SIGUSR2] = SIG_ACT_TERM,
    [SIGPIPE] = SIG_ACT_TERM, [SIGALRM] = SIG_ACT_TERM,
    [SIGTERM] = SIG_ACT_TERM, [SIGCHLD] = SIG_ACT_IGN,
    [SIGCONT] = SIG_ACT_CONT, [SIGSTOP] = SIG_ACT_STOP,
    [SIGTSTP] = SIG_ACT_STOP, [SIGTTIN] = SIG_ACT_STOP,
    [SIGTTOU] = SIG_ACT_STOP, [SIGSYS] = SIG_ACT_TERM,
};
void init_signal_state(struct task_struct *t) {
    t->signal_pending = 0;
    t->signal_mask = 0;
    for (int i = 0; i < NSIG; i++) {
        t->sigactions[i].sa_handler = SIG_DFL;
        t->sigactions[i].sa_mask = 0;
        t->sigactions[i].sa_flags = 0;
        t->sigactions[i].sa_restorer = NULL;
    }
}
void signal_reset_user(struct task_struct *t) {
    init_signal_state(t);
}
int exception_to_signal(int int_no) {
    switch (int_no) {
    case 0:
        return SIGFPE;
    case 6:
        return SIGILL;
    case 13:
        return SIGSEGV;
    case 14:
        return SIGSEGV;
    case 16:
        return SIGFPE;
    case 19:
        return SIGFPE;
    default:
        return 0;
    }
}
void signal_terminate(struct task_struct *t, int sig) {
    proc_exit(t, 128 + sig);
}
static void signal_stop_current(void) {
    thread_block_with_status(TASK_STOPPED);
}
struct sigframe64 {
    uint64_t restorer;
    uint64_t signo;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t old_mask;
};
static int sigframe_valid(uint64_t cs, uint64_t rip, uint64_t rsp,
                          uint64_t ss, uint64_t rflags) {
    if (cs != SELECTOR_USER64_CODE && cs != SELECTOR_U_CODE) {
        return 0;
    }
    if (ss != SELECTOR_U_DATA) {
        return 0;
    }
    if (rip < USER_VADDR_START || rip >= USER_SPACE_END) {
        return 0;
    }
    if (rsp < USER_VADDR_START || rsp >= USER_SPACE_END) {
        return 0;
    }
    if ((rflags >> 32) != 0 || (rflags & 0x1AF028ull) != 0 ||
        (rflags & 0x202ull) != 0x202ull) {
        return 0;
    }
    return 1;
}
static void deliver_signal64(struct task_struct *cur, struct Registers *r,
                             int sig, struct sigaction *sa) {
    struct sigframe64 frame;
    frame.restorer = (uint64_t)sa->sa_restorer;
    frame.signo = (uint64_t)sig;
    frame.rip = r->rip;
    frame.cs = r->cs;
    frame.rflags = r->rflags & ~(1ull << 8);
    frame.rsp = r->user_rsp;
    frame.ss = r->ss;
    frame.rax = r->rax;
    frame.rbx = r->rbx;
    frame.rcx = r->rcx;
    frame.rdx = r->rdx;
    frame.rsi = r->rsi;
    frame.rdi = r->rdi;
    frame.rbp = r->rbp;
    frame.r8 = r->r8;
    frame.r9 = r->r9;
    frame.r10 = r->r10;
    frame.r11 = r->r11;
    frame.r12 = r->r12;
    frame.r13 = r->r13;
    frame.r14 = r->r14;
    frame.r15 = r->r15;
    frame.old_mask = cur->signal_mask;
    uint64_t sp = r->user_rsp - 128;
    sp -= sizeof(struct sigframe64);
    sp &= ~0xfULL;
    sp -= 8;
    uint32_t stack_low =
        (cur->stack_bottom != 0) ? cur->stack_bottom : USER_STACK_BOTTOM;
    if (sp < stack_low) {
        signal_terminate(cur, sig);
        return;
    }
    memcpy((void *)sp, &frame, sizeof(frame));
    if (!(sa->sa_flags & SA_NODEFER)) {
        cur->signal_mask |= (1u << sig);
    }
    cur->signal_mask |= sa->sa_mask;
    cur->signal_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
    r->user_rsp = sp;
    r->rip = (uint64_t)sa->sa_handler;
    r->rdi = (uint64_t)sig;
    r->rax = 0;
}
static void deliver_signal(struct task_struct *cur, struct Registers *r,
                           int sig, struct sigaction *sa) {
    if (r->cs == SELECTOR_USER64_CODE) {
        deliver_signal64(cur, r, sig, sa);
        return;
    }
    struct sigframe frame;
    frame.restorer = (uint32_t)sa->sa_restorer;
    frame.signo = (uint32_t)sig;
    frame.eip = r->eip;
    frame.cs = r->cs;
    frame.eflags = r->eflags & ~(1u << 8);
    frame.user_esp = r->user_esp;
    frame.ss = r->ss;
    frame.eax = r->eax;
    frame.ebx = r->ebx;
    frame.ecx = r->ecx;
    frame.edx = r->edx;
    frame.esi = r->esi;
    frame.edi = r->edi;
    frame.ebp = r->ebp;
    frame.old_mask = cur->signal_mask;
    uint32_t frame_size = sizeof(struct sigframe);
    uint32_t new_esp = (r->user_esp - frame_size) & ~3u;
    uint32_t stack_low =
        (cur->stack_bottom != 0) ? cur->stack_bottom : USER_STACK_BOTTOM;
    if (new_esp < stack_low) {
        signal_terminate(cur, sig);
    }
    memcpy((void *)new_esp, &frame, frame_size);
    if (!(sa->sa_flags & SA_NODEFER)) {
        cur->signal_mask |= (1u << sig);
    }
    cur->signal_mask |= sa->sa_mask;
    cur->signal_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
    r->user_esp = new_esp;
    r->eip = (uint32_t)sa->sa_handler;
    r->eax = (uint32_t)sig;
}
void check_pending_signals(struct Registers *r) {
    struct task_struct *cur = current;
    if (cur == NULL) {
        return;
    }
    if ((r->cs & 3) != 3) {
        return;
    }
    if (cur->signal_pending == 0) {
        return;
    }
    for (;;) {
        uint32_t deliverable = cur->signal_pending & ~cur->signal_mask;
        if (deliverable == 0) {
            break;
        }
        int sig = __builtin_ctz(deliverable);
        if (sig >= NSIG) {
            cur->signal_pending = 0;
            break;
        }
        cur->signal_pending &= ~(1u << sig);
        struct sigaction *sa = &cur->sigactions[sig];
        void (*handler)(int) = sa->sa_handler;
        if (sig == SIGKILL) {
            signal_terminate(cur, sig);
        }
        if (sig == SIGSTOP) {
            signal_stop_current();
            continue;
        }
        if (handler == SIG_DFL) {
            uint8_t act = sig_default[sig];
            if (act == SIG_ACT_IGN) {
                continue;
            } else if (act == SIG_ACT_STOP) {
                signal_stop_current();
                continue;
            } else if (act == SIG_ACT_CONT) {
                continue;
            } else {
                signal_terminate(cur, sig);
            }
        } else if (handler == SIG_IGN) {
            continue;
        }
        deliver_signal(cur, r, sig, sa);
        return;
    }
}
int sys_sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig < 1 || sig >= NSIG) {
        return -1;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -1;
    }
    if (old) {
        memcpy((void *)old, &current->sigactions[sig],
               sizeof(struct sigaction));
    }
    if (act) {
        memcpy(&current->sigactions[sig], (const void *)act,
               sizeof(struct sigaction));
    }
    return 0;
}
int sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (oldset) {
        *oldset = current->signal_mask;
    }
    if (set) {
        sigset_t s = *set;
        if (how == SIG_BLOCK) {
            current->signal_mask |= s;
        } else if (how == SIG_UNBLOCK) {
            current->signal_mask &= ~s;
        } else if (how == SIG_SETMASK) {
            current->signal_mask = s;
        } else {
            return -1;
        }
        current->signal_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
    }
    return 0;
}
static int sig_default_terminates(struct task_struct *t, int sig) {
    if (sig == SIGKILL) {
        return 1;
    }
    void (*handler)(int) = t->sigactions[sig].sa_handler;
    if (handler == SIG_IGN) {
        return 0;
    }
    if (handler == SIG_DFL) {
        return sig_default[sig] == SIG_ACT_TERM;
    }
    return 0;
}
int sys_kill(int pid, int sig) {
    if (pid < 0) {
        uint32_t pgid = (uint32_t)(-pid);
        int n = 0;
        for (uint32_t i = 0; i < MAX_TASKS; i++) {
            struct task_struct *t = &task_table[i];
            if (!t->slot_used || t->status == TASK_DIED)
                continue;
            if (t->pid != pgid)
                continue;
            thread_kill_pid(t->pid);
            t->signal_pending |= (1u << (sig & 31));
            n++;
        }
        return n ? 0 : -1;
    }

    if (sig < 0 || sig >= NSIG) {
        return -1;
    }
    if (pid == 0) {
        pid = (int)current->pid;
    }
    struct task_struct *t = pid2thread(pid);
    if (t == NULL) {
        return -1;
    }
    if (sig == 0) {
        return 0;
    }
    if (sig == SIGKILL) {
        thread_kill_pid((uint32_t)pid);
        return 0;
    }
    if (sig == SIGCONT && t->status == TASK_STOPPED) {
        thread_ready(t);
        t->signal_pending &= ~(1u << SIGCONT);
        return 0;
    }
    t->signal_pending |= (1u << sig);
    if (t->status == TASK_WAITING) {
        thread_ready(t);
    } else if (t->status & (TASK_BLOCKED | TASK_STOPPED)) {
        if (sig_default_terminates(t, sig)) {
            t->signal_pending &= ~(1u << sig);
            thread_kill_pid((uint32_t)pid);
        }
    }
    return 0;
}
uint64_t sys_sigreturn(struct Registers *r) {
    struct task_struct *cur = current;
    if (r->cs == SELECTOR_USER64_CODE) {
        uint64_t faddr = r->user_rsp - 8;
        if (faddr < USER_VADDR_START ||
            faddr > USER_SPACE_END - sizeof(struct sigframe64) ||
            !user_range_readable((uint32_t)faddr,
                                 sizeof(struct sigframe64))) {
            signal_terminate(cur, SIGSEGV);
            return (uint64_t)-1;
        }
        struct sigframe64 *sf = (struct sigframe64 *)faddr;
        if (!sigframe_valid(sf->cs, sf->rip, sf->rsp, sf->ss, sf->rflags)) {
            signal_terminate(cur, SIGSEGV);
            return (uint64_t)-1;
        }
        r->rip = sf->rip;
        r->cs = sf->cs;
        r->rflags = sf->rflags;
        r->user_rsp = sf->rsp;
        r->ss = sf->ss;
        r->rax = sf->rax;
        r->rbx = sf->rbx;
        r->rcx = sf->rcx;
        r->rdx = sf->rdx;
        r->rsi = sf->rsi;
        r->rdi = sf->rdi;
        r->rbp = sf->rbp;
        r->r8 = sf->r8;
        r->r9 = sf->r9;
        r->r10 = sf->r10;
        r->r11 = sf->r11;
        r->r12 = sf->r12;
        r->r13 = sf->r13;
        r->r14 = sf->r14;
        r->r15 = sf->r15;
        cur->signal_mask = sf->old_mask;
        cur->signal_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
        return sf->rax;
    }
    uint64_t faddr = r->user_esp - 4;
    if (faddr < USER_VADDR_START ||
        faddr > USER_SPACE_END - sizeof(struct sigframe) ||
        !user_range_readable((uint32_t)faddr, sizeof(struct sigframe))) {
        signal_terminate(cur, SIGSEGV);
        return (uint64_t)-1;
    }
    struct sigframe *sf = (struct sigframe *)faddr;
    if (!sigframe_valid(sf->cs, sf->eip, sf->user_esp, sf->ss, sf->eflags)) {
        signal_terminate(cur, SIGSEGV);
        return (uint64_t)-1;
    }
    r->eip = sf->eip;
    r->cs = sf->cs;
    r->eflags = sf->eflags;
    r->user_esp = sf->user_esp;
    r->ss = sf->ss;
    r->eax = sf->eax;
    r->ebx = sf->ebx;
    r->ecx = sf->ecx;
    r->edx = sf->edx;
    r->esi = sf->esi;
    r->edi = sf->edi;
    r->ebp = sf->ebp;
    cur->signal_mask = sf->old_mask;
    cur->signal_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
    return sf->eax;
}

void itimer_tick(void) {
    uint32_t now = tick;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct task_struct *t = &task_table[i];
        if (!t->slot_used || t->status == TASK_DIED || t->itimer_expire == 0) {
            continue;
        }
        if (now < t->itimer_expire) {
            continue;
        }
        t->signal_pending |= (1u << SIGALRM);
        t->itimer_expire = t->itimer_interval ? now + t->itimer_interval : 0;
        if (t->status == TASK_WAITING) {
            thread_ready(t);
        }
    }
}

