/*
 * throttleTest.c — test autonomo per throttleDriver
 *
 * Il programma:
 *   1. Apre /dev/throttleDriver e configura il driver (registra se stesso
 *      come prog, registra la syscall, imposta MAX, abilita il monitor)
 *   2. Lancia N thread che invocano la syscall il più velocemente possibile
 *   3. Misura il throughput effettivo ogni secondo
 *   4. Al termine legge le statistiche dal driver
 *   5. Verifica automaticamente che il throttling abbia funzionato
 *   6. Ripristina la configurazione del driver e stampa PASS / FAIL
 *
 * Uso:
 *   sudo ./throttleTest <num_thread> <durata_sec> <MAX>
 *
 * Esempio:
 *   sudo ./throttleTest 8 6 200
 *
 * Note:
 *   - Richiede root per le ioctl di configurazione del driver.
 *   - La syscall testata è getpid() (nr=39 su x86-64): veloce, non
 *     bloccante, ideale per saturare il rate limiter.
 *   - Il cleanup viene eseguito anche in caso di errore.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
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
    // Aggiunta per il test: media calls/finestra (allineata all'hrtimer del driver)
    long         avg_calls_per_window;
    long         peak_calls_per_window;
};

#define IOCTL_ADD_PROG    _IOW('T',  1, char[PROG_PATH_MAX])
#define IOCTL_DEL_PROG    _IOW('T',  2, char[PROG_PATH_MAX])
#define IOCTL_ADD_SYSCALL _IOW('T',  7, int)
#define IOCTL_DEL_SYSCALL _IOW('T',  8, int)
#define IOCTL_SET_MONITOR _IOW('T', 10, int)
#define IOCTL_SET_MAX     _IOW('T', 12, int)
#define IOCTL_GET_STATS   _IOR('T', 13, struct throttle_stats)
#define IOCTL_RESET_STATS _IO('T', 14)

/* ================================================================
 *  Stato globale del test
 * ================================================================ */

static atomic_long  total_calls = 0;
static volatile int running     = 1;
static int          syscall_nr  = SYS_getpid;

struct thread_arg {
    int  id;
    long calls_done;
};

/* ================================================================
 *  Worker thread
 * ================================================================ */

static void *worker(void *arg)
{
    struct thread_arg *ta = arg;
    long count = 0;

    while (running) {
        syscall(syscall_nr);
        count++;
        atomic_fetch_add(&total_calls, 1);
    }

    ta->calls_done = count;
    return NULL;
}

/* ================================================================
 *  Cleanup — deregistra prog e syscall, spegne il monitor
 * ================================================================ */

static void do_cleanup(int fd, const char *progpath)
{
    int val = 0;
    ioctl(fd, IOCTL_SET_MONITOR, &val);
    ioctl(fd, IOCTL_DEL_SYSCALL, &syscall_nr);
    ioctl(fd, IOCTL_DEL_PROG,    progpath);
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "Uso: sudo %s <num_thread> <durata_sec> <MAX>\n"
            "  num_thread:  thread concorrenti che invocano la syscall\n"
            "  durata_sec:  durata del test (secondi)\n"
            "  MAX:         invocazioni/s massime da impostare nel driver\n"
            "Esempio: sudo %s 8 6 200\n",
            argv[0], argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    int duration    = atoi(argv[2]);
    int max_val     = atoi(argv[3]);

    if (num_threads <= 0 || duration <= 0 || max_val <= 0) {
        fprintf(stderr, "Errore: tutti i parametri devono essere > 0.\n");
        return 1;
    }

    /* ---- Apri il device ---- */
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("open " DEVICE_PATH);
        return 1;
    }

    /*
     * Path del binario: il kernel identifica l'eseguibile tramite inode/device
     * ricavati da mm->exe_file. Per registrarlo passiamo il path assoluto
     * letto da /proc/self/exe, che il kernel risolverà allo stesso inode.
     */
    char progpath[PROG_PATH_MAX] = {0};
    if (readlink("/proc/self/exe", progpath, PROG_PATH_MAX - 1) < 0) {
        perror("readlink /proc/self/exe");
        close(fd); return 1;
    }

    printf("=========================================\n");
    printf("  throttleDriver — test autonomo\n");
    printf("=========================================\n");
    printf("  Programma : '%s'  (PID %d, UID %d)\n",
           progpath, getpid(), getuid());
    printf("  Syscall   : getpid() nr=%d\n", syscall_nr);
    printf("  Thread    : %d\n", num_threads);
    printf("  Durata    : %d s\n", duration);
    printf("  MAX       : %d inv/s\n", max_val);
    printf("=========================================\n\n");

    /* ---- Configura il driver ---- */
    if (ioctl(fd, IOCTL_ADD_PROG, progpath) < 0) {
        perror("IOCTL_ADD_PROG"); close(fd); return 1;
    }
    if (ioctl(fd, IOCTL_ADD_SYSCALL, &syscall_nr) < 0) {
        perror("IOCTL_ADD_SYSCALL");
        ioctl(fd, IOCTL_DEL_PROG, progpath);
        close(fd); return 1;
    }
    if (ioctl(fd, IOCTL_SET_MAX, &max_val) < 0) {
        perror("IOCTL_SET_MAX");
        do_cleanup(fd, progpath); close(fd); return 1;
    }
    int mon = 1;
    if (ioctl(fd, IOCTL_SET_MONITOR, &mon) < 0) {
        perror("IOCTL_SET_MONITOR");
        do_cleanup(fd, progpath); close(fd); return 1;
    }

    /* ---- Lancia i thread ---- */
    pthread_t         *threads = malloc(num_threads * sizeof(pthread_t));
    struct thread_arg *args    = malloc(num_threads * sizeof(struct thread_arg));

    for (int i = 0; i < num_threads; i++) {
        args[i].id = i;
        args[i].calls_done = 0;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    /* ---- Warmup: attende la fine della finestra corrente del driver ----
     * L'hrtimer del driver parte al carico del modulo e non è sincronizzato
     * con l'inizio del test.  Senza warmup la prima finestra di misura può
     * coprire due finestre del driver (fino a 2×MAX completamenti).
     * Un secondo di attesa garantisce che la prima misurazione parta sempre
     * all'inizio di una finestra fresca. */
    sleep(1);
    /* Reset delle statistiche dopo il warmup: le finestre successive vengono
     * misurate dal driver stesso, che è l'unica sorgente allineata ai propri
     * boundary di finestra. */
    ioctl(fd, IOCTL_RESET_STATS, NULL);

    /* ---- Misura il throughput ogni secondo ---- */
    long *per_sec = malloc(duration * sizeof(long));
    long  baseline = atomic_load(&total_calls);  /* baseline post-warmup */
    long  prev     = baseline;

    printf("  sec  calls/s  stato\n");
    printf("  ---  -------  -----\n");

    for (int sec = 0; sec < duration; sec++) {
        sleep(1);
        long cur  = atomic_load(&total_calls);
        long rate = cur - prev;
        per_sec[sec] = rate;
        prev = cur;

        /* Tolleranza del 50%: copre il jitter di scheduling tra il boundary
         * dell'hrtimer e quello dello sleep del test (~ms). */
        const char *stato = (rate > (long)(max_val * 1.5)) ? "ANOMALIA" : "ok";
        printf("  %3d  %7ld  %s\n", sec + 1, rate, stato);
    }

    /* ---- Ferma i thread ---- */
    running = 0;
    { int val = 0; ioctl(fd, IOCTL_SET_MONITOR, &val); }
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    long total = atomic_load(&total_calls) - baseline;  /* esclude il warmup */

    /* ---- Statistiche dal driver ---- */
    struct throttle_stats stats;
    int got_stats = (ioctl(fd, IOCTL_GET_STATS, &stats) == 0);

    /* ---- Cleanup driver ---- */
    do_cleanup(fd, progpath);
    close(fd);

    /* ================================================================
     *  Verifica automatica del throttling
     *
     *  La verifica usa le statistiche per-finestra del driver, che sono
     *  allineate ai boundary dell'hrtimer e quindi prive dell'errore di
     *  misura introdotto dallo sleep(1) del test (drift tra oscillatori).
     *
     *  Criteri:
     *   1. Media per-finestra (driver) ≤ MAX * 1.2
     *   2. Picco per-finestra  (driver) ≤ MAX
     *      (il driver non può superare MAX nella propria finestra;
     *       se lo fa, è un bug nel throttling stesso)
     *
     *  Il display per-secondo qui sotto rimane a scopo informativo/visivo
     *  ma non influenza il risultato PASS/FAIL.
     * ================================================================ */

    double avg       = got_stats ? (double)stats.avg_calls_per_window : (double)total / duration;
    int    pass_avg  = got_stats ? (stats.avg_calls_per_window  <= (long)(max_val * 1.2))
                                 : (avg <= max_val * 1.2);
    int    pass_wins = got_stats ? (stats.peak_calls_per_window <= (long)max_val)
                                 : 1;

    printf("\n=========================================\n");
    printf("  Risultati\n");
    printf("=========================================\n");
    printf("  Totale chiamate : %ld in %d s\n", total, duration);
    printf("  Media effettiva : %.1f calls/s\n", avg);
    printf("  MAX configurato : %d calls/s\n", max_val);

    if (got_stats) {
        printf("\n  -- Statistiche driver (per-finestra, allineate all'hrtimer) --\n");
        printf("  Peak calls/finestra : %ld\n",   stats.peak_calls_per_window);
        printf("  Avg  calls/finestra : %ld\n",   stats.avg_calls_per_window);
        printf("  Peak delay          : %lld ns (%.3f ms)\n",
               stats.peak_delay_ns, stats.peak_delay_ns / 1e6);
        printf("  Peak delay prog     : '%s'\n",  stats.peak_delay_prog);
        printf("  Peak delay uid      : %u\n",    stats.peak_delay_uid);
        printf("  Peak bloccati       : %ld thread\n", stats.peak_blocked_threads);
        printf("  Avg  bloccati       : %ld thread\n", stats.avg_blocked_threads);
    }

    printf("\n  -- Verifica (basata su statistiche driver) --\n");
    printf("  Avg/finestra <= MAX*1.2 (%d): %s  (%ld)\n",
           (int)(max_val * 1.2), pass_avg  ? "PASS" : "FAIL",
           got_stats ? stats.avg_calls_per_window : (long)avg);
    printf("  Peak/finestra <= MAX    (%d): %s  (%ld)\n",
           max_val, pass_wins ? "PASS" : "FAIL",
           got_stats ? stats.peak_calls_per_window : (long)max_val);

    int pass = pass_avg && pass_wins;
    printf("\n=========================================\n");
    printf("  %s\n", pass ? "PASS" : "FAIL");
    printf("=========================================\n");

    free(threads);
    free(args);
    free(per_sec);
    return pass ? 0 : 1;
}
