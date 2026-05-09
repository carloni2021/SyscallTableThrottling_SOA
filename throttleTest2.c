/*
 * throttleTest2.c — test avanzato per il syscall throttling LKM
 *
 * Esegue due test indipendenti sul driver throttleDriver:
 *
 *  TEST 1 — Syscall bloccante: read(2) su /dev/zero
 *    Verifica che il meccanismo di throttling funzioni correttamente
 *    per una syscall bloccante.  read(2) è il caso classico: il wrapper
 *    SCT intercetta la chiamata, esegue throttle_check() (che può mettere
 *    il thread nella wait queue), e solo dopo chiama l'handler originale.
 *    L'utilizzo di /dev/zero garantisce che la sorgente non introduca
 *    ulteriore latenza: l'unico fattore limitante è il rate limit del LKM.
 *    In questo modo si dimostra che il flusso
 *        "thread → wrapper → sleep in wait_queue → orig read() → dati"
 *    funziona esattamente come per una syscall non bloccante.
 *
 *  TEST 2 — Throttling basato su UID
 *    Verifica che il driver applichi il rate limit sulla base dell'effective
 *    UID del chiamante, indipendentemente dal nome del programma.
 *    Vengono registrate nel driver solo:
 *        - una syscall (getpid, nr=39)
 *        - un UID (TARGET_UID)
 *    Due gruppi di processi figlio operano in parallelo:
 *        - "throttled": N figli con eUID = TARGET_UID  → soggetti al MAX
 *        - "control":   M figli con eUID = CONTROL_UID → nessun limite
 *    I figli usano fork()+setresuid() per cambiare UID prima di entrare
 *    nel loop di test; i contatori condivisi tra processi sono realizzati
 *    con memoria anonima mappata (mmap MAP_SHARED|MAP_ANONYMOUS) e
 *    variabili _Atomic, che garantiscono la coerenza senza bisogno di lock.
 *
 * Uso:
 *   sudo ./throttleTest2 <num_workers> <durata_sec> <MAX>
 *
 * Esempio:
 *   sudo ./throttleTest2 8 6 200
 *
 * Note:
 *   - Richiede root per le ioctl di configurazione.
 *   - Il test 2 utilizza gli UID sintetici TARGET_UID e CONTROL_UID
 *     (default 60001/60002): non è necessario che esistano in /etc/passwd.
 *   - Il cleanup viene eseguito anche in caso di errore.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <libgen.h>
#include <errno.h>

/* ================================================================
 *  Interfaccia ioctl — deve coincidere con throttleDriver.c
 * ================================================================ */

#define TASK_COMM_LEN  16
#define PROG_PATH_MAX  256
#define DEVICE_PATH    "/dev/throttleDriver"

struct throttle_stats {
    long long    peak_delay_ns;
    long long    avg_delay_ns;
    char         peak_delay_prog[TASK_COMM_LEN];
    unsigned int peak_delay_uid;
    long         avg_blocked_threads;
    long         peak_blocked_threads;
    long         peak_calls_per_window;
    long         avg_calls_per_window;
    long long    total_calls;
};

#define IOCTL_ADD_PROG    _IOW('T',  1, char[PROG_PATH_MAX])
#define IOCTL_DEL_PROG    _IOW('T',  2, char[PROG_PATH_MAX])
#define IOCTL_ADD_UID     _IOW('T',  4, unsigned int)
#define IOCTL_DEL_UID     _IOW('T',  5, unsigned int)
#define IOCTL_ADD_SYSCALL _IOW('T',  7, int)
#define IOCTL_DEL_SYSCALL _IOW('T',  8, int)
#define IOCTL_SET_MONITOR _IOW('T', 10, int)
#define IOCTL_SET_MAX     _IOW('T', 12, int)
#define IOCTL_GET_STATS   _IOR('T', 13, struct throttle_stats)
#define IOCTL_RESET_STATS _IO ('T', 14)

/*
 * UID sintetici per il test 2.  Valori alti (>60000) per evitare
 * collisioni con utenti di sistema o utenti reali.
 */
#define TARGET_UID   60001u   /* UID registrato nel driver → throttlato   */
#define CONTROL_UID  60002u   /* UID NON registrato       → non throttlato */

/* ================================================================
 *  TEST 1: Syscall bloccante — read(2) su /dev/zero
 * ================================================================ */

static atomic_long  t1_calls   = 0;
static volatile int t1_running = 1;
static int          t1_zerofd  = -1;

/*
 * Worker thread: chiama read() in loop il più velocemente possibile.
 *
 * read() è la syscall bloccante per eccellenza nel modello UNIX:
 * può sospendere il thread in kernel space in attesa di dati
 * (pipe vuota, socket, file su NFS, ...).  Su /dev/zero ritorna
 * immediatamente, ma il percorso kernel è identico: il wrapper
 * SCT installato dal LKM intercetta la chiamata e — se il rate
 * limit è superato — il thread dorme nella wait_queue prima ancora
 * che l'handler originale di read() venga eseguito.
 */
static void *t1_worker(void *arg)
{
    char buf[1];
    (void)arg;
    while (t1_running) {
        read(t1_zerofd, buf, 1);
        atomic_fetch_add(&t1_calls, 1);
    }
    return NULL;
}

static int test_blocking(int drv_fd, const char *progpath,
                         int nworkers, int duration, int max_val)
{
    int  result   = 1;           /* assume FAIL finché non verificato */
    int  prog_ok  = 0;
    int  sys_ok   = 0;
    int  sys_nr   = SYS_read;
    int  mon_val  = 1;

    printf("\n========================================\n");
    printf("  TEST 1: syscall bloccante — read(2)\n");
    printf("========================================\n");
    printf("  Syscall  : read(2) su /dev/zero  (nr=%d)\n", SYS_read);
    printf("  Worker   : %d pthread\n", nworkers);
    printf("  Durata   : %d s  |  MAX = %d inv/s\n\n", duration, max_val);

    /* ---- Apri /dev/zero ---- */
    t1_zerofd = open("/dev/zero", O_RDONLY);
    if (t1_zerofd < 0) { perror("open /dev/zero"); return -1; }

    /* ---- Configura il driver ---- */
    if (ioctl(drv_fd, IOCTL_ADD_PROG, progpath) < 0) { perror("ADD_PROG"); goto cleanup; }
    prog_ok = 1;
    if (ioctl(drv_fd, IOCTL_ADD_SYSCALL, &sys_nr)  < 0) { perror("ADD_SYSCALL"); goto cleanup; }
    sys_ok = 1;
    if (ioctl(drv_fd, IOCTL_SET_MAX,     &max_val) < 0) { perror("SET_MAX");     goto cleanup; }
    if (ioctl(drv_fd, IOCTL_SET_MONITOR, &mon_val) < 0) { perror("SET_MONITOR"); goto cleanup; }

    /* ---- Lancia i worker ---- */
    pthread_t *th = malloc(nworkers * sizeof(pthread_t));

    for (int i = 0; i < nworkers; i++)
        pthread_create(&th[i], NULL, t1_worker, NULL);

    /* Warmup: lascia scadere la finestra parziale del driver (vedi throttleTest.c) */
    sleep(1);
    /* Reset statistiche dopo il warmup: le finestre successive vengono
     * misurate dal driver, allineate ai propri boundary. */
    ioctl(drv_fd, IOCTL_RESET_STATS, NULL);

    sleep(duration);

    /* ---- Ferma i thread ---- */
    t1_running = 0;
    { int off = 0; ioctl(drv_fd, IOCTL_SET_MONITOR, &off); }
    for (int i = 0; i < nworkers; i++)
        pthread_join(th[i], NULL);
    free(th);

    /* ---- Statistiche driver ---- */
    struct throttle_stats st = {0};
    ioctl(drv_fd, IOCTL_GET_STATS, &st);

    /* ---- Verifica basata su statistiche driver (allineate all'hrtimer) ---- */
    int pass_avg  = (st.avg_calls_per_window  <= (long)max_val);
    int pass_wins = (st.peak_calls_per_window <= (long)max_val);
    result = (pass_avg && pass_wins) ? 0 : 1;

    printf("\n  -- Statistiche driver (per-finestra, allineate all'hrtimer) --\n");
    printf("  Peak calls/finestra : %ld\n",   st.peak_calls_per_window);
    printf("  Avg  calls/finestra : %ld\n",   st.avg_calls_per_window);
    printf("  Totale chiamate     : %lld in %d s (misurato dal driver)\n",
           st.total_calls, duration);
    printf("  Peak delay          : %lld ns (%.3f ms)\n",
           st.peak_delay_ns, st.peak_delay_ns / 1e6);
    printf("  Avg  delay          : %lld ns (%.3f ms)\n",
           st.avg_delay_ns, st.avg_delay_ns / 1e6);
    printf("  Peak delay prog     : '%s'  uid=%u\n",
           st.peak_delay_prog, st.peak_delay_uid);
    printf("  Peak bloccati       : %ld thread\n", st.peak_blocked_threads);
    printf("  Avg  bloccati       : %ld thread\n", st.avg_blocked_threads);

    printf("\n  -- Verifica (basata su statistiche driver) --\n");
    printf("  Avg/finestra <= MAX (%d): %s  (%ld)\n",
           max_val, pass_avg  ? "PASS" : "FAIL", st.avg_calls_per_window);
    printf("  Peak/finestra <= MAX (%d): %s  (%ld)\n",
           max_val, pass_wins ? "PASS" : "FAIL", st.peak_calls_per_window);

cleanup:
    { int off = 0; ioctl(drv_fd, IOCTL_SET_MONITOR, &off); }
    if (sys_ok)  ioctl(drv_fd, IOCTL_DEL_SYSCALL, &sys_nr);
    if (prog_ok) ioctl(drv_fd, IOCTL_DEL_PROG, progpath);
    close(t1_zerofd);
    return result;
}

/* ================================================================
 *  TEST 2: Throttling basato su UID
 * ================================================================ */

/*
 * Stato condiviso tra il processo padre e i processi figlio.
 *
 * Allocato con mmap(MAP_SHARED|MAP_ANONYMOUS) in modo che le scritture
 * di un processo siano immediatamente visibili agli altri.  Le variabili
 * _Atomic garantiscono la coerenza senza lock: su x86-64 le operazioni
 * atomic_fetch_add() si traducono in istruzioni LOCK XADD che operano
 * direttamente sulla memoria fisica condivisa, indipendentemente da quale
 * processo la esegue.
 */
struct t2_shared {
    _Atomic long throttled_calls;  /* counter per il gruppo con TARGET_UID  */
    _Atomic long control_calls;    /* counter per il gruppo con CONTROL_UID */
    _Atomic int  running;          /* flag di stop: 0 → i figli terminano   */
};

/*
 * Corpo del processo figlio "throttled" (eUID = TARGET_UID).
 *
 * setresuid() imposta ruid=euid=suid=TARGET_UID: dal punto di vista del
 * kernel, current_euid() restituirà TARGET_UID per questo processo.
 * Se TARGET_UID è registrato nel driver, ogni invocazione di una syscall
 * registrata (getpid, nr=39) passerà per throttle_check() e verrà
 * soggetta al rate limit.
 */
static void run_throttled_child(int drv_fd, struct t2_shared *sh)
{
    close(drv_fd);   /* il fd del driver non serve al figlio */

    if (setresuid(TARGET_UID, TARGET_UID, TARGET_UID) < 0) {
        perror("setresuid throttled");
        exit(1);
    }

    while (atomic_load(&sh->running)) {
        syscall(SYS_getpid);
        atomic_fetch_add(&sh->throttled_calls, 1);
    }
    exit(0);
}

/*
 * Corpo del processo figlio "control" (eUID = CONTROL_UID).
 *
 * CONTROL_UID non è registrato nel driver: caller_is_registered() lo
 * restituirà 0, quindi throttle_check() ritorna immediatamente senza
 * applicare alcun limite.  Il gruppo di controllo deve girare a velocità
 * piena, dimostrando che il throttling è selettivo.
 */
static void run_control_child(int drv_fd, struct t2_shared *sh)
{
    close(drv_fd);

    if (setresuid(CONTROL_UID, CONTROL_UID, CONTROL_UID) < 0) {
        perror("setresuid control");
        exit(1);
    }

    while (atomic_load(&sh->running)) {
        syscall(SYS_getpid);
        atomic_fetch_add(&sh->control_calls, 1);
    }
    exit(0);
}

static int test_uid(int drv_fd, int nworkers, int duration, int max_val)
{
    int  result  = 1;
    int  uid_ok  = 0;
    int  sys_ok  = 0;
    int  sys_nr  = SYS_getpid;
    int  mon_val = 1;
    unsigned int tuid = TARGET_UID;

    /* Numero di processi per gruppo: stesso numero di worker */
    int n_throttled = nworkers;
    int n_control   = (nworkers < 4) ? nworkers : 4; /* max 4 per il gruppo control */

    printf("\n========================================\n");
    printf("  TEST 2: throttling basato su UID\n");
    printf("========================================\n");
    printf("  Syscall         : getpid()  (nr=%d)\n", SYS_getpid);
    printf("  UID registrato  : %u  (gruppo throttled, %d proc)\n",
           TARGET_UID, n_throttled);
    printf("  UID controllo   : %u  (gruppo control,   %d proc)\n",
           CONTROL_UID, n_control);
    printf("  Prog registrato : nessuno  (solo UID)\n");
    printf("  Durata          : %d s  |  MAX = %d inv/s\n\n",
           duration, max_val);

    /* ---- Alloca stato condiviso (padre + tutti i figli) ----
     * fork() copia lo spazio di memoria: senza memoria condivisa ogni figlio
     * avrebbe il suo contatore isolato. MAP_SHARED|MAP_ANONYMOUS mappa le
     * stesse pagine fisiche in tutti i processi, le variabili _Atomic usano
     * LOCK XADD sul bus e sono quindi atomiche anche tra processi distinti. */
    struct t2_shared *sh = mmap(NULL, sizeof(*sh),
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) { perror("mmap"); return -1; }
    atomic_init(&sh->throttled_calls, 0);
    atomic_init(&sh->control_calls,   0);
    atomic_init(&sh->running,         1);

    /* ---- Configura il driver: solo UID, nessun prog name ----
     *
     * Registrando unicamente TARGET_UID, caller_is_registered() tornerà
     * 1 solo per i processi il cui effective UID coincide con TARGET_UID.
     * L'assenza di prog name nella lista garantisce che il nome del
     * binario ("throttleTest2") non contribuisca al match: il test è
     * quindi un test puro dell'identità per UID.
     */
    if (ioctl(drv_fd, IOCTL_ADD_SYSCALL, &sys_nr)  < 0) { perror("ADD_SYSCALL"); goto cleanup_mmap; }
    sys_ok = 1;
    if (ioctl(drv_fd, IOCTL_ADD_UID,     &tuid)    < 0) { perror("ADD_UID");     goto cleanup_mmap; }
    uid_ok = 1;
    if (ioctl(drv_fd, IOCTL_SET_MAX,     &max_val) < 0) { perror("SET_MAX");     goto cleanup_mmap; }
    if (ioctl(drv_fd, IOCTL_SET_MONITOR, &mon_val) < 0) { perror("SET_MONITOR"); goto cleanup_mmap; }

    /* ---- Fork dei processi figlio ---- */
    pid_t *pids = malloc((n_throttled + n_control) * sizeof(pid_t));

    for (int i = 0; i < n_throttled; i++) {
        pid_t p = fork();
        if (p < 0) { perror("fork throttled"); pids[i] = -1; continue; }
        if (p == 0) run_throttled_child(drv_fd, sh); /* non ritorna */
        pids[i] = p;
    }
    for (int i = 0; i < n_control; i++) {
        pid_t p = fork();
        if (p < 0) { perror("fork control"); pids[n_throttled + i] = -1; continue; }
        if (p == 0) run_control_child(drv_fd, sh); /* non ritorna */
        pids[n_throttled + i] = p;
    }

    /* Warmup: lascia scadere la finestra parziale del driver */
    sleep(1);
    ioctl(drv_fd, IOCTL_RESET_STATS, NULL);

    /* ---- Misurazione throughput ---- */
    long base_ctl = atomic_load(&sh->control_calls);

    sleep(duration);

    /* Cattura il totale del gruppo control prima di fermare i processi:
     * dopo IOCTL_SET_MONITOR(0) i processi throttled bloccati si svegliano
     * senza limiti e gonfierebbero il conteggio. Il gruppo control non è
     * soggetto a throttling quindi il suo contatore è già affidabile. */
    long total_ctl = atomic_load(&sh->control_calls) - base_ctl;

    /* ---- Ferma i processi figlio ----
     *
     * Ordine deliberato:
     *   1. Segnala ai figli di terminare (running=0).
     *   2. Disabilita il monitor: wake_up_all() nel kernel sveglia
     *      i thread del gruppo throttled eventualmente bloccati nella
     *      wait_queue, così possono uscire dal loop e fare exit().
     *   3. waitpid() attende il completamento ordinato di tutti i figli.
     */
    atomic_store(&sh->running, 0);
    { int off = 0; ioctl(drv_fd, IOCTL_SET_MONITOR, &off); }
    for (int i = 0; i < n_throttled + n_control; i++)
        if (pids[i] > 0) waitpid(pids[i], NULL, 0);
    free(pids);

    /* ---- Statistiche driver ---- */
    struct throttle_stats st = {0};
    ioctl(drv_fd, IOCTL_GET_STATS, &st);

    /* ---- Verifica ---- */
    double avg_ctl    = (double)total_ctl / duration;

    /* Throttled: usa driver stats (allineate all'hrtimer, immune al drift) */
    int pass_thr_avg  = (st.avg_calls_per_window  <= (long)max_val);
    int pass_thr_wins = (st.peak_calls_per_window <= (long)max_val);

    /*
     * Control: il driver non monitora CONTROL_UID, quindi la misura
     * deve restare userspace. Il rate deve essere significativamente
     * superiore a MAX per dimostrare che il throttling è selettivo.
     */
    int pass_ctl_free = (avg_ctl > max_val * 5.0);

    printf("\n  -- Statistiche driver (gruppo throttled, allineate all'hrtimer) --\n");
    printf("  Peak calls/finestra : %ld\n",   st.peak_calls_per_window);
    printf("  Avg  calls/finestra : %ld\n",   st.avg_calls_per_window);
    printf("  Peak delay          : %lld ns (%.3f ms)\n",
           st.peak_delay_ns, st.peak_delay_ns / 1e6);
    printf("  Avg  delay          : %lld ns (%.3f ms)\n",
           st.avg_delay_ns, st.avg_delay_ns / 1e6);
    printf("  Peak delay prog     : '%s'  uid=%u\n",
           st.peak_delay_prog, st.peak_delay_uid);
    printf("  Peak bloccati       : %ld processi\n", st.peak_blocked_threads);
    printf("  Avg  bloccati       : %ld processi\n", st.avg_blocked_threads);

    printf("\n  -- Confronto gruppi --\n");
    printf("  Gruppo throttled (uid=%u): avg %ld calls/s (driver)\n",
           TARGET_UID, st.avg_calls_per_window);
    printf("  Gruppo control   (uid=%u): avg %.1f calls/s (userspace)\n",
           CONTROL_UID, avg_ctl);
    printf("  Rapporto control/throttled: %.1fx\n",
           st.avg_calls_per_window > 0 ? avg_ctl / st.avg_calls_per_window : 0.0);

    printf("\n  -- Verifica --\n");
    printf("  Throttled avg/finestra <= MAX (%d): %s  (%ld)\n",
           max_val, pass_thr_avg  ? "PASS" : "FAIL", st.avg_calls_per_window);
    printf("  Throttled peak/finestra <= MAX (%d): %s  (%ld)\n",
           max_val, pass_thr_wins ? "PASS" : "FAIL", st.peak_calls_per_window);
    printf("  Control media (%.1f) > MAX*5 (%d): %s\n",
           avg_ctl, max_val * 5, pass_ctl_free ? "PASS" : "FAIL");

    result = (pass_thr_avg && pass_thr_wins && pass_ctl_free) ? 0 : 1;

cleanup_mmap:
    if (sys_ok) ioctl(drv_fd, IOCTL_DEL_SYSCALL, &sys_nr);
    if (uid_ok) ioctl(drv_fd, IOCTL_DEL_UID,     &tuid);
    munmap(sh, sizeof(*sh));
    return result;
}

/* ================================================================
 *  main
 * ================================================================ */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "Uso: sudo %s <num_workers> <durata_sec> <MAX>\n"
            "  num_workers : processi/thread per gruppo\n"
            "  durata_sec  : durata di ciascun test (secondi)\n"
            "  MAX         : invocazioni/s massime configurate nel driver\n"
            "Esempio: sudo %s 8 6 200\n",
            argv[0], argv[0]);
        return 1;
    }

    int nworkers = atoi(argv[1]);
    int duration = atoi(argv[2]);
    int max_val  = atoi(argv[3]);

    if (nworkers <= 0 || duration <= 0 || max_val <= 0) {
        fprintf(stderr, "Errore: tutti i parametri devono essere > 0.\n");
        return 1;
    }

    /* ---- Apertura device ---- */
    int drv_fd = open(DEVICE_PATH, O_RDWR);
    if (drv_fd < 0) { perror("open " DEVICE_PATH); return 1; }

    /*
     * Path del binario: il kernel identifica l'eseguibile tramite inode/device
     * ricavati da mm->exe_file. Passiamo il path assoluto letto da
     * /proc/self/exe, che il kernel risolverà allo stesso inode.
     */
    char progpath[PROG_PATH_MAX] = {0};
    if (readlink("/proc/self/exe", progpath, PROG_PATH_MAX - 1) < 0) {
        perror("readlink /proc/self/exe");
        close(drv_fd); return 1;
    }

    printf("=========================================\n");
    printf("  throttleDriver — test avanzato\n");
    printf("=========================================\n");
    printf("  Binario  : '%s'  (PID %d, UID %d)\n",
           progpath, getpid(), getuid());
    printf("  Workers  : %d  |  Durata: %d s  |  MAX: %d\n",
           nworkers, duration, max_val);
    printf("=========================================\n");

    /* ---- Esecuzione dei test ---- */
    int r1 = test_blocking(drv_fd, progpath, nworkers, duration, max_val);
    int r2 = test_uid     (drv_fd,           nworkers, duration, max_val);

    close(drv_fd);

    /* ---- Riepilogo finale ---- */
    printf("\n=========================================\n");
    printf("  RIEPILOGO\n");
    printf("=========================================\n");
    printf("  Test 1 (syscall bloccante)  : %s\n",
           r1 == 0 ? "PASS" : r1 < 0 ? "ERRORE" : "FAIL");
    printf("  Test 2 (throttling per UID) : %s\n",
           r2 == 0 ? "PASS" : r2 < 0 ? "ERRORE" : "FAIL");
    int all_pass = (r1 == 0 && r2 == 0);
    printf("\n  %s\n", all_pass ? "PASS" : "FAIL");
    printf("=========================================\n");

    return all_pass ? 0 : 1;
}
