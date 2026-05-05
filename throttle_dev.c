
/* throttle_dev.c — Device a caratteri e handler IOCTL
 *
 * Espone /dev/throttleDriver come interfaccia di configurazione.
 * Tutti i comandi che modificano lo stato richiedono privilegi root (euid=0).
 * I comandi di sola lettura (LIST_*, GET_*) sono accessibili a tutti.
 *
 * Comandi IOCTL supportati:
 *   ADD/DEL/LIST PROG    — gestione lista programmi monitorati
 *   ADD/DEL/LIST UID     — gestione lista UID monitorati
 *   ADD/DEL/LIST SYSCALL — gestione lista syscall hookate
 *   SET/GET_MONITOR      — abilita/disabilita il throttling
 *   SET/GET_MAX          — imposta il limite di chiamate per finestra
 *   GET/RESET_STATS      — statistiche di ritardo e thread bloccati
*/

// Include del core per accesso a strutture e funzioni condivise
#include "throttle.h"

//Major del device, assegnato dinamicamente rappresenta l'identificativo del driver per operazioni su /dev/throttleDriver 
static int major;
// Static perché è usato solo in questo file. Viene assegnato da register_chrdev e usato per creare l'interfaccia device.
static int dev_open(struct inode *i, struct file *f)    { return 0; }
static int dev_release(struct inode *i, struct file *f) { return 0; }

/*verifica se il chiamante è root (euid=0) per autorizzazione operazioni di configurazione altrimenti 
restituisce -EPERM per bloccare l'accesso non autorizzato a funzionalità critiche del modulo.*/
static int caller_is_root(void)
{
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

/* Handler IOCTL per tutte le operazioni di configurazione e query.
 -verifica i privilegi del chiamante
 -gestisce le liste di 
    -programmi
    -UID
    -syscall
 -abilita/disabilita il monitoraggio
 -imposta il limite di chiamate 
 -restituisce statistiche.*/
static long ioctl_handler(struct file *filep,
                          unsigned int cmd, unsigned long arg)
{
    struct prog_node  *pnode, *pcursor;
    struct uid_node   *unode, *ucursor;
    struct hlist_node *htmp;
    struct throttle_stats        stats_out;
    struct throttle_prog_list   *plist = NULL;
    struct throttle_uid_list     ulist;
    struct throttle_syscall_list slist;
    char         prog_path[PROG_PATH_MAX];
    unsigned int uid_val;
    int          nr_val;
    unsigned long flags;

    int read_only = (cmd == IOCTL_LIST_PROGS   ||
                     cmd == IOCTL_LIST_UIDS     ||
                     cmd == IOCTL_LIST_SYSCALLS ||
                     cmd == IOCTL_GET_STATUS    ||
                     cmd == IOCTL_GET_STATS);

    if (!read_only && !caller_is_root()) {
        printk(KERN_WARNING "<throttle>: accesso negato a euid=%u\n",
               from_kuid(&init_user_ns, current_euid()));
        return -EPERM;
    }

    switch (cmd) {

    case IOCTL_ADD_PROG: {
        struct path kpath;
        if (copy_from_user(prog_path, (char __user *)arg, PROG_PATH_MAX))
            return -EFAULT;
        prog_path[PROG_PATH_MAX - 1] = '\0';
        if (kern_path(prog_path, LOOKUP_FOLLOW, &kpath))
            return -ENOENT;
        pnode = kmalloc(sizeof(*pnode), GFP_KERNEL);
        if (!pnode) { path_put(&kpath); return -ENOMEM; }
        pnode->inode  = kpath.dentry->d_inode->i_ino;
        pnode->device = kpath.dentry->d_inode->i_sb->s_dev;
        strscpy(pnode->fpath, prog_path, PROG_PATH_MAX);
        path_put(&kpath);
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each_possible(prog_table, pcursor, hnode, pnode->inode) {
            if (pcursor->inode == pnode->inode && pcursor->device == pnode->device) {
                spin_unlock_irqrestore(&config_lock, flags);
                kfree(pnode);
                return 0;
            }
        }
        hash_add(prog_table, &pnode->hnode, pnode->inode);
        spin_unlock_irqrestore(&config_lock, flags);
        printk(KERN_INFO "<throttle>: [PROG+] '%s'\n", pnode->fpath);
        break;
    }

    case IOCTL_DEL_PROG: {
        struct path kpath;
        unsigned long del_ino; dev_t del_dev;
        if (copy_from_user(prog_path, (char __user *)arg, PROG_PATH_MAX))
            return -EFAULT;
        prog_path[PROG_PATH_MAX - 1] = '\0';
        if (kern_path(prog_path, LOOKUP_FOLLOW, &kpath)) return -ENOENT;
        del_ino = kpath.dentry->d_inode->i_ino;
        del_dev = kpath.dentry->d_inode->i_sb->s_dev;
        path_put(&kpath);
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each_possible_safe(prog_table, pcursor, htmp, hnode, del_ino)
            if (pcursor->inode == del_ino && pcursor->device == del_dev) {
                hash_del(&pcursor->hnode); kfree(pcursor);
            }
        spin_unlock_irqrestore(&config_lock, flags);
        break;
    }

    case IOCTL_LIST_PROGS: {
        int bkt;
        plist = kzalloc(sizeof(*plist), GFP_KERNEL);
        if (!plist) return -ENOMEM;
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each(prog_table, bkt, pcursor, hnode) {
            if (plist->count < MAX_REG_PROGS)
                strscpy(plist->paths[plist->count++], pcursor->fpath, PROG_PATH_MAX);
        }
        spin_unlock_irqrestore(&config_lock, flags);
        if (copy_to_user((struct throttle_prog_list __user *)arg, plist, sizeof(*plist))) {
            kfree(plist); return -EFAULT;
        }
        kfree(plist);
        break;
    }

    case IOCTL_ADD_UID:
        if (copy_from_user(&uid_val, (unsigned int __user *)arg, sizeof(uid_val)))
            return -EFAULT;
        unode = kmalloc(sizeof(*unode), GFP_KERNEL);
        if (!unode) return -ENOMEM;
        unode->uid = (uid_t)uid_val;
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each_possible(uid_table, ucursor, hnode, uid_val) {
            if (ucursor->uid == (uid_t)uid_val) {
                spin_unlock_irqrestore(&config_lock, flags);
                kfree(unode); return 0;
            }
        }
        hash_add(uid_table, &unode->hnode, unode->uid);
        spin_unlock_irqrestore(&config_lock, flags);
        printk(KERN_INFO "<throttle>: [UID+] %u\n", uid_val);
        break;

    case IOCTL_DEL_UID:
        if (copy_from_user(&uid_val, (unsigned int __user *)arg, sizeof(uid_val)))
            return -EFAULT;
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each_possible_safe(uid_table, ucursor, htmp, hnode, uid_val)
            if (ucursor->uid == (uid_t)uid_val) { hash_del(&ucursor->hnode); kfree(ucursor); }
        spin_unlock_irqrestore(&config_lock, flags);
        break;

    case IOCTL_LIST_UIDS: {
        int bkt;
        memset(&ulist, 0, sizeof(ulist));
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each(uid_table, bkt, ucursor, hnode)
            if (ulist.count < MAX_REG_UIDS) ulist.uids[ulist.count++] = ucursor->uid;
        spin_unlock_irqrestore(&config_lock, flags);
        if (copy_to_user((struct throttle_uid_list __user *)arg, &ulist, sizeof(ulist)))
            return -EFAULT;
        break;
    }

    case IOCTL_ADD_SYSCALL: {
        /* Bitmap: test_bit O(1); nessuna allocazione, nessuna lista */
        int hret;
        if (copy_from_user(&nr_val, (int __user *)arg, sizeof(nr_val)))
            return -EFAULT;
        if (nr_val < 0 || nr_val >= NR_syscalls) return -EINVAL;
        if (test_bit(nr_val, syscall_bitmap)) return 0;     /* già presente */
        hret = install_hook(nr_val);
        if (hret) return hret;
        spin_lock_irqsave(&config_lock, flags);
        set_bit(nr_val, syscall_bitmap);
        spin_unlock_irqrestore(&config_lock, flags);
        printk(KERN_INFO "<throttle>: [SYS+] nr=%d\n", nr_val);
        break;
    }

    case IOCTL_DEL_SYSCALL:
        if (copy_from_user(&nr_val, (int __user *)arg, sizeof(nr_val)))
            return -EFAULT;
        if (!test_bit(nr_val, syscall_bitmap)) break;       /* non presente */
        spin_lock_irqsave(&config_lock, flags);
        clear_bit(nr_val, syscall_bitmap);
        spin_unlock_irqrestore(&config_lock, flags);
        remove_hook(nr_val);
        break;

    case IOCTL_LIST_SYSCALLS: {
        int bit;
        memset(&slist, 0, sizeof(slist));
        spin_lock_irqsave(&config_lock, flags);
        for_each_set_bit(bit, syscall_bitmap, NR_syscalls)
            if (slist.count < MAX_HOOKED_SYSCALLS) slist.nrs[slist.count++] = bit;
        spin_unlock_irqrestore(&config_lock, flags);
        if (copy_to_user((struct throttle_syscall_list __user *)arg, &slist, sizeof(slist)))
            return -EFAULT;
        break;
    }

    case IOCTL_SET_MONITOR:
        if (copy_from_user(&nr_val, (int __user *)arg, sizeof(nr_val)))
            return -EFAULT;
        spin_lock_irqsave(&config_lock, flags);
        monitor_enabled = (nr_val != 0) ? 1 : 0;
        spin_unlock_irqrestore(&config_lock, flags);
        if (!monitor_enabled)
            wake_up_all(&throttle_wq);
        printk(KERN_INFO "<throttle>: monitor %s\n", monitor_enabled ? "ON" : "OFF");
        break;

    case IOCTL_GET_STATUS: {
        struct throttle_status st;
        spin_lock_irqsave(&config_lock, flags);
        st.monitor_enabled = monitor_enabled;
        st.max_calls       = max_calls;
        st.current_count   = atomic_read(&call_count);
        spin_unlock_irqrestore(&config_lock, flags);
        if (copy_to_user((struct throttle_status __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        break;
    }

    case IOCTL_SET_MAX:
        if (copy_from_user(&nr_val, (int __user *)arg, sizeof(nr_val)))
            return -EFAULT;
        if (nr_val < 0) return -EINVAL;
        spin_lock_irqsave(&config_lock, flags);
        max_calls = nr_val;
        spin_unlock_irqrestore(&config_lock, flags);
        spin_lock_irqsave(&stats_lock, flags);
        peak_delay_ns = 0; peak_delay_uid = 0;
        memset(peak_delay_prog, 0, TASK_COMM_LEN);
        peak_blocked = 0; total_blocked_sum = 0; total_blocked_count = 0;
        spin_unlock_irqrestore(&stats_lock, flags);
        /* Sveglia al più nr_val thread: coerente con i nuovi slot disponibili */
        wake_up_nr(&throttle_wq, nr_val);
        printk(KERN_INFO "<throttle>: MAX=%d\n", nr_val);
        break;

    case IOCTL_RESET_STATS:
        spin_lock_irqsave(&stats_lock, flags);
        peak_delay_ns = 0; peak_delay_uid = 0;
        memset(peak_delay_prog, 0, TASK_COMM_LEN);
        peak_blocked = 0; total_blocked_sum = 0; total_blocked_count = 0;
        spin_unlock_irqrestore(&stats_lock, flags);
        break;

    case IOCTL_GET_STATS:
        spin_lock_irqsave(&stats_lock, flags);
        stats_out.peak_delay_ns       = peak_delay_ns;
        stats_out.peak_delay_uid      = peak_delay_uid;
        strscpy(stats_out.peak_delay_prog, peak_delay_prog, TASK_COMM_LEN);
        stats_out.peak_blocked_threads = peak_blocked;
        stats_out.avg_blocked_threads  = (total_blocked_count > 0)
            ? (total_blocked_sum / total_blocked_count) : 0;
        spin_unlock_irqrestore(&stats_lock, flags);
        if (copy_to_user((struct throttle_stats __user *)arg, &stats_out, sizeof(stats_out)))
            return -EFAULT;
        break;

    default:
        return -EINVAL;
    }
    return 0;
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = dev_open,
    .release        = dev_release,
    .unlocked_ioctl = ioctl_handler,
};

int throttle_dev_register(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    return major;
}

void throttle_dev_unregister(void)
{
    unregister_chrdev(major, DEVICE_NAME);
}
