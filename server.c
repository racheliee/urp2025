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

#define ull unsigned long long
#define DEVICE_BLOCK_SIZE 512

static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

u_quad_t *write_pba_1_svc(pba_write_params *params, struct svc_req *rqstp) {
    static u_quad_t elapsed_ns = 0;

    struct timespec t_before, t_after;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_before);

    static int fd = -1;
    if (fd == -1) {
        fd = open(DEVICE_PATH, O_RDWR | O_DIRECT);

        if (fd < 0) {
            perror("open device");
            elapsed_ns = (u_quad_t)-1;
            return &elapsed_ns;
        }
    }

    void *buf;
    if (posix_memalign(&buf, ALIGN, params->nbytes) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        elapsed_ns = (u_quad_t)-1;
        return &elapsed_ns;
    }

    ssize_t r = pread(fd, buf, params->nbytes, params->pba_src);
    if (r == -1) { 
        perror("pread");
        elapsed_ns = (u_quad_t)-1;
        goto exit;
    }
    if ((size_t)r != (size_t)params->nbytes) {
        fprintf(stderr, "read only segments of nbytes: %lu expected, but only %lu\n", (size_t)params->nbytes, (size_t)r);
        elapsed_ns = (u_quad_t)-1;
        goto exit;
    }
    ssize_t w = pwrite(fd, (char*)buf, r, params->pba_dst);

    if (w < params->nbytes) {
        fprintf(stderr, "written only segments of nbytes: %lu expected, but only %lu\n", (size_t)params->nbytes, (size_t)w);
        elapsed_ns = (u_quad_t)-1;
        goto exit;
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_after);
    elapsed_ns = (u_quad_t)ns_diff(t_before, t_after);

exit:
    free(buf);

    return &elapsed_ns;
}
