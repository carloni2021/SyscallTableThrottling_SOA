/*
 * throttle_ctl.c — tool userspace per configurare throttleDriver
 *
 * Uso:
 *   sudo ./throttle_ctl add_prog  <nome>
 *   sudo ./throttle_ctl del_prog  <nome>
 *   sudo ./throttle_ctl add_uid   <uid>
 *   sudo ./throttle_ctl del_uid   <uid>
 *   sudo ./throttle_ctl add_sys   <nr>
 *   sudo ./throttle_ctl del_sys   <nr>
 *   sudo ./throttle_ctl monitor   <0|1>
 *   sudo ./throttle_ctl set_max   <N>
 *        ./throttle_ctl status
 *        ./throttle_ctl stats
 *        ./throttle_ctl reset_stats
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#define TASK_COMM_LEN    16
#define PROG_PATH_MAX    256
#define DEVICE_PATH      "/dev/throttleDriver"

/* Devono coincidere con throttleDriver.c */

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

#define IOCTL_ADD_PROG      _IOW('T',  1, char[PROG_PATH_MAX])
#define IOCTL_DEL_PROG      _IOW('T',  2, char[PROG_PATH_MAX])
#define IOCTL_ADD_UID       _IOW('T',  4, unsigned int)
#define IOCTL_DEL_UID       _IOW('T',  5, unsigned int)
#define IOCTL_ADD_SYSCALL   _IOW('T',  7, int)
#define IOCTL_DEL_SYSCALL   _IOW('T',  8, int)
struct throttle_status {
    int monitor_enabled;
    int max_calls;
    int current_count;
};

#define IOCTL_SET_MONITOR   _IOW('T', 10, int)
#define IOCTL_GET_STATUS    _IOR('T', 11, struct throttle_status)
#define IOCTL_SET_MAX       _IOW('T', 12, int)
#define IOCTL_GET_STATS     _IOR('T', 13, struct throttle_stats)
#define IOCTL_RESET_STATS   _IO ('T', 14)

static int open_dev(void)
{
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("open " DEVICE_PATH);
        exit(1);
    }
    return fd;
}

static void usage(void)
{
    fprintf(stderr,
        "Uso:\n"
        "  throttle_ctl add_prog  <nome>   — registra nome programma\n"
        "  throttle_ctl del_prog  <nome>   — deregistra nome programma\n"
        "  throttle_ctl add_uid   <uid>    — registra UID\n"
        "  throttle_ctl del_uid   <uid>    — deregistra UID\n"
        "  throttle_ctl add_sys   <nr>     — registra syscall number\n"
        "  throttle_ctl del_sys   <nr>     — deregistra syscall number\n"
        "  throttle_ctl monitor   <0|1>    — off/on il monitor\n"
        "  throttle_ctl set_max   <N>      — imposta MAX inv/sec\n"
        "  throttle_ctl status             — mostra stato corrente\n"
        "  throttle_ctl stats              — mostra statistiche\n"
        "  throttle_ctl reset_stats        — azzera le statistiche (solo root)\n"
    );
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 2) usage();

    int fd = open_dev();
    const char *cmd = argv[1];
    int ret = 0;

    if (strcmp(cmd, "add_prog") == 0 || strcmp(cmd, "del_prog") == 0) {
        if (argc < 3) usage();
        char path[PROG_PATH_MAX] = {0};
        strncpy(path, argv[2], PROG_PATH_MAX - 1);
        unsigned long ioc = (strcmp(cmd, "add_prog") == 0)
                            ? IOCTL_ADD_PROG : IOCTL_DEL_PROG;
        ret = ioctl(fd, ioc, path);

    } else if (strcmp(cmd, "add_uid") == 0 || strcmp(cmd, "del_uid") == 0) {
        if (argc < 3) usage();
        unsigned int uid = (unsigned int)atoi(argv[2]);
        unsigned long ioc = (strcmp(cmd, "add_uid") == 0)
                            ? IOCTL_ADD_UID : IOCTL_DEL_UID;
        ret = ioctl(fd, ioc, &uid);

    } else if (strcmp(cmd, "add_sys") == 0 || strcmp(cmd, "del_sys") == 0) {
        if (argc < 3) usage();
        int nr = atoi(argv[2]);
        unsigned long ioc = (strcmp(cmd, "add_sys") == 0)
                            ? IOCTL_ADD_SYSCALL : IOCTL_DEL_SYSCALL;
        ret = ioctl(fd, ioc, &nr);

    } else if (strcmp(cmd, "monitor") == 0) {
        if (argc < 3) usage();
        int val = atoi(argv[2]);
        ret = ioctl(fd, IOCTL_SET_MONITOR, &val);

    } else if (strcmp(cmd, "set_max") == 0) {
        if (argc < 3) usage();
        int val = atoi(argv[2]);
        ret = ioctl(fd, IOCTL_SET_MAX, &val);

    } else if (strcmp(cmd, "status") == 0) {
        struct throttle_status st;
        ret = ioctl(fd, IOCTL_GET_STATUS, &st);
        if (ret == 0) {
            printf("=== Stato throttleDriver ===\n");
            printf("  Monitor : %s\n", st.monitor_enabled ? "ON" : "OFF");
            printf("  MAX     : %d inv/s\n", st.max_calls);
            printf("  Count   : %d (finestra corrente)\n", st.current_count);
        }

    } else if (strcmp(cmd, "stats") == 0) {
        struct throttle_stats s;
        ret = ioctl(fd, IOCTL_GET_STATS, &s);
        if (ret == 0) {
            printf("=== Statistiche throttleDriver ===\n");
            printf("  Totale chiamate:      %lld\n",   s.total_calls);
            printf("  Peak calls/finestra:  %ld\n",   s.peak_calls_per_window);
            printf("  Avg  calls/finestra:  %ld\n",   s.avg_calls_per_window);
            printf("  Peak delay:           %lld ns (%.3f ms)\n",
                   s.peak_delay_ns, s.peak_delay_ns / 1e6);
            printf("  Avg  delay:           %lld ns (%.3f ms)\n",
                   s.avg_delay_ns, s.avg_delay_ns / 1e6);
            printf("  Peak delay prog:      '%s'\n",  s.peak_delay_prog);
            printf("  Peak delay uid:       %u\n",    s.peak_delay_uid);
            printf("  Peak thread bloccati: %ld\n",   s.peak_blocked_threads);
            printf("  Avg  thread bloccati: %ld\n",   s.avg_blocked_threads);
        }

    } else if (strcmp(cmd, "reset_stats") == 0) {
        ret = ioctl(fd, IOCTL_RESET_STATS, 0);
        if (ret == 0)
            printf("Statistiche azzerate.\n");

    } else {
        usage();
    }

    if (ret < 0) {
        perror("ioctl");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}