/*
 * throttleTest2.c — test avanzato per il syscall throttling LKM
 *
 * Esegue due test indipendenti sul driver throttleDriver:
 *
 *  TEST 1 — Syscall bloccante: read(2) su /dev/zero
 *    Verifica che il meccanismo di throttling funzioni correttamente
 *    per una syscall che, per natura, può bloccare il thread in kernel
 *    space in attesa di dati.  read(2) è il caso classico: il wrapper
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
 *    La registrazione di sole tre entità nel driver:
 *        - una syscall (getpid, nr=39)
 *        - un UID (TARGET_UID)
 *    è sufficiente per distinguere i thread da throttlare.
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
    char         peak_delay_prog[TASK_COMM_LEN];
    unsigned int peak_delay_uid;
    long         avg_blocked_threads;
    long         peak_blocked_threads;
};

#define IOCTL_ADD_PROG    _IOW('T',  1, char[PROG_PATH_MAX])
#define IOCTL_DEL_PROG    _IOW('T',  2, char[PROG_PATH_MAX])
#define IOCTL_LIST_PROGS  _IOR('T',  3, struct throttle_prog_list)
#define IOCTL_ADD_UID     _IOW('T',  4, unsigned int)
#define IOCTL_DEL_UID     _IOW('T',  5, unsigned int)
#define IOCTL_ADD_SYSCALL _IOW('T',  7, int)
#define IOCTL_DEL_SYSCALL _IOW('T',  8, int)
#define IOCTL_SET_MONITOR _IOW('T', 10, int)
#define IOCTL_SET_MAX     _IOW('T', 12, int)
#define IOCTL_GET_STATS   _IOR('T', 13, struct throttle_stats)
#define IOCTL_RESET_STATS _IO ('T', 14)

#define MAX_REG_PROGS 32
struct throttle_prog_list {
    int  count;
    char paths[MAX_REG_PROGS][PROG_PATH_MAX];
};

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
    pthread_t *th     = malloc(nworkers * sizeof(pthread_t));
    long      *ps     = malloc(duration * sizeof(long));
    long       prev   = 0;

    for (int i = 0; i < nworkers; i++)
        pthread_create(&th[i], NULL, t1_worker, NULL);

    /* Warmup: lascia scadere la finestra parziale del driver (vedi throttleTest.c) */
    sleep(1);
    prev = atomic_load(&t1_calls);

    /* ---- Misurazione throughput ---- */
    printf("  sec  calls/s  stato\n");
    printf("  ---  -------  -----\n");
    for (int s = 0; s < duration; s++) {
        sleep(1);
        long cur  = atomic_load(&t1_calls);
        long rate = cur - prev;
        ps[s] = rate;
        prev  = cur;
        printf("  %3d  %7ld  %s\n", s+1, rate,
               rate > (long)(max_val * 1.5) ? "ANOMALIA" : "ok");
    }

    /* ---- Ferma i thread ---- */
    t1_running = 0;
    { int off = 0; ioctl(drv_fd, IOCTL_SET_MONITOR, &off); }
    for (int i = 0; i < nworkers; i++)
        pthread_join(th[i], NULL);
    free(th);
    long total = atomic_load(&t1_calls);

    /* ---- Statistiche driver ---- */
    struct throttle_stats st = {0};
    ioctl(drv_fd, IOCTL_GET_STATS, &st);

    /* ---- Verifica ---- */
    double avg    = (double)total / duration;
    int pass_avg  = (avg <= max_val * 1.2);
    int pass_wins = 1;
    for (int i = 0; i < duration; i++)
        if (ps[i] > (long)(max_val * 1.5)) { pass_wins = 0; break; }
    free(ps);
    result = (pass_avg && pass_wins) ? 0 : 1;

    printf("\n  -- Statistiche driver --\n");
    printf("  Peak delay      : %lld ns (%.3f ms)\n",
           st.peak_delay_ns, st.peak_delay_ns / 1e6);
    printf("  Peak delay prog : '%s'  uid=%u\n",
           st.peak_delay_prog, st.peak_delay_uid);
    printf("  Peak bloccati   : %ld thread\n", st.peak_blocked_threads);
    printf("  Avg  bloccati   : %ld thread\n", st.avg_blocked_threads);

    printf("\n  -- Verifica --\n");
    printf("  Media (%.1f) <= MAX*1.2 (%d): %s\n",
           avg, (int)(max_val * 1.2), pass_avg ? "PASS" : "FAIL");
    printf("  Nessuna finestra > MAX*1.5 (%d): %s\n",
           (int)(max_val * 1.5), pass_wins ? "PASS" : "FAIL");

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

    /* ---- Alloca stato condiviso (padre + tutti i figli) ---- */
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

    /* ---- Misurazione throughput ---- */
    long prev_thr = atomic_load(&sh->throttled_calls);
    long prev_ctl = atomic_load(&sh->control_calls);
    long *ps_thr  = malloc(duration * sizeof(long));
    long *ps_ctl  = malloc(duration * sizeof(long));

    printf("  sec  throttled/s  control/s  stato\n");
    printf("  ---  -----------  ---------  -----\n");
    for (int s = 0; s < duration; s++) {
        sleep(1);
        long cur_thr = atomic_load(&sh->throttled_calls);
        long cur_ctl = atomic_load(&sh->control_calls);
        ps_thr[s] = cur_thr - prev_thr;
        ps_ctl[s] = cur_ctl - prev_ctl;
        prev_thr  = cur_thr;
        prev_ctl  = cur_ctl;
        printf("  %3d  %11ld  %9ld  %s\n", s+1, ps_thr[s], ps_ctl[s],
               ps_thr[s] > (long)(max_val * 1.5) ? "ANOMALIA" : "ok");
    }

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

    long total_thr = atomic_load(&sh->throttled_calls);
    long total_ctl = atomic_load(&sh->control_calls);

    /* ---- Statistiche driver ---- */
    struct throttle_stats st = {0};
    ioctl(drv_fd, IOCTL_GET_STATS, &st);

    /* ---- Verifica ---- */
    double avg_thr    = (double)total_thr / duration;
    double avg_ctl    = (double)total_ctl / duration;
    int pass_thr_avg  = (avg_thr <= max_val * 1.2);
    int pass_thr_wins = 1;
    for (int i = 0; i < duration; i++)
        if (ps_thr[i] > (long)(max_val * 1.5)) { pass_thr_wins = 0; break; }

    /*
     * Il gruppo di controllo deve essere significativamente più veloce
     * del gruppo throttled: se il LKM funziona correttamente, il rate del
     * gruppo control sarà molto superiore a MAX (tipicamente 10×-100× su
     * hardware moderno). Usiamo MAX*5 come soglia minima conservativa.
     */
    int pass_ctl_free = (avg_ctl > max_val * 5.0);

    free(ps_thr);
    free(ps_ctl);

    printf("\n  -- Statistiche driver --\n");
    printf("  Peak delay      : %lld ns (%.3f ms)\n",
           st.peak_delay_ns, st.peak_delay_ns / 1e6);
    printf("  Peak delay prog : '%s'  uid=%u\n",
           st.peak_delay_prog, st.peak_delay_uid);
    printf("  Peak bloccati   : %ld processi\n", st.peak_blocked_threads);
    printf("  Avg  bloccati   : %ld processi\n", st.avg_blocked_threads);

    printf("\n  -- Confronto gruppi --\n");
    printf("  Gruppo throttled (uid=%u): %.1f calls/s\n", TARGET_UID,  avg_thr);
    printf("  Gruppo control   (uid=%u): %.1f calls/s\n", CONTROL_UID, avg_ctl);
    printf("  Rapporto control/throttled: %.1fx\n",
           avg_thr > 0 ? avg_ctl / avg_thr : 0.0);

    printf("\n  -- Verifica --\n");
    printf("  Throttled: media (%.1f) <= MAX*1.2 (%d): %s\n",
           avg_thr, (int)(max_val * 1.2), pass_thr_avg ? "PASS" : "FAIL");
    printf("  Throttled: nessuna finestra > MAX*1.5 (%d): %s\n",
           (int)(max_val * 1.5), pass_thr_wins ? "PASS" : "FAIL");
    printf("  Control:   media (%.1f) > MAX*5 (%d): %s\n",
           avg_ctl, max_val * 5, pass_ctl_free ? "PASS" : "FAIL");

    result = (pass_thr_avg && pass_thr_wins && pass_ctl_free) ? 0 : 1;

cleanup_mmap:
    if (sys_ok) ioctl(drv_fd, IOCTL_DEL_SYSCALL, &sys_nr);
    if (uid_ok) ioctl(drv_fd, IOCTL_DEL_UID,     &tuid);
    munmap(sh, sizeof(*sh));
    return result;
}

/* ================================================================
 *  TEST 3: Controllo accessi root-only
 * ================================================================ */

/*
 * La specifica richiede che le operazioni di modifica (registrazione,
 * deregistrazione, configurazione) siano riservate ai processi con
 * effective user-ID = 0.  Questo test lo verifica empiricamente:
 * un processo figlio abbassa il proprio eUID a TARGET_UID (non root)
 * e tenta alcune ioctl di scrittura → deve ricevere EPERM.
 * Le ioctl di sola lettura (LIST_*) devono invece avere successo.
 *
 * Il risultato viene comunicato al padre tramite il codice di uscita
 * del figlio (0 = tutti i controlli passati, 1 = almeno uno fallito).
 */
static int test_root_access(int drv_fd)
{
    printf("\n========================================\n");
    printf("  TEST 3: controllo accessi root-only\n");
    printf("========================================\n");
    printf("  UID non-root usato : %u\n\n", TARGET_UID);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        /* ---- Processo figlio: diventa non-root ---- */
        if (setresuid(TARGET_UID, TARGET_UID, TARGET_UID) < 0) {
            perror("setresuid nel figlio");
            exit(2);
        }

        int ok = 1;

        /*
         * 1. Ioctl di scrittura: ADD_PROG
         *    Atteso: -1 con errno == EPERM.
         */
        char name[PROG_PATH_MAX] = "/nonexistent/test_accesso";
        int r = ioctl(drv_fd, IOCTL_ADD_PROG, name);
        if (r == -1 && errno == EPERM) {
            printf("  [OK] ADD_PROG  da uid=%u → EPERM\n", TARGET_UID);
        } else {
            printf("  [FAIL] ADD_PROG  da uid=%u: ret=%d errno=%d"
                   " (atteso -1/EPERM)\n", TARGET_UID, r, errno);
            ok = 0;
        }

        /*
         * 2. Ioctl di scrittura: RESET_STATS
         *    Anche il reset delle statistiche è un'operazione di modifica
         *    e deve essere riservato a root.
         */
        r = ioctl(drv_fd, IOCTL_RESET_STATS);
        if (r == -1 && errno == EPERM) {
            printf("  [OK] RESET_STATS da uid=%u → EPERM\n", TARGET_UID);
        } else {
            printf("  [FAIL] RESET_STATS da uid=%u: ret=%d errno=%d"
                   " (atteso -1/EPERM)\n", TARGET_UID, r, errno);
            ok = 0;
        }

        /*
         * 3. Ioctl di sola lettura: LIST_PROGS
         *    Deve riuscire anche per utenti non-root.
         */
        struct throttle_prog_list pl = {0};
        r = ioctl(drv_fd, IOCTL_LIST_PROGS, &pl);
        if (r == 0) {
            printf("  [OK] LIST_PROGS da uid=%u → successo"
                   " (%d prog registrati)\n", TARGET_UID, pl.count);
        } else {
            printf("  [FAIL] LIST_PROGS da uid=%u: ret=%d errno=%d"
                   " (atteso successo)\n", TARGET_UID, r, errno);
            ok = 0;
        }

        exit(ok ? 0 : 1);
    }

    /* ---- Processo padre: attende il figlio ---- */
    int status;
    waitpid(pid, &status, 0);
    int passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    printf("\n  -- Verifica --\n");
    printf("  Controllo accessi root-only: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}

/* ================================================================
 *  TEST 4: Monitor off/on — rilascio dei thread bloccati
 * ================================================================ */

static atomic_long  t4_calls   = 0;
static volatile int t4_running = 1;

static void *t4_worker(void *arg)
{
    (void)arg;
    while (t4_running) {
        syscall(SYS_getpid);
        atomic_fetch_add(&t4_calls, 1);
    }
    return NULL;
}

/*
 * Il test misura il throughput in due fasi consecutive:
 *
 *   Fase A — monitor ON:  i thread vengono throttlati a ≤ MAX/s.
 *             Alcuni resteranno bloccati nella wait_queue del driver.
 *
 *   Fase B — monitor OFF: il driver chiama wake_up_all() e setta
 *             monitor_enabled=0; throttle_check() ritorna immediatamente
 *             per ogni chiamata successiva. I thread bloccati si svegliano
 *             e il throughput torna illimitato.
 *
 * Criteri di PASS:
 *   - rate_A ≤ MAX*1.5  (throttling attivo)
 *   - rate_B > MAX*5    (nessun limite dopo lo spegnimento)
 */
static int test_monitor_toggle(int drv_fd, const char *progpath,
                               int nworkers, int max_val)
{
#define PHASE_SEC 3            /* secondi per ciascuna fase */
    int  sys_nr  = SYS_getpid;
    int  mon     = 1;
    int  prog_ok = 0, sys_ok = 0;
    int  result  = 1;

    printf("\n========================================\n");
    printf("  TEST 4: monitor off/on\n");
    printf("========================================\n");
    printf("  Syscall  : getpid()  (nr=%d)\n", SYS_getpid);
    printf("  Workers  : %d pthread\n", nworkers);
    printf("  MAX      : %d inv/s\n", max_val);
    printf("  Fasi     : %d s ON  +  %d s OFF\n\n", PHASE_SEC, PHASE_SEC);

    /* ---- Configura il driver ---- */
    if (ioctl(drv_fd, IOCTL_ADD_PROG,    progpath) < 0) { perror("ADD_PROG");    goto cleanup; }
    prog_ok = 1;
    if (ioctl(drv_fd, IOCTL_ADD_SYSCALL, &sys_nr)  < 0) { perror("ADD_SYSCALL"); goto cleanup; }
    sys_ok = 1;
    if (ioctl(drv_fd, IOCTL_SET_MAX,     &max_val) < 0) { perror("SET_MAX");     goto cleanup; }
    if (ioctl(drv_fd, IOCTL_SET_MONITOR, &mon)     < 0) { perror("SET_MONITOR"); goto cleanup; }

    /* ---- Lancia i worker ---- */
    pthread_t *th = malloc(nworkers * sizeof(pthread_t));
    for (int i = 0; i < nworkers; i++)
        pthread_create(&th[i], NULL, t4_worker, NULL);

    /* ================================================================
     *  Fase A: monitor ON
     * ================================================================ */
    printf("  [Fase A] monitor ON  — throughput atteso ≤ %d inv/s\n",
           (int)(max_val * 1.5));
    printf("  sec  calls/s  stato\n");
    printf("  ---  -------  -----\n");

    /* Warmup: lascia scadere la finestra parziale del driver */
    sleep(1);
    long prev = atomic_load(&t4_calls);
    long ps_a[PHASE_SEC];
    for (int s = 0; s < PHASE_SEC; s++) {
        sleep(1);
        long cur  = atomic_load(&t4_calls);
        ps_a[s]   = cur - prev;
        prev      = cur;
        printf("  %3d  %7ld  %s\n", s+1, ps_a[s],
               ps_a[s] > (long)(max_val * 1.5) ? "ANOMALIA" : "ok");
    }
    long snap_a = atomic_load(&t4_calls);

    /* ================================================================
     *  Transizione: spegni il monitor
     *  Il driver esegue wake_up_all(&throttle_wq): i thread bloccati
     *  escono dalla wait_queue e completano la loro invocazione.
     * ================================================================ */
    printf("\n  >>> Spegnimento monitor — wake_up_all() nel kernel <<<\n\n");
    { int off = 0; ioctl(drv_fd, IOCTL_SET_MONITOR, &off); }

    /* ================================================================
     *  Fase B: monitor OFF
     * ================================================================ */
    printf("  [Fase B] monitor OFF — throughput atteso > %d inv/s\n",
           max_val * 5);
    printf("  sec  calls/s\n");
    printf("  ---  -------\n");

    prev = atomic_load(&t4_calls);
    long ps_b[PHASE_SEC];
    for (int s = 0; s < PHASE_SEC; s++) {
        sleep(1);
        long cur  = atomic_load(&t4_calls);
        ps_b[s]   = cur - prev;
        prev      = cur;
        printf("  %3d  %7ld\n", s+1, ps_b[s]);
    }

    /* ---- Ferma i thread ---- */
    t4_running = 0;
    for (int i = 0; i < nworkers; i++)
        pthread_join(th[i], NULL);
    free(th);

    /* ---- Calcolo medie sui campioni per-secondo ---- */
    long sum_a = 0, sum_b = 0;
    for (int i = 0; i < PHASE_SEC; i++) { sum_a += ps_a[i]; sum_b += ps_b[i]; }
    double rate_a = (double)sum_a / PHASE_SEC;
    double rate_b = (double)sum_b / PHASE_SEC;
    (void)snap_a;   /* snap_a usato solo per il confronto logico */

    /* ---- Statistiche driver ---- */
    struct throttle_stats st = {0};
    ioctl(drv_fd, IOCTL_GET_STATS, &st);

    /* ---- Verifica ---- */
    int pass_on  = (rate_a <= max_val * 1.5);
    int pass_off = (rate_b >  max_val * 5.0);
    result = (pass_on && pass_off) ? 0 : 1;

    printf("\n  -- Statistiche driver (fase A) --\n");
    printf("  Peak delay    : %lld ns (%.3f ms)\n",
           st.peak_delay_ns, st.peak_delay_ns / 1e6);
    printf("  Peak bloccati : %ld thread\n", st.peak_blocked_threads);
    printf("  Avg  bloccati : %ld thread\n", st.avg_blocked_threads);

    printf("\n  -- Verifica --\n");
    printf("  Fase A: media (%.1f) <= MAX*1.5 (%d): %s\n",
           rate_a, (int)(max_val * 1.5), pass_on  ? "PASS" : "FAIL");
    printf("  Fase B: media (%.1f) >  MAX*5   (%d): %s\n",
           rate_b, max_val * 5,           pass_off ? "PASS" : "FAIL");

cleanup:
    /* monitor già spento nella transizione, ma per sicurezza */
    { int off = 0; ioctl(drv_fd, IOCTL_SET_MONITOR, &off); }
    if (sys_ok)  ioctl(drv_fd, IOCTL_DEL_SYSCALL, &sys_nr);
    if (prog_ok) ioctl(drv_fd, IOCTL_DEL_PROG, progpath);
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
    int r1 = test_blocking     (drv_fd, progpath, nworkers, duration, max_val);
    int r2 = test_uid          (drv_fd,           nworkers, duration, max_val);
    int r3 = test_root_access  (drv_fd);
    int r4 = test_monitor_toggle(drv_fd, progpath, nworkers, max_val);

    close(drv_fd);

    /* ---- Riepilogo finale ---- */
    printf("\n=========================================\n");
    printf("  RIEPILOGO\n");
    printf("=========================================\n");
    printf("  Test 1 (syscall bloccante)   : %s\n",
           r1 == 0 ? "PASS" : r1 < 0 ? "ERRORE" : "FAIL");
    printf("  Test 2 (throttling per UID)  : %s\n",
           r2 == 0 ? "PASS" : r2 < 0 ? "ERRORE" : "FAIL");
    printf("  Test 3 (accesso root-only)   : %s\n",
           r3 == 0 ? "PASS" : r3 < 0 ? "ERRORE" : "FAIL");
    printf("  Test 4 (monitor off/on)      : %s\n",
           r4 == 0 ? "PASS" : r4 < 0 ? "ERRORE" : "FAIL");
    int all_pass = (r1 == 0 && r2 == 0 && r3 == 0 && r4 == 0);
    printf("\n  %s\n", all_pass ? "PASS" : "FAIL");
    printf("=========================================\n");

    return all_pass ? 0 : 1;
}
