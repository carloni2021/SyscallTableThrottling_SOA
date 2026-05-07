/* SPDX-License-Identifier: GPL-2.0 */
#ifndef THROTTLE_H
#define THROTTLE_H

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/bitmap.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/uidgid.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/syscalls.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <asm/processor-flags.h>
#include <asm/special_insns.h>
#include <asm/msr.h>
#include <asm/io.h>
#include <linux/stop_machine.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
#include <linux/moduleloader.h> /* module_alloc / vfree */
#endif

/* ================================================================
 *  COSTANTI
 * ================================================================ */

#define DEVICE_NAME         "throttleDriver"
#define TASK_COMM_LEN       16
#define MAX_HOOKED_SYSCALLS 32
#define PROG_PATH_MAX       256
#define DEFAULT_MAX         100

/* Valore sentinella ritornato da sys_vtpmo quando un indirizzo non è mappato */
#define NO_MAP (-1)

/* Maschera pagina fisica — usata in vtpmo e nel finder della SCT */
#define PT_PHYS_MASK 0xfffffffffffff000ULL

/* ================================================================
 *  INTERFACCIA IOCTL
 * ================================================================ */

#define IOCTL_ADD_PROG      _IOW('T',  1, char[PROG_PATH_MAX])
#define IOCTL_DEL_PROG      _IOW('T',  2, char[PROG_PATH_MAX])
#define IOCTL_ADD_UID       _IOW('T',  4, unsigned int)
#define IOCTL_DEL_UID       _IOW('T',  5, unsigned int)
#define IOCTL_ADD_SYSCALL   _IOW('T',  7, int)
#define IOCTL_DEL_SYSCALL   _IOW('T',  8, int)
#define IOCTL_SET_MONITOR   _IOW('T', 10, int)

struct throttle_status {
    int monitor_enabled;
    int max_calls;
    int current_count;
};
#define IOCTL_GET_STATUS    _IOR('T', 11, struct throttle_status)
#define IOCTL_SET_MAX       _IOW('T', 12, int)

struct throttle_stats {
    long long peak_delay_ns;
    char      peak_delay_prog[TASK_COMM_LEN];
    uid_t     peak_delay_uid;
    long      avg_blocked_threads;
    long      peak_blocked_threads;
    long      peak_calls_per_window;
    long      avg_calls_per_window;
};
#define IOCTL_GET_STATS     _IOR('T', 13, struct throttle_stats)
#define IOCTL_RESET_STATS   _IO ('T', 14)

#define MAX_REG_PROGS  32
#define MAX_REG_UIDS   32

struct throttle_prog_list {
    int  count;
    char paths[MAX_REG_PROGS][PROG_PATH_MAX];
};
struct throttle_uid_list {
    int          count;
    unsigned int uids[MAX_REG_UIDS];
};
struct throttle_syscall_list {
    int count;
    int nrs[MAX_HOOKED_SYSCALLS];
};

#define IOCTL_LIST_PROGS    _IOR('T',  3, struct throttle_prog_list)
#define IOCTL_LIST_UIDS     _IOR('T',  6, struct throttle_uid_list)
#define IOCTL_LIST_SYSCALLS _IOR('T',  9, struct throttle_syscall_list)

/* ================================================================
 *  STRUTTURE DATI INTERNE
 *
 *  Syscall  → bitmap   (NR_syscalls bit, test_bit O(1))
 *  UID      → hashtable 2^UID_HASH_BITS  bucket, chiave = uid
 *  Programmi→ hashtable 2^PROG_HASH_BITS bucket, chiave = inode
 * ================================================================ */

#define UID_HASH_BITS  8    /* 256 bucket */
#define PROG_HASH_BITS 8    /* 256 bucket */

struct prog_node {
    unsigned long     inode;
    dev_t             device;
    char              fpath[PROG_PATH_MAX];
    struct hlist_node hnode;    /* nodo hashtable, bucket selezionato da inode */
};
struct uid_node {
    uid_t             uid;
    struct hlist_node hnode;    /* nodo hashtable, bucket selezionato da uid */
};
/* syscall_node rimossa: il set di syscall è tracciato da syscall_bitmap */

typedef asmlinkage long (*syscall_fn_t)(const struct pt_regs *);

/* ================================================================
 *  STATO GLOBALE — definito in throttle_main.c
 * ================================================================ */

/* Hashtable programmi/UID (2^BITS bucket) e bitmap syscall (NR_syscalls bit) */
extern struct hlist_head prog_table[1 << PROG_HASH_BITS];
extern struct hlist_head uid_table[1 << UID_HASH_BITS];
extern unsigned long     syscall_bitmap[BITS_TO_LONGS(NR_syscalls)];

extern int               monitor_enabled;
extern int               max_calls;
extern int               module_unloading;
extern spinlock_t        config_lock;

extern atomic_t          call_count;
extern wait_queue_head_t throttle_wq;

/* ================================================================
 *  STATISTICHE — definite in throttle_core.c
 * ================================================================ */

extern long long  peak_delay_ns;
extern char       peak_delay_prog[TASK_COMM_LEN];
extern uid_t      peak_delay_uid;
extern atomic_t   current_blocked;
extern long       peak_blocked;
extern long long  total_blocked_sum;
extern long       total_blocked_count;
extern long       peak_calls_per_window;
extern long long  total_calls_sum_w;
extern spinlock_t stats_lock;

/* Drain protocol: usati da throttle_hook.c e throttle_main.c */
extern atomic_t          active_threads_in_wrapper;
extern wait_queue_head_t unload_wq;
extern struct hrtimer    window_timer;

/* ================================================================
 *  SYS_CALL_TABLE — definita in throttle_discovery.c
 * ================================================================ */

extern syscall_fn_t *sys_call_table_ptr;

/* ================================================================
 *  PROTOTIPI — throttle_mem.c
 * ================================================================ */

int  sys_vtpmo(unsigned long vaddr);
void begin_syscall_table_hack(void);
void end_syscall_table_hack(void);
void write_cr0_forced(unsigned long val);

/* ================================================================
 *  PROTOTIPI — throttle_discovery.c
 * ================================================================ */

int find_sys_call_table(void);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
int  find_x64_sys_call(void);
int  patch_x64_sys_call(void);
void restore_x64_sys_call(void);
#endif

/* ================================================================
 *  PROTOTIPI — throttle_hook.c
 * ================================================================ */

int  install_hook(int nr);
void remove_hook(int nr);
void remove_all_hooks(void);

/* ================================================================
 *  PROTOTIPI — throttle_core.c
 * ================================================================ */

void throttle_check(int nr);
void throttle_core_start(struct hrtimer *timer);

/* ================================================================
 *  PROTOTIPI — throttle_dev.c
 * ================================================================ */

int  throttle_dev_register(void);
void throttle_dev_unregister(void);

#endif /* THROTTLE_H */
