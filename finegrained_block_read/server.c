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

finegrained_read_returns *read_1_svc(finegrained_read_params *params, struct svc_req *rqstp) {
    static finegrained_read_returns out;
    out.value.value_len = 1;

    char dum_value[4] = "1234"; // null does not included to dum_value;
    out.value.value_val = malloc(4);
    memcpy(out.value.value_val, dum_value, 4);

    return &out;
}

void *write_1_svc(finegrained_write_params *params, struct svc_req *rqstp) {
    return (void *)&dummy;
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