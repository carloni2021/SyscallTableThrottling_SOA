
/* throttle_dev.c — Device a caratteri e handler IOCTL
 *
 * Espone /dev/throttleDriver come interfaccia di configurazione.
 * Tutti i comandi che modificano lo stato richiedono privilegi root (euid=0).
 * I comandi di sola lettura (GET_*) sono accessibili a tutti.
 *
 * Comandi IOCTL supportati:
 *   ADD/DEL PROG         — gestione lista programmi monitorati
 *   ADD/DEL UID          — gestione lista UID monitorati
 *   ADD/DEL SYSCALL      — gestione lista syscall hookate
 *   SET/GET_MONITOR      — abilita/disabilita il throttling
 *   SET/GET_MAX          — imposta il limite di chiamate per finestra
 *   GET/RESET_STATS      — statistiche di ritardo e thread bloccati
 *
 * read() su /dev/throttleDriver restituisce la lista di programmi, UID e
 *   syscall registrati in formato testo — accessibile a tutti, nessun IOCTL.
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
    -programmi (hash table)
    -UID       (hash table)
    -syscall   (bitmap)
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
    char         prog_path[PROG_PATH_MAX];
    unsigned int uid_val;
    int          nr_val;
    unsigned long flags;

    // Solo i comandi di lettura sono accessibili a tutti, gli altri richiedono privilegi root
    int read_only = (cmd == IOCTL_GET_STATUS ||
                     cmd == IOCTL_GET_STATS);

    // Controllo dei privilegi: se il comando non è di sola lettura e il chiamante non è root, negare l'accesso
    if (!read_only && !caller_is_root()) {
        printk(KERN_WARNING "<throttle>: accesso negato a euid=%u\n",
               from_kuid(&init_user_ns, current_euid()));
        return -EPERM;
    }

    //effettiva gestione comandi
    switch (cmd) {

    //Aggiunta tramite progname
    case IOCTL_ADD_PROG: {
        struct path kpath;
        // Copia del percorso del programma da spazio utente a kernel
        if (copy_from_user(prog_path, (char __user *)arg, PROG_PATH_MAX))
            return -EFAULT;

        // Assicura che la stringa sia null-terminated (buff overflow)
        prog_path[PROG_PATH_MAX - 1] = '\0';

        // Risolve il percorso per inode e device
        if (kern_path(prog_path, LOOKUP_FOLLOW, &kpath))
            return -ENOENT;

        // Crea un nuovo nodo per il programma e popola i campi
        pnode = kmalloc(sizeof(*pnode), GFP_KERNEL);
        if (!pnode) { path_put(&kpath); return -ENOMEM; }
        pnode->inode  = kpath.dentry->d_inode->i_ino;
        pnode->device = kpath.dentry->d_inode->i_sb->s_dev;
        strscpy(pnode->fpath, prog_path, PROG_PATH_MAX);
        path_put(&kpath);

        // Aggiunge il nodo alla hash table dei programmi monitorati, evitando duplicati
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

    //Rimozione tramite progname
    case IOCTL_DEL_PROG: {
        struct path kpath;
        unsigned long del_ino; dev_t del_dev;
        // Copia sicura del percorso del programma da spazio utente a kernel
        if (copy_from_user(prog_path, (char __user *)arg, PROG_PATH_MAX))
            return -EFAULT;
        prog_path[PROG_PATH_MAX - 1] = '\0';
        //Controlla se il percorso esiste e ottiene inode e device per la rimozione
        if (kern_path(prog_path, LOOKUP_FOLLOW, &kpath)) return -ENOENT;
        del_ino = kpath.dentry->d_inode->i_ino;
        del_dev = kpath.dentry->d_inode->i_sb->s_dev;
        path_put(&kpath);
        // Rimuove il nodo corrispondente dalla hash table dei programmi monitorati
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each_possible_safe(prog_table, pcursor, htmp, hnode, del_ino)
            if (pcursor->inode == del_ino && pcursor->device == del_dev) {
                hash_del(&pcursor->hnode); kfree(pcursor);
            }
        spin_unlock_irqrestore(&config_lock, flags);
        break;
    }

    //SEZIONE UID
    // Aggiunta tramite UID
    case IOCTL_ADD_UID:
        if (copy_from_user(&uid_val, (unsigned int __user *)arg, sizeof(uid_val)))
            return -EFAULT;
        // Crea un nuovo nodo per l'UID e popola i campi
        unode = kmalloc(sizeof(*unode), GFP_KERNEL);
        if (!unode) return -ENOMEM;
        unode->uid = (uid_t)uid_val;

        //Aggiunta del nodo alla hash table (sempre tramite uid come chiave) con controllo di duplicati, sotto lock
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

    // Rimozione tramite UID
    case IOCTL_DEL_UID:
        if (copy_from_user(&uid_val, (unsigned int __user *)arg, sizeof(uid_val)))
            return -EFAULT;
        spin_lock_irqsave(&config_lock, flags);
        hash_for_each_possible_safe(uid_table, ucursor, htmp, hnode, uid_val)
            if (ucursor->uid == (uid_t)uid_val) { hash_del(&ucursor->hnode); kfree(ucursor); }
        spin_unlock_irqrestore(&config_lock, flags);
        break;

    //Aggiunta avviene tramite numero di syscall (nr) e viene gestita con una bitmap per efficienza.
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

    //Rimozione syscall
    case IOCTL_DEL_SYSCALL:
        if (copy_from_user(&nr_val, (int __user *)arg, sizeof(nr_val)))
            return -EFAULT;
        if (!test_bit(nr_val, syscall_bitmap)) break;       /* non presente */
        spin_lock_irqsave(&config_lock, flags);
        clear_bit(nr_val, syscall_bitmap);
        spin_unlock_irqrestore(&config_lock, flags);
        remove_hook(nr_val);
        break;

    //Sezione monitoraggio: abilitazione/disabilitazione e query stato
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

    // Imposta il limite di chiamate per finestra.
    case IOCTL_SET_MAX:
        if (copy_from_user(&nr_val, (int __user *)arg, sizeof(nr_val)))
            return -EFAULT;
        if (nr_val < 0) return -EINVAL;

        //modifica max_calls
        spin_lock_irqsave(&config_lock, flags);
        max_calls = nr_val;
        spin_unlock_irqrestore(&config_lock, flags);

        //reset statistiche
        spin_lock_irqsave(&stats_lock, flags);
        peak_delay_ns = 0; peak_delay_uid = 0; total_delay_ns = 0; delay_count = 0;
        //memset per azzerare il nome del programma associato al picco di ritardo
        memset(peak_delay_prog, 0, TASK_COMM_LEN);
        peak_blocked = 0; total_blocked_sum = 0; total_blocked_count = 0;
        peak_calls_per_window = 0; total_calls_sum_w = 0;
        spin_unlock_irqrestore(&stats_lock, flags);

        /* Sveglia al più nr_val thread: coerente con i nuovi slot disponibili */
        wake_up_nr(&throttle_wq, nr_val);
        printk(KERN_INFO "<throttle>: MAX=%d\n", nr_val);
        break;

    //azzera le statistiche di ritardo e thread bloccati
    case IOCTL_RESET_STATS:
        spin_lock_irqsave(&stats_lock, flags);
        peak_delay_ns = 0; peak_delay_uid = 0; total_delay_ns = 0; delay_count = 0;
        memset(peak_delay_prog, 0, TASK_COMM_LEN);
        peak_blocked = 0; total_blocked_sum = 0; total_blocked_count = 0;
        peak_calls_per_window = 0; total_calls_sum_w = 0;
        spin_unlock_irqrestore(&stats_lock, flags);
        break;

    //Stampa le statistiche di ritardo e thread bloccati, con protezione tramite spinlock per garantire coerenza dei dati durante la lettura.
    case IOCTL_GET_STATS:
        spin_lock_irqsave(&stats_lock, flags);
        stats_out.peak_delay_ns        = peak_delay_ns;
        stats_out.avg_delay_ns         = (delay_count > 0)
            ? (total_delay_ns / delay_count) : 0;
        stats_out.peak_delay_uid       = peak_delay_uid;
        strscpy(stats_out.peak_delay_prog, peak_delay_prog, TASK_COMM_LEN);
        stats_out.peak_blocked_threads = peak_blocked;
        stats_out.avg_blocked_threads  = (total_blocked_count > 0)
            ? (total_blocked_sum / total_blocked_count) : 0;
        stats_out.peak_calls_per_window = peak_calls_per_window;
        stats_out.avg_calls_per_window  = (total_blocked_count > 0)
            ? (long)(total_calls_sum_w / total_blocked_count) : 0;
        stats_out.total_calls           = total_calls_sum_w;
        spin_unlock_irqrestore(&stats_lock, flags);
        // Copia sicura delle statistiche dallo spazio kernel a quello utente
        if (copy_to_user((struct throttle_stats __user *)arg, &stats_out, sizeof(stats_out)))
            return -EFAULT;
        break;

    default:
        return -EINVAL;
    }
    return 0;
}

/*
 * dev_read — lettura da /dev/throttleDriver
 *
 * Restituisce un testo formattato con lista di programmi, UID e syscall registrati al momento della chiamata.
 * Il buffer viene allocato dinamicamente. La paginazione gestita tramite *ppos, come per file kernel.
 *
 * Due passaggi sotto config_lock:
 *   1. conta gli elementi per dimensionare il buffer
 *   2. formatta il testo nel buffer allocato
 */
static ssize_t dev_read(struct file *filep, char __user *buf,
                        size_t count, loff_t *ppos)
{
    struct prog_node *pcursor;
    struct uid_node  *ucursor;
    unsigned long flags;
    char *kbuf;
    size_t limit, len = 0;
    ssize_t to_copy;
    int bkt, bit;
    int prog_cnt = 0, uid_cnt = 0, bit_cnt = 0;

    /* Primo passaggio: conta gli elementi per dimensionare il buffer */
    spin_lock_irqsave(&config_lock, flags);
    hash_for_each(prog_table, bkt, pcursor, hnode) prog_cnt++;
    hash_for_each(uid_table,  bkt, ucursor, hnode) uid_cnt++;
    for_each_set_bit(bit, syscall_bitmap, NR_syscalls) bit_cnt++;
    spin_unlock_irqrestore(&config_lock, flags);

    //256 byte di overhead per intestazioni e formattazione, più spazio per ogni elemento (path programma, UID, numero syscall)
    limit = 256 + (size_t)prog_cnt * (PROG_PATH_MAX + 6) + (size_t)uid_cnt  * 24 + (size_t)bit_cnt  * 12;

    kbuf = kvzalloc(limit, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    /* Secondo passaggio: formatta il testo */
    spin_lock_irqsave(&config_lock, flags);
    len += scnprintf(kbuf + len, limit - len,
                     "=== Programmi registrati (%d) ===\n", prog_cnt);
    hash_for_each(prog_table, bkt, pcursor, hnode)
        len += scnprintf(kbuf + len, limit - len, "  %s\n", pcursor->fpath);

    len += scnprintf(kbuf + len, limit - len,
                     "=== UID registrati (%d) ===\n", uid_cnt);
    hash_for_each(uid_table, bkt, ucursor, hnode)
        len += scnprintf(kbuf + len, limit - len, "  %u\n", ucursor->uid);

    len += scnprintf(kbuf + len, limit - len,
                     "=== Syscall registrate (%d) ===\n", bit_cnt);
    for_each_set_bit(bit, syscall_bitmap, NR_syscalls)
        len += scnprintf(kbuf + len, limit - len, "  %d\n", bit);
    spin_unlock_irqrestore(&config_lock, flags);

    //check per non leggere oltre buffer
    if (*ppos >= (loff_t)len) {
        kvfree(kbuf);
        return 0;
    }

    //calcola quanti byte restituire
    to_copy = min((size_t)(len - (size_t)*ppos), count);
    if (copy_to_user(buf, kbuf + *ppos, to_copy)) {
        kvfree(kbuf);
        return -EFAULT;
    }

    //aggiorna posizione di lettura
    *ppos += to_copy;
    kvfree(kbuf);
    return to_copy;
}

// Struttura file_operations che definisce le operazioni supportate dal device, inclusa la gestione degli IOCTL.
static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = dev_open,
    .release        = dev_release,
    .read           = dev_read,
    .unlocked_ioctl = ioctl_handler,
};

// Registra il device a caratteri e associa le operazioni definite in fops.
//Il major viene assegnato dinamicamente e restituito per la creazione dell'interfaccia device.
int throttle_dev_register(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    return major;
}

//Funzione di cleanup che deregistra il device a caratteri,
//rimuovendo l'associazione con /dev/throttleDriver e liberando le risorse allocate per l'interfaccia device.
void throttle_dev_unregister(void)
{
    unregister_chrdev(major, DEVICE_NAME);
}
