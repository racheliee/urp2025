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

typedef struct {
    uint64_t pba;
    size_t len;
} pba_seg;

/* helper to get physical block address from logical offset */
static int get_pba(int fd, off_t logical, size_t length, pba_seg **out, size_t* out_cnt) {
    size_t size = sizeof(struct fiemap) + EXTENTS_MAX * sizeof(struct fiemap_extent);
    struct fiemap *fiemap = (struct fiemap*)calloc(1, size);
    if(!fiemap) return -1;

    fiemap->fm_start = logical;
    fiemap->fm_length = length;
    fiemap->fm_extent_count = EXTENTS_MAX;

    if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
        perror("ioctl fiemap");
        free(fiemap);
        return -1;
    }
    if (fiemap->fm_mapped_extents > EXTENTS_MAX) {
        fprintf(stderr, "More mapped extentes needed: mapped %ld, but need %lu\n", (long)EXTENTS_MAX, fiemap->fm_mapped_extents);
        free(fiemap);
        return -1;
    }
    if (fiemap->fm_mapped_extents == 0) {
        fprintf(stderr, "no extents mapped at logical %ld\n", (long)logical);
        free(fiemap);
        return -1;
    }

    pba_seg *vec = calloc(fiemap->fm_mapped_extents, sizeof(pba_seg));
    size_t n = 0;
    if(!vec) {
        free(fiemap);
        return -1;
    }

    for(size_t i = 0; i < fiemap->fm_mapped_extents; ++i) {
        struct fiemap_extent *e = &fiemap->fm_extents[i];
        // TODO: 8KB 이상의 block에 Test 하려면 코드 수정 필요.

        vec[n].pba = e->fe_physical + (logical - e->fe_logical);
        vec[n].len = length;
        n++;
    }

    if (n == 0) { free(vec); free(fiemap); return -1; }
    *out = vec, *out_cnt = n;
    free(fiemap);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <server_eternity> <file_path> [-b block_size] [-n iterations] [-s seed] [-l log]\n"
        "Options:\n"
        "  -b block_number # of block number. Block is 4096B. (default: 1)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n"
        "  -s seed         Seed Number (default: -1)\n"
        "  -l log          Show Log (default: false)\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *server_host = argv[1];
    const char *path = argv[2];

    /* Options */
    size_t block_size = DEFAULT_BLOCK_SIZE;
    long iterations = DEFAULT_ITERS;
    long seed = time(NULL);
    int log = 0;

    int opt;
    while ((opt = getopt(argc, argv, "b:n:s:l")) != -1) {
        switch (opt) {
        case 'b': {
            int block_num = strtoul(optarg, NULL, 10);
            if (block_num <= 0) {
                fprintf(stderr, "Block size must be positive number.\n");
                return 1;
            }
            block_size = ALIGN * block_num;
            break;
        }
        case 'n':
            iterations = strtol(optarg, NULL, 10);
            if (iterations <= 0) iterations = DEFAULT_ITERS;
            break;
        case 's': {
            int s = strtol(optarg, NULL, 10);
            if(s >= 0) seed = s;
            break;
        }
        case 'l':
            log = 1;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    // RPC connect
    srand(seed);
    CLIENT *clnt = clnt_create(server_host, BLOCKCOPY_PROG, BLOCKCOPY_VERS, "tcp");
    if (!clnt) {
        clnt_pcreateerror(server_host);
        exit(1);
    }

    // Open file
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open src");
        exit(1);
    }

    // get fstat
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        exit(1);
    }
    off_t filesize = st.st_size;

    // aligned memory allocation in buf
    void *buf;
    if (posix_memalign(&buf, ALIGN, block_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    // check time
    struct timespec start_time, end_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        perror("clock_gettime start");
    }

    // Test Start
    for (long i = 0; i < iterations; i++) {
        if(log) {
            if(i % 1000 == 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC, &now_ts);

                double elapsed = (now_ts.tv_sec - start_time.tv_sec)
                                + (now_ts.tv_nsec - start_time.tv_nsec) / 1e9;

                fprintf(stderr, "\rBlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs", i, iterations, (double) i / iterations * 100, elapsed);
            }
        }

        off_t max_blocks = filesize / block_size;
        off_t src_blk = rand() % max_blocks;
        off_t dst_blk = rand() % max_blocks;

        // Set dst_blk randomly to not overlap with src_blk
        while(src_blk == dst_blk) dst_blk = rand() % max_blocks;

        off_t src_logical = src_blk * block_size;
        off_t dst_logical = dst_blk * block_size;

        pba_seg* src_pba;
        pba_seg* dst_pba;
        size_t src_pba_cnt, dst_pba_cnt;
        if (get_pba(fd, src_logical, block_size, &src_pba, &src_pba_cnt) != 0)
            continue;

        if (get_pba(fd, dst_logical, block_size, &dst_pba, &dst_pba_cnt) != 0)
            continue;
        
        if(src_pba_cnt != 1 || dst_pba_cnt != 1) {
            fprintf(stderr, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
            fprintf(stderr, "Number of extents are not 1. src_pba_cnt: %lu, dst_pba_cnt: %lu\n", src_pba_cnt, dst_pba_cnt);
            for(int j = 0; j < src_pba_cnt; ++j) {
                fprintf(stderr, "src_pba[%d]: %lu, len: %lu\n", j, src_pba[j].pba, src_pba[j].len);
            }
            for(int j = 0; j < src_pba_cnt; ++j) {
                fprintf(stderr, "dst_pba[%d]: %lu, len: %lu\n", j, dst_pba[j].pba, dst_pba[j].len);
            }
            fprintf(stderr, "\n");
        }

        pba_write_params params;
        params.pba_src = src_pba[0].pba;
        params.pba_dst = dst_pba[0].pba;
        params.nbytes = src_pba[0].len;

        int *res = write_pba_1(&params, clnt);
        if (!res || *res != 0) {
            fprintf(stderr, "RPC write failed at PBA %lu to %lu\n", (unsigned long)src_pba[0].pba, dst_pba[0].pba);
            break;
        }
    }

    if(log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);

        double elapsed = (now_ts.tv_sec - start_time.tv_sec)
                        + (now_ts.tv_nsec - start_time.tv_nsec) / 1e9;

        fprintf(stderr, "\rBlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs", iterations, iterations, (double) 100, elapsed);
    }

    free(buf);
    close(fd);
    clnt_destroy(clnt);

    if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
        perror("clock_gettime end");
    }

    // Calculate elapsed time
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                    (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    // Calculate statistics
    long long total_bytes = (long long)iterations * block_size;
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / elapsed;

    printf("\n\n");
    printf("------------ RPC Test Results ------------\n");
    printf("Iterations attempted: %ld\n", iterations);
    printf("Block size: %zu bytes\n", block_size);
    printf("Log on: %s", log ? "true" : "false");
    printf("\nI/O: \n");
    //printf("  Total data copied: %.2f MB\n", total_bytes / (1024.0 * 1024.0));
    printf("  Elapsed time: %.3f seconds\n", elapsed);
    printf("  Approx throughput: %.2f MB/s\n", throughput_mbps);
    //printf("  Operations per second: %.2f ops/s\n", ops_per_sec);
    printf("------------------------------------------\n");

    return 0;
}
