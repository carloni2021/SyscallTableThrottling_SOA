// SPDX-License-Identifier: GPL-2.0
/*
 * throttle_hook.c — Installazione e rimozione di hook sulla sys_call_table
 *
 * generic_sct_wrapper: handler unico per tutte le syscall hookate.
 *   Legge regs->orig_ax per il numero di syscall, esegue throttle_check(),
 *   poi chiama l'handler originale salvato in hooks[].
 *
 * install_hook / remove_hook: scrivono sulla sys_call_table (tramite
 *   begin/end_syscall_table_hack) per sostituire l'handler originale
 *   con generic_sct_wrapper e viceversa.
 */

#include "throttle.h"

struct hook_entry {
    int           nr;
    syscall_fn_t  orig_fn;
    int           active;
};

static struct hook_entry hooks[MAX_HOOKED_SYSCALLS];
static DEFINE_MUTEX(hook_mutex);

/*
 * generic_sct_wrapper — unico handler per tutte le syscall hookate.
 *
 * Non usa try_module_get: il refcount del modulo rimane 0, permettendo a rmmod
 * di chiamare throttle_exit anche con thread bloccati in throttle_check.
 * Il drain è gestito da active_threads_in_wrapper + unload_wq in throttle_main.c.
 *
 * Race durante unload: se l'hook viene rimosso mentre questo thread era già
 * in transito verso generic_sct_wrapper (aveva letto il vecchio ptr SCT),
 * hooks[i].active sarà 0 ma sys_call_table_ptr[nr] punta già all'handler originale.
 * In quel caso lo chiamiamo direttamente.
 */
static asmlinkage long generic_sct_wrapper(const struct pt_regs *regs)
{
    int nr = (int)regs->orig_ax;
    syscall_fn_t orig_fn = NULL;
    long ret;
    int i;

    atomic_inc(&active_threads_in_wrapper);

    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++) {
        if (hooks[i].active && hooks[i].nr == nr) {
            orig_fn = hooks[i].orig_fn;
            break;
        }
    }

    if (!orig_fn) {
        /* Hook rimosso durante il transito (race di unload): chiama l'handler
         * originale direttamente dalla tabella, che è già stata ripristinata. */
        ret = (sys_call_table_ptr && nr < NR_syscalls)
              ? sys_call_table_ptr[nr](regs)
              : -ENOSYS;
        if (atomic_dec_and_test(&active_threads_in_wrapper) &&
            smp_load_acquire(&module_unloading))
            wake_up(&unload_wq);
        return ret;
    }

    throttle_check(nr);

    if (atomic_dec_and_test(&active_threads_in_wrapper) &&
        smp_load_acquire(&module_unloading))
        wake_up(&unload_wq);
    return orig_fn(regs);
}

int install_hook(int nr)
{
    int i;

    if (!sys_call_table_ptr) return -ENODEV;

    mutex_lock(&hook_mutex);

    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++)
        if (hooks[i].active && hooks[i].nr == nr) {
            mutex_unlock(&hook_mutex);
            return 0;
        }

    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++)
        if (!hooks[i].active) break;
    if (i == MAX_HOOKED_SYSCALLS) {
        mutex_unlock(&hook_mutex);
        return -ENOMEM;
    }

    hooks[i].nr      = nr;
    hooks[i].orig_fn = sys_call_table_ptr[nr];
    hooks[i].active  = 1;

    begin_syscall_table_hack();
    sys_call_table_ptr[nr] = generic_sct_wrapper;
    end_syscall_table_hack();
    mb();

    printk(KERN_INFO "<throttle>: [HOOK+] nr=%d orig=%px\n",
           nr, (void *)hooks[i].orig_fn);
    mutex_unlock(&hook_mutex);
    return 0;
}

//rimozione di un hook: ripristina l'handler originale sulla sys_call_table e marca l'hook come non attivo
void remove_hook(int nr)
{
    int i;
    mutex_lock(&hook_mutex);
    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++) {
        if (hooks[i].active && hooks[i].nr == nr) {
            //ripristino dell'handler originale sulla sys_call_table
            begin_syscall_table_hack();
            sys_call_table_ptr[nr] = hooks[i].orig_fn;
            end_syscall_table_hack();
            mb();
            hooks[i].active = 0;
            printk(KERN_INFO "<throttle>: [HOOK-] nr=%d ripristinato\n", nr);
            break;
        }
    }
    mutex_unlock(&hook_mutex);
}

void remove_all_hooks(void)
{
    int i;
    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++)
        if (hooks[i].active)
            remove_hook(hooks[i].nr);
}
