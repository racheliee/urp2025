#define _GNU_SOURCE
#include "server_random.h"
#include "blockcopy_random.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEVICE_BLOCK_SIZE 512

static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

// static _Atomic uint64_t g_read_ns  = 0;
// static _Atomic uint64_t g_write_ns = 0;
// static _Atomic uint64_t g_other_ns = 0;

static uint64_t g_read_ns = 0;
static uint64_t g_write_ns = 0;
static uint64_t g_other_ns = 0;

int *write_pba_batch_1_svc(pba_batch_params *params, struct svc_req *rqstp) {
    static int result = 0;

    struct timespec t_total0, t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    static int fd = -1;
    if (fd == -1) {
        fd = open(DEVICE_PATH, O_RDWR | O_DIRECT);
        if (fd < 0) {
            perror("open");
            result = -1;
            return &result;
        }
    }

    void *buf;
    if (posix_memalign(&buf, ALIGN, params->block_size) != 0) {
        perror("posix_memalign");
        result = -1;
        return &result;
    }

    uint64_t total_read_ns = 0;
    uint64_t total_write_ns = 0;

    for (int i = 0; i < params->count; i++) {

        /* --- READ PHASE --- */
        struct timespec t_read0, t_read1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read0);

        ssize_t r = pread(fd, buf, params->block_size, params->pba_srcs[i]);

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read1);
        total_read_ns += ns_diff(t_read0, t_read1);

        if (r != params->block_size) {
            perror("pread");
            result = -1;
            break;
        }

        /* --- WRITE PHASE --- */
        struct timespec t_write0, t_write1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write0);

        ssize_t w = pwrite(fd, buf, params->block_size, params->pba_dsts[i]);

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write1);
        total_write_ns += ns_diff(t_write0, t_write1);

        if (w != params->block_size) {
            perror("pwrite");
            result = -1;
            break;
        }
    }

    free(buf);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);
    uint64_t total_ns = ns_diff(t_total0, t_total1);

    uint64_t other_ns = (total_ns > total_read_ns + total_write_ns)
                            ? (total_ns - total_read_ns - total_write_ns)
                            : 0;

    /* Accumulate into global timing counters */
    g_read_ns += total_read_ns;
    g_write_ns += total_write_ns;
    g_other_ns += other_ns;

    return &result;
}

get_server_ios *get_time_1_svc(void *argp, struct svc_req *rqstp) {
    static get_server_ios out;
    // out.server_read_time = atomic_load_explicit(&g_read_ns, memory_order_relaxed);
    // out.server_write_time = atomic_load_explicit(&g_write_ns, memory_order_relaxed);
    // out.server_other_time = atomic_load_explicit(&g_other_ns, memory_order_relaxed);

    out.server_read_time = g_read_ns;
    out.server_write_time = g_write_ns;
    out.server_other_time = g_other_ns;

    return &out;
}

void *reset_time_1_svc(void *argp, struct svc_req *rqstp) {
    static char dummy;
    g_read_ns = g_write_ns = g_other_ns = 0;

    fprintf(stdout, "server time reset complete.\n");
    fflush(stdout);
    return (void *)&dummy;
}