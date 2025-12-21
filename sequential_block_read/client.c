#define _GNU_SOURCE
#include "server.h"
#include "blockcopy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

#define DEVICE_BLOCK_SIZE 512

static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

// static _Atomic uint64_t g_read_ns  = 0;
// static _Atomic uint64_t g_write_ns = 0;
// static _Atomic uint64_t g_other_ns = 0;

static uint64_t g_read_ns = 0;
static uint64_t g_write_ns = 0;
static uint64_t g_other_ns = 0;

int *write_pba_1_svc(pba_write_params *params, struct svc_req *rqstp) {

/************ Time Check Start ************/

    struct timespec t_total0, t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    static int result = 0;

    static int fd = -1;
    if (fd == -1) {
        fd = open(DEVICE_PATH, O_RDWR | O_DIRECT);

        if (fd < 0) {
            perror("open device");
            result = -1;
            return &result;
        }
    }

    void *buf;
    if (posix_memalign(&buf, ALIGN, params->nbytes) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        result = -1;
        return &result;
    }

    /************ Read ************/

    struct timespec t_read0, t_read1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_read0);

    ssize_t r = pread(fd, buf, params->nbytes, params->pba_src);
    if (r == -1) {
        perror("pread");
        result = -1;
        free(buf);
        goto exit;
    }
    if ((size_t)r != (size_t)params->nbytes) {
        fprintf(stderr, "read only segments of nbytes: %lu expected, but only %lu\n", (size_t)params->nbytes, (size_t)r);
        result = -1;
        free(buf);
        goto exit;
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_read1);

    /************ Read End ************/

    /************ Write ************/

    struct timespec t_write0, t_write1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_write0);

    ssize_t w = pwrite(fd, (char*)buf, r, params->pba_dst);

    if(w == -1) {
        perror("pwrite");
        result = -1;
        free(buf);
        goto exit;
    }
    if (w < params->nbytes) {
        fprintf(stderr, "written only segments of nbytes: %lu expected, but only %lu\n", (size_t)params->nbytes, (size_t)w);
        result = -1;
        free(buf);
        goto exit;
    }



    clock_gettime(CLOCK_MONOTONIC_RAW, &t_write1);
    if (fsync(fd) < 0) {
        perror("fsync");
        result = -1;
        free(buf);
        goto exit;
    }
    free(buf);

    /************ Write End ************/

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);
    uint64_t read_ns = ns_diff(t_read0, t_read1);
    uint64_t write_ns = ns_diff(t_write0, t_write1);
    uint64_t total_ns = ns_diff(t_total0, t_total1);
    uint64_t other_ns = (total_ns > read_ns + write_ns) ? (total_ns - read_ns - write_ns) : 0;

    // atomic_fetch_add_explicit(&g_read_ns, read_ns, memory_order_relaxed);
    // atomic_fetch_add_explicit(&g_write_ns, write_ns, memory_order_relaxed);
    // atomic_fetch_add_explicit(&g_other_ns, other_ns, memory_order_relaxed);

    g_read_ns += read_ns;
    g_write_ns += write_ns;
    g_other_ns += other_ns;

    //******************** added********************************
    //uint64_t total_ns = ns_diff(t_total0, t_total1);
    //uint64_t other_ns = (total_ns > read_ns + write_ns) ? (total_ns - read_ns - write_ns) : 0;
    // *********************************************************
/************ Time Check End ************/

exit:
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
