// SPDX-License-Identifier: GPL-2.0
/*
 * throttle_main.c — Variabili globali condivise e init/exit del modulo
 *
 * Sequenza di init:
 *   1. Registra il device a caratteri (throttle_dev.c)
 *   2. Trova sys_call_table via page-table walk (throttle_discovery.c)
 *   3. Trova x64_sys_call via kprobe/LSTAR e la patcha (throttle_discovery.c)
 *      [solo kernel >= 5.15; non fatale se la ricerca fallisce]
 *   4. Avvia l'hrtimer 1 Hz (throttle_core.c)
 */

#include "throttle.h"

/* ================================================================
 *  STATO GLOBALE CONDIVISO
 * ================================================================ */

 //Macro definizione delle teste di lista
LIST_HEAD(prog_list);
LIST_HEAD(uid_list);
LIST_HEAD(syscall_list);

int  monitor_enabled  = 0;
int  max_calls        = DEFAULT_MAX;
int  module_unloading = 0;
DEFINE_SPINLOCK(config_lock);

atomic_t call_count = ATOMIC_INIT(0);
//wait queue per le code di attesa in throttle_check
DECLARE_WAIT_QUEUE_HEAD(throttle_wq);

/* Drain protocol per rmmod pulito:
 * active_threads_in_wrapper conta i thread attualmente dentro generic_sct_wrapper.
 * unload_wq viene svegliata quando scende a 0 durante il module unloading. */
atomic_t          active_threads_in_wrapper = ATOMIC_INIT(0);
DECLARE_WAIT_QUEUE_HEAD(unload_wq);

static struct hrtimer window_timer;

/* ================================================================
 *  INIT / EXIT
 * ================================================================ */

 // Funzione di init del modulo: registra device, trova sys_call_table e x64_sys_call, avvia timer
static int __init throttle_init(void)
{
    int ret;

    // Registra il device a caratteri (throttle_dev.c)
    ret = throttle_dev_register();
    if (ret < 0) return ret;

    printk(KERN_INFO "<throttle>: caricato | major=%d | MAX=%d\n", ret, max_calls);
    printk(KERN_INFO "<throttle>: mknod /dev/%s c %d 0\n", DEVICE_NAME, ret);

    ret = find_sys_call_table();
    if (ret) {
        throttle_dev_unregister();
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
    /* Trova x64_sys_call via LSTAR scan e la patcha con JMP → stub esterno.
     * Necessario su kernel >= 5.15 dove il dispatch usa x64_sys_call()
     * anziché sys_call_table direttamente. */
    if (!find_x64_sys_call()) {
        if (patch_x64_sys_call())
            printk(KERN_WARNING "<throttle>: patch x64_sys_call fallita (non fatale)\n");
    } else {
        printk(KERN_WARNING "<throttle>: x64_sys_call non trovata (non fatale)\n");
    }
#endif

    throttle_core_start(&window_timer);
    return 0;
}

//Funzione di cleanup del modulo: rimuove hook, drena i thread in-flight, cancella timer, pulisce liste
static void __exit throttle_exit(void)
{
    struct prog_node    *pcursor, *ptmp;
    struct uid_node     *ucursor, *utmp;
    struct syscall_node *scursor, *stmp;
    unsigned long flags;

    printk(KERN_INFO "<throttle>: exit [1] inizio\n");

    /* [2] Segnala l'unloading: throttle_check uscirà dai suoi loop di attesa */
    smp_store_release(&module_unloading, 1);
    monitor_enabled = 0;
    wake_up_all(&throttle_wq);
    printk(KERN_INFO "<throttle>: exit [2] unloading segnalato\n");

    /* [3] Rimuove gli hook SCT: nuove syscall vanno all'handler originale.
     * I thread già dentro generic_sct_wrapper continuano fino al drain. */
    remove_all_hooks();
    printk(KERN_INFO "<throttle>: exit [3] hooks SCT rimossi\n");

    /* [4] Quiescence 1: garantisce che CPU con il vecchio ptr SCT abbiano
     * avuto il tempo di entrare in generic_sct_wrapper (e incrementare
     * active_threads_in_wrapper) prima che controlliamo il contatore. */
    synchronize_rcu();

    /* [5] Drain: aspetta che tutti i thread in-flight escano dal wrapper */
    wait_event(unload_wq, atomic_read(&active_threads_in_wrapper) == 0);
    printk(KERN_INFO "<throttle>: exit [5] drain completato\n");

    /* [6] Quiescence 2: barriera finale su tutti i CPU */
    synchronize_rcu();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
    /* [7] Ripristina x64_sys_call e libera lo stub esterno. */
    restore_x64_sys_call();
    printk(KERN_INFO "<throttle>: exit [7] x64_sys_call ripristinata\n");
#endif

    hrtimer_cancel(&window_timer);
    printk(KERN_INFO "<throttle>: exit [8] timer cancellato\n");

    spin_lock_irqsave(&config_lock, flags);
    list_for_each_entry_safe(pcursor, ptmp, &prog_list, list)
        { list_del(&pcursor->list); kfree(pcursor); }
    list_for_each_entry_safe(ucursor, utmp, &uid_list, list)
        { list_del(&ucursor->list); kfree(ucursor); }
    list_for_each_entry_safe(scursor, stmp, &syscall_list, list)
        { list_del(&scursor->list); kfree(scursor); }
    spin_unlock_irqrestore(&config_lock, flags);

    throttle_dev_unregister();
    printk(KERN_INFO "<throttle>: rimosso\n");
}

module_init(throttle_init);
module_exit(throttle_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Syscall throttling LKM — hardware-only discovery");
MODULE_AUTHOR("Luca");
