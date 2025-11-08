#include "server.h"
#include "blockcopy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <tirpc/rpc/rpc.h>

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

bool_t write_pba_1_svc(pba_write_params *params, int *result, struct svc_req *rqstp) {
    
/************ Time Check Start ************/
    (void) rqstp;

    struct timespec t_total0, t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    *result = -1;
    
    static int fd = -1;
    if (fd == -1) {
        fd = open(DEVICE_PATH, O_RDWR | O_DIRECT);

        if (fd < 0) {
            perror("open device");
            goto exit;
        }
    }

    void *buf;
    if (posix_memalign(&buf, ALIGN, params->nbytes) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        goto exit;
    }

    /************ Read ************/

    struct timespec t_read0, t_read1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_read0);

    ssize_t r = pread(fd, buf, params->nbytes, params->pba_src);
    if (r == -1) { 
        perror("pread");
        free(buf);
        goto exit;
    }
    if ((size_t)r != (size_t)params->nbytes) {
        fprintf(stderr, "read only segments of nbytes: %lu expected, but only %lu\n", (size_t)params->nbytes, (size_t)r);
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
        free(buf);
        goto exit;
    }
    if (w < params->nbytes) {
        fprintf(stderr, "written only segments of nbytes: %lu expected, but only %lu\n", (size_t)params->nbytes, (size_t)w);
        free(buf);
        goto exit;
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_write1);
    free(buf);

    /************ Write End ************/

    uint64_t read_ns = ns_diff(t_read0, t_read1);
    uint64_t write_ns = ns_diff(t_write0, t_write1);
    uint64_t total_ns = ns_diff(t_total0, t_total1);
    uint64_t other_ns = (total_ns > read_ns + write_ns)
                                  ? (total_ns - read_ns - write_ns) : 0;

    // atomic_fetch_add_explicit(&g_read_ns, read_ns, memory_order_relaxed);
    // atomic_fetch_add_explicit(&g_write_ns, write_ns, memory_order_relaxed);
    // atomic_fetch_add_explicit(&g_other_ns, other_ns, memory_order_relaxed);

    g_read_ns += read_ns;
    g_write_ns += write_ns;
    g_other_ns += other_ns;

    t_total1 = t_write1;

    *result = 0;
/************ Time Check End ************/

exit:
    return TRUE;
}

bool_t get_time_1_svc(void *argp, struct get_server_ios *result, struct svc_req *rqstp) {
    (void)argp; (void)rqstp;
    // out.server_read_time = atomic_load_explicit(&g_read_ns, memory_order_relaxed);
    // out.server_write_time = atomic_load_explicit(&g_write_ns, memory_order_relaxed);
    // out.server_other_time = atomic_load_explicit(&g_other_ns, memory_order_relaxed);

    result->server_read_time = g_read_ns;
    result->server_write_time = g_write_ns;
    result->server_other_time = g_other_ns;

    return TRUE;
}

bool_t reset_time_1_svc(void *argp, void *result, struct svc_req *rqstp) {
    (void)argp; (void)result; (void)rqstp;
    g_read_ns = g_write_ns = g_other_ns = 0;

    fprintf(stdout, "server time reset complete.\n");
    fflush(stdout);
    return TRUE;
}


bool_t blockcopy_prog_1_freeresult(SVCXPRT *transp, xdrproc_t xdr_result, caddr_t result)
{
    (void)transp;
    (void)xdr_result;
    (void)result;
    
    return TRUE;
}