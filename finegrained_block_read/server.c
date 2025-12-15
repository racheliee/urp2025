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
#define min(a,b) (( (a) < (b) ) ? (a) : (b))

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
    // result of read value
    static char read_value[MAX_BYTES];

    // return value
    static finegrained_read_returns out;
    out.value.value_val = read_value;

    static struct timespec t_total0, t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);
    
    static int fd = -1;
    if (fd == -1) {
        fd = open(DEVICE_PATH, O_RDONLY | O_DIRECT);

        if (fd < 0) {
            perror("open device");
            out.value.value_len = 0;
            return &out;
        }
    }

    // for time measure
    uint64_t read_ns = 0;

    // target len to read
    int target_read_bytes_num = params->read_bytes;
    if(target_read_bytes_num > MAX_BYTES) {
        fprintf(stderr, "read bytes should be less than %zu, but current %zu.\n", (size_t)MAX_BYTES, (size_t)target_read_bytes_num);
        out.value.value_len = 0;
        return &out;
    }

    // current read bytes
    int read_bytes_num = 0;

    // pba
    int pbas_len = params->pba.pba_len;
    finegrained_pba* pbas = params->pba.pba_val;

    for(int i = 0; i < pbas_len; ++i) {
        int64_t pba = pbas[i].pba;
        int extent_bytes = pbas[i].extent_bytes;
        int offset = pbas[i].offset;
        int length = pbas[i].length;

        if(offset < 0 || offset >= length) {
            fprintf(stderr, "offset error. should be greater or equal then 0, less than length: %d\n", length);
            out.value.value_len = 0;
            return &out;
        }

        void *buf;
        if (posix_memalign(&buf, ALIGN, extent_bytes) != 0) {
            fprintf(stderr, "posix_memalign failed\n");
            out.value.value_len = 0;
            return &out;
        }

        /************ Read ************/

        static struct timespec t_read0, t_read1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read0);

        ssize_t r = pread(fd, buf, extent_bytes, pba);
        if (r == -1) { 
            perror("pread");
            free(buf);
            out.value.value_len = 0;
            return &out;
        }
        if ((size_t)r != (size_t)extent_bytes) {
            fprintf(stderr, "read only segments of extent_bytes: %zu expected, but only %zu\n", (size_t)extent_bytes, (size_t)r);
            free(buf);
            out.value.value_len = 0;
            return &out;
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read1);
        read_ns += ns_diff(t_read0, t_read1);

        /************ Read End ************/

        memcpy(read_value + read_bytes_num, (char*)buf + offset, length);
        
        read_bytes_num += length;
        free(buf);
    }

    out.value.value_len = (u_int) read_bytes_num;

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);

    uint64_t total_ns = ns_diff(t_total0, t_total1);
    g_other_ns += (total_ns > read_ns ? total_ns - read_ns : 0);
    g_read_ns += read_ns;

    return &out;
}

int *write_1_svc(finegrained_write_params *params, struct svc_req *rqstp) {
    static int result = 0;

    static struct timespec t_total0, t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);
    
    static int fd = -1;
    if (fd == -1) {
        fd = open(DEVICE_PATH, O_RDWR | O_DIRECT);

        if (fd < 0) {
            perror("open device");
            result = -1;
            return &result;
        }
    }

    // for time measure
    uint64_t read_ns = 0;
    uint64_t write_ns = 0;

    // write value information
    char* write_value = params->value.value_val;
    int target_write_bytes_num = params->value.value_len;

    // current read bytes
    int write_bytes_num = 0;

    // pba
    int pbas_len = params->pba.pba_len;
    finegrained_pba* pbas = params->pba.pba_val;

    for(int i = 0; i < pbas_len; ++i) {
        int64_t pba = pbas[i].pba;
        int extent_bytes = pbas[i].extent_bytes;
        int offset = pbas[i].offset;
        int length = pbas[i].length;

        if(offset < 0 || offset >= length) {
            fprintf(stderr, "offset error. should be greater or equal then 0, less than length: %d\n", length);
            result = -1;
            return &result;
        }

        void *buf;
        if (posix_memalign(&buf, ALIGN, extent_bytes) != 0) {
            fprintf(stderr, "posix_memalign failed\n");
            result = -1;
            return &result;
        }

        /************ Read ************/

        static struct timespec t_read0, t_read1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read0);

        ssize_t r = pread(fd, buf, extent_bytes, pba);
        if (r == -1) { 
            perror("pread");
            free(buf);
            result = -1;
            return &result;
        }
        if ((size_t)r != (size_t)extent_bytes) {
            fprintf(stderr, "read only segments of extent_bytes: %zu expected, but only %zu\n", (size_t)extent_bytes, (size_t)r);
            free(buf);
            result = -1;
            return &result;
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read1);
        read_ns += ns_diff(t_read0, t_read1);

        /************ Read End ************/

        memcpy(buf+offset, write_value+write_bytes_num, length);
        
        /************ Write ************/

        struct timespec t_write0, t_write1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write0);

        ssize_t w = pwrite(fd, buf, extent_bytes, pba);
        if(w == -1) {
            perror("pwrite");
            result = -1;
            free(buf);
            return &result;
        }
        if (w < extent_bytes) {
            fprintf(stderr, "written only segments of extent_bytes: %zu expected, but only %zu\n", (size_t)extent_bytes, (size_t)w);
            result = -1;
            free(buf);
            return &result;
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write1);
        write_ns += ns_diff(t_write0, t_write1);

        /************ Write End ************/

        write_bytes_num += length;
        free(buf);
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);

    uint64_t total_ns = ns_diff(t_total0, t_total1);
    g_read_ns += read_ns;
    g_write_ns += write_ns;
    g_other_ns += (total_ns > read_ns + write_ns ? total_ns - read_ns - write_ns : 0);
    
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
