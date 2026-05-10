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


 //NOTA BENE
 //orig_fn puntatore alla syscall originale
 //orig_ax numero syscall (sarebbe il valore del registro rax)
 
 
#include "throttle.h"

struct hook_entry {
    //nr della syscall hookata, handler originale salvato, flag di attivazione
    int           nr;
    syscall_fn_t  orig_fn;
    int           active;
};

static struct hook_entry hooks[MAX_HOOKED_SYSCALLS];
static DEFINE_MUTEX(hook_mutex);

/*
generic_sct_wrapper — unico handler per tutte le syscall hookate.
N.B. Non usa try_module_get (refcount rimane a 0) permettendo rmmod
Il problema dei thread attivi è gestito da active_threads_in_wrapper + unload_wq in throttle_main.c. 
 */
static asmlinkage long generic_sct_wrapper(const struct pt_regs *regs)
{
    int nr = (int)regs->orig_ax;
    syscall_fn_t orig_fn = NULL;
    long ret;
    int i;
    //tiene traccia di n. thread attivi dentro al mio wrapper (servirà nella parte di unload)
    atomic_inc(&active_threads_in_wrapper);

    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++) {
        if (hooks[i].active && hooks[i].nr == nr) {
            orig_fn = hooks[i].orig_fn;
            break;
        }
    }

    /*NOTA BENE 
    * le successive righe risolvono la condizione di parallelismo in cui: 
    * un thread entra in generic_sct_wrapper
    * mentre un altro thread sta rimuovendo l'hook
    */

    //se orig_fn null allora la syscall non è più hookata (es. è stata rimossa mentre eravamo in throttle_check)
    if (!orig_fn) {
        ret = (sys_call_table_ptr && nr < NR_syscalls)
        //controlla se sys_call_table_ptr è valido e nr è un numero di syscall valido, se sì chiama direttamente la syscall originale senza wrapper, altrimenti ritorna -ENOSYS
              ? sys_call_table_ptr[nr](regs)
              : -ENOSYS;
        if (atomic_dec_and_test(&active_threads_in_wrapper) &&
            smp_load_acquire(&module_unloading))
            wake_up(&unload_wq);
        return ret;
    }

    //check se può essere eseguito
    throttle_check(nr);

    /* Il drain segnala completamento DOPO orig_fn: il decremento deve avvenire
     * dopo la chiamata per evitare che throttle_exit liberi la memoria del modulo
     * mentre siamo ancora dentro generic_sct_wrapper (use-after-free).
    */
    ret = orig_fn(regs);
    if (atomic_dec_and_test(&active_threads_in_wrapper) &&
        smp_load_acquire(&module_unloading))
        wake_up(&unload_wq);
    return ret;
}

// Installa un hook sulla syscall nr: sostituisce sys_call_table[nr] con generic_sct_wrapper e salva l'handler originale in hooks[]. Ritorna 0 se successo, -ENODEV se sys_call_table non trovata, -ENOMEM se non ci sono slot liberi per nuovi hook.
int install_hook(int nr)
{
    //contesto di processo perchè chiamato dall'ioctl_handler : può dormire -> mutex
    int i;

    if (!sys_call_table_ptr) return -ENODEV;

    mutex_lock(&hook_mutex);

    // Controlla se la syscall è già hookata    
    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++)
        if (hooks[i].active && hooks[i].nr == nr) {
            mutex_unlock(&hook_mutex);
            return 0;
        }

    // Cerca uno slot libero per il nuovo hook
    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++)
        if (!hooks[i].active) break;
    if (i == MAX_HOOKED_SYSCALLS) {
        mutex_unlock(&hook_mutex);
        return -ENOMEM;
    }

    // Installa l'hook: salva l'handler originale, scrive generic_sct_wrapper sulla sys_call_table, marca l'hook come attivo
    hooks[i].nr      = nr;
    hooks[i].orig_fn = sys_call_table_ptr[nr];
    hooks[i].active  = 1;

    // Scrittura sulla sys_call_table protetta da begin/end_syscall_table_hack, con memory barrier per garantire visibilità su altri core
    begin_syscall_table_hack();
    sys_call_table_ptr[nr] = generic_sct_wrapper;
    end_syscall_table_hack();

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
            hooks[i].active = 0;
            printk(KERN_INFO "<throttle>: [HOOK-] nr=%d ripristinato\n", nr);
            break;
        }
    }
    mutex_unlock(&hook_mutex);
}


// Rimuove tutti gli hook attivi, usata durante unload per assicurare che la sys_call_table sia ripristinata allo stato originale.
void remove_all_hooks(void)
{
    int i;
    for (i = 0; i < MAX_HOOKED_SYSCALLS; i++)
        if (hooks[i].active)
            remove_hook(hooks[i].nr);
}
