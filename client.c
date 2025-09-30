#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <rpc/rpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "blockcopy.h"
#include "client.h"

/* helper to get physical block address from logical offset */
static int get_pba(int fd, off_t logical, uint64_t *pba_out) {
    size_t size = sizeof(struct fiemap) + sizeof(struct fiemap_extent);
    struct fiemap *fiemap = calloc(1, size);

    fiemap->fm_start = logical;
    fiemap->fm_length = 4096; /* map one block */
    fiemap->fm_extent_count = 1;

    if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
        perror("ioctl fiemap");
        free(fiemap);
        return -1;
    }
    if (fiemap->fm_mapped_extents == 0) {
        fprintf(stderr, "no extents mapped at logical %ld\n", (long)logical);
        free(fiemap);
        return -1;
    }

    struct fiemap_extent *e = &fiemap->fm_extents[0];
    *pba_out = e->fe_physical + (logical - e->fe_logical);

    free(fiemap);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_host> <src_file> [-b block_size] [-n iterations]\n", argv[0]);
        return 1;
    }

    const char *server_host = argv[1];
    const char *src_path = argv[2];

    size_t block_size = DEFAULT_BLOCK_SIZE;
    long iterations = DEFAULT_ITERS;

    int opt;
    while ((opt = getopt(argc - 2, argv + 2, "b:n:")) != -1) {
        switch (opt) {
        case 'b':
            block_size = strtoul(optarg, NULL, 10);
            if (!(block_size == 4096 || block_size == 8192 || block_size == 16384)) {
                fprintf(stderr, "Block size must be 4096, 8192, or 16384\n");
                exit(1);
            }
            break;
        case 'n':
            iterations = strtol(optarg, NULL, 10);
            break;
        }
    }

    CLIENT *clnt = clnt_create(server_host, BLOCKCOPY_PROG, BLOCKCOPY_VERS, "tcp");
    if (!clnt) {
        clnt_pcreateerror(server_host);
        exit(1);
    }

    int fd = open(src_path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open src");
        exit(1);
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        exit(1);
    }
    off_t filesize = st.st_size;

    void *buf;
    if (posix_memalign(&buf, ALIGN, block_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    srand((unsigned)time(NULL));

    struct timespec start_time, end_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        perror("clock_gettime start");
    }

    for (long i = 0; i < iterations; i++) {
        off_t max_blocks = filesize / block_size;
        off_t block = rand() % max_blocks;
        off_t logical = block * block_size;

        uint64_t pba;
        if (get_pba(fd, logical, &pba) != 0)
            continue;

        ssize_t r = pread(fd, buf, block_size, logical);
        if (r < 0) {
            perror("pread");
            break;
        }

        pba_write_params params;
        params.pba = pba;
        params.data.data_val = buf;
        params.data.data_len = r;
        params.nbytes = r;

        int *res = write_pba_1(&params, clnt);
        if (!res || *res != 0) {
            fprintf(stderr, "RPC write failed at PBA %lu\n", (unsigned long)pba);
            break;
        }
    }

    free(buf);
    close(fd);
    clnt_destroy(clnt);

    if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
        perror("clock_gettime end");
    }

    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("\n=== Client Stats ===\n");
    printf("  Iterations attempted: %ld\n", iterations);
    printf("  Block size: %zu bytes\n", block_size);
    printf("  Elapsed time: %.3f seconds\n", elapsed);
    printf("  Approx throughput: %.2f MB/s\n",
           ((iterations * block_size) / (1024.0 * 1024.0)) / elapsed);

    return 0;
}
