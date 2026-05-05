/*
 * throttle_core.c — Motore di throttling
 *
 * get_caller_exe_info: risolve inode/device/nome dell'eseguibile del processo
 *   chiamante tramite mm->exe_file.
 *
 * caller_is_registered / syscall_is_registered: verificano se il processo
 *   corrente e la syscall invocata sono nel set da monitorare.
 *
 * window_timer_fn: callback hrtimer 1 Hz che azzera call_count, aggiorna
 *   le statistiche sui thread bloccati e sveglia throttle_wq.
 *
 * throttle_check: punto di ingresso dal wrapper di hook; blocca il chiamante
 *   su throttle_wq se call_count supera max_calls, aggiorna peak_delay.
 */

#include "throttle.h"

//  STATISTICHE
/* Per semplicità, tutte le statistiche sono aggiornate sotto un unico lock (stats_lock) per evitare complessità di sincronizzazione. 
*  In un'implementazione più sofisticata, si potrebbero usare strutture dati separate e lock più granulari per ridurre la contesa.
*/
long long  peak_delay_ns                    = 0;
char       peak_delay_prog[TASK_COMM_LEN]   = {0};
uid_t      peak_delay_uid                   = 0;
atomic_t   current_blocked                  = ATOMIC_INIT(0);
long       peak_blocked                     = 0;
long long  total_blocked_sum                = 0;
long       total_blocked_count              = 0;
//  LOCKS
DEFINE_SPINLOCK(stats_lock);


//  HELPER 

/* Risolve inode/device/nome dell'eseguibile del processo chiamante per identificazione e logging. Se mm o exe_file non sono disponibili, usa comm.
*  Perchè mm->exe_file? Perché è più affidabile di comm: due processi con lo stesso nome (es. "bash") possono essere distinti se hanno eseguibili 
*  diversi. Inoltre, comm può essere modificato da un processo, mentre exe_file riflette l'effettivo binario in esecuzione.
*  Nota: questa funzione è chiamata ad ogni syscall monitorata, quindi è ottimizzata per il caso comune (mm->exe_file valido) e minimizza la durata 
*  del lock mmap_read_lock.
*/
static void get_caller_exe_info(unsigned long *ino, dev_t *dev,
                                char *name_buf, int buf_len)
{
    struct mm_struct *mm = current->mm;
    *ino = 0; *dev = 0;
    if (!mm) {
        if (name_buf) strscpy(name_buf, current->comm, buf_len);
        return;
    }
    mmap_read_lock(mm);
    if (mm->exe_file) {
        struct inode *ei = file_inode(mm->exe_file);
        *ino = ei->i_ino;
        *dev = ei->i_sb->s_dev;
        if (name_buf)
            strscpy(name_buf,
                    kbasename(mm->exe_file->f_path.dentry->d_name.name),
                    buf_len);
    } else {
        if (name_buf) strscpy(name_buf, current->comm, buf_len);
    }
    mmap_read_unlock(mm);
}

/* Verifica se il processo corrente è nel set monitorato (per inode/device o euid).
 * hash_for_each_possible itera solo il bucket della chiave → O(1) atteso. */
static int caller_is_registered(unsigned long ino, dev_t dev, uid_t euid)
{
    struct prog_node *pnode;
    struct uid_node  *unode;
    if (ino) {
        hash_for_each_possible(prog_table, pnode, hnode, ino)
            if (pnode->inode == ino && pnode->device == dev) return 1;
    }
    hash_for_each_possible(uid_table, unode, hnode, euid)
        if (unode->uid == euid) return 1;
    return 0;
}

/* Verifica se la syscall nr è nel set monitorato — O(1) con test_bit. */
static int syscall_is_registered(int nr)
{
    return test_bit(nr, syscall_bitmap);
}

/* ================================================================
 *  HRTIMER — finestra 1 Hz
 * ================================================================ */


/*  Hrtimer supporta sia CLOCK_MONOTONIC che CLOCK_REALTIME, ma usiamo MONOTONIC per evitare problemi di salto temporale.
*   La funzione window_timer_fn viene chiamata ogni secondo e azzera call_count, aggiorna le statistiche sui thread bloccati e sveglia throttle_wq per sbloccare
*   eventuali thread in attesa.
*/
static enum hrtimer_restart window_timer_fn(struct hrtimer *timer)
{
    /*Nota che hrtimer è già in un contesto di interrupt, quindi non possiamo fare sleep o operazioni bloccanti. Per questo usiamo spin_lock_irqsave e wake_up_all.
    inoltre hrimter è altamente preciso, quindi non c'è rischio di drift significativo nel timer.*/

    unsigned long flags;
    atomic_set(&call_count, 0);
    if (READ_ONCE(monitor_enabled)) {
        spin_lock_irqsave(&stats_lock, flags);
        {
            long blocked = atomic_read(&current_blocked);
            total_blocked_sum   += blocked;
            total_blocked_count += 1;
            if (blocked > peak_blocked) peak_blocked = blocked;
        }
        spin_unlock_irqrestore(&stats_lock, flags);
    }
    /* Sveglia al più max_calls thread: tanti quanti i slot disponibili nella nuova finestra.
     * wake_up_all causerebbe thundering herd (tutti i thread bloccati si svegliano
     * e quasi tutti tornano subito a dormire); wake_up_nr limita il risveglio ai soli
     * thread che hanno ragionevole probabilità di ottenere un slot. */
    wake_up_nr(&throttle_wq, READ_ONCE(max_calls));
    hrtimer_forward_now(timer, ktime_set(1, 0));
    return HRTIMER_RESTART;
}

void throttle_core_start(struct hrtimer *timer)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
    hrtimer_setup(timer, window_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
    hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    timer->function = window_timer_fn;
#endif
    hrtimer_start(timer, ktime_set(1, 0), HRTIMER_MODE_REL);
}

/* ================================================================
 *  THROTTLE CHECK
 * ================================================================ */

 // Punto di ingresso dal wrapper di hook; blocca il chiamante su throttle_wq se call_count supera max_calls, aggiorna peak_delay.
void throttle_check(int nr)
{
    char          comm[TASK_COMM_LEN];
    unsigned long exe_ino;
    dev_t         exe_dev;
    uid_t         euid;
    unsigned long flags;
    int           registered, count;
    ktime_t       enter_time, exit_time;
    long long     delay_ns;

    if (!READ_ONCE(monitor_enabled)) return;

    get_caller_exe_info(&exe_ino, &exe_dev, comm, TASK_COMM_LEN);
    euid = from_kuid(&init_user_ns, current_euid());

    spin_lock_irqsave(&config_lock, flags);
    registered = syscall_is_registered(nr) &&
                 caller_is_registered(exe_ino, exe_dev, euid);
    spin_unlock_irqrestore(&config_lock, flags);

    if (!registered) return;

    count = atomic_inc_return(&call_count);
    if (count <= max_calls) return;

    atomic_dec(&call_count);
    enter_time = ktime_get();
    atomic_inc(&current_blocked);

    printk(KERN_DEBUG "<throttle>: BLOCKED prog='%s' euid=%u nr=%d max=%d\n",
           comm, euid, nr, max_calls);

    {
        int ret;
        while (1) {
            /* READ_ONCE: necessario perché module_unloading è scritto con
             * smp_store_release; senza READ_ONCE il compilatore potrebbe
             * cachare il valore in un registro e non vedere mai l'aggiornamento. */
            /* _exclusive: registra il thread come waiter esclusivo, così
             * wake_up_nr(max_calls) sveglia esattamente max_calls thread
             * invece di tutti (thundering herd). */
            ret = wait_event_interruptible_exclusive(throttle_wq,
                      atomic_read(&call_count) < max_calls ||
                      !monitor_enabled || READ_ONCE(module_unloading));
            if (ret != 0 || !monitor_enabled || READ_ONCE(module_unloading)) break;
            count = atomic_inc_return(&call_count);
            if (count <= max_calls) break;
            atomic_dec(&call_count);
        }
    }

    atomic_dec(&current_blocked);
    exit_time = ktime_get();
    delay_ns  = ktime_to_ns(ktime_sub(exit_time, enter_time));

    spin_lock_irqsave(&stats_lock, flags);
    if (delay_ns > peak_delay_ns) {
        peak_delay_ns  = delay_ns;
        peak_delay_uid = euid;
        strscpy(peak_delay_prog, comm, TASK_COMM_LEN);
    }
    spin_unlock_irqrestore(&stats_lock, flags);
}
