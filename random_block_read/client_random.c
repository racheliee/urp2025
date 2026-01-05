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

#include "blockcopy_random.h"
#include "client_random.h"

typedef struct {
    uint64_t pba;
    size_t len;
} pba_seg;

static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

static inline double get_elapsed(uint64_t ns) {
    return (double)ns / 1e9;
}

/* helper to get physical block address from logical offset */
static int get_pba(int fd, off_t logical, size_t length,
                   pba_seg **out, size_t *out_cnt, uint64_t *fiemap_ns) {
    struct timespec t_before, t_after;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_before);

    size_t size = sizeof(struct fiemap) + EXTENTS_MAX * sizeof(struct fiemap_extent);
    struct fiemap *fiemap = (struct fiemap *)calloc(1, size);
    if (!fiemap) return -1;

    fiemap->fm_start = logical;
    fiemap->fm_length = length;
    fiemap->fm_extent_count = EXTENTS_MAX;

    int result = 0;

    if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
        perror("ioctl fiemap");
        result = -1;
        goto exit;
    }
    if (fiemap->fm_mapped_extents > EXTENTS_MAX) {
        fprintf(stderr,
                "More mapped extents needed: mapped %ld, but need %u\n",
                (long)fiemap->fm_mapped_extents, EXTENTS_MAX);
        result = -1;
        goto exit;
    }
    if (fiemap->fm_mapped_extents == 0) {
        fprintf(stderr, "no extents mapped at logical %ld\n", (long)logical);
        result = -1;
        goto exit;
    }

    pba_seg *vec = calloc(fiemap->fm_mapped_extents, sizeof(pba_seg));
    size_t n = 0;
    if (!vec) {
        result = -1;
        goto exit;
    }

    for (size_t i = 0; i < fiemap->fm_mapped_extents; ++i) {
        struct fiemap_extent *e = &fiemap->fm_extents[i];

        vec[n].pba = e->fe_physical + (logical - e->fe_logical);
        vec[n].len = length;
        n++;
    }

    if (n == 0) {
        free(vec);
        result = -1;
        goto exit;
    }

    *out = vec;
    *out_cnt = n;

exit:
    free(fiemap);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_after);
    *fiemap_ns = ns_diff(t_before, t_after);
    return result;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <server_hostname> <file_path> [options]\n"
        "Options:\n"
        "  -b block_number    # of blocks (1 block = 4096B, default: 1)\n"
        "  -n iterations      Number of random copies (default: 1000000)\n"
        "  -s seed            Random seed (default: current time)\n"
        "  -l                 Show progress log\n"
        "  -t                 Output results in CSV format\n",
        prog);
}

static uint64_t g_fiemap_ns = 0;
static uint64_t g_rpc_total_ns = 0;

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
    int csv = 0;
    int batch_size = 100;  // Default batch size

    int opt;
    while ((opt = getopt(argc, argv, "b:n:s:lt")) != -1) {
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
            if (s >= 0) seed = s;
            break;
        }
        case 'l':
            log = 1;
            break;
        case 't':
            csv = 1;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    struct timespec t_total0, t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    struct timespec t_prep0, t_prep1;
    t_prep0 = t_total0;

    srand(seed);

    // RPC connect
    CLIENT *clnt = clnt_create(server_host, BLOCKCOPY_PROG, BLOCKCOPY_VERS, "tcp");
    if (!clnt) {
        clnt_pcreateerror(server_host);
        exit(1);
    }

    {
        void *res = reset_time_1(NULL, clnt);
        if (res == NULL) {
            fprintf(stderr, "RPC reset server time failed\n");
            clnt_destroy(clnt);
            exit(1);
        }
    }

    // Open file
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open file");
        exit(1);
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        exit(1);
    }
    off_t filesize = st.st_size;

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_prep1);

    // Allocate batch parameters
    pba_batch_params batch_params;
    
    // Test Start
    long i = 0;
    while (i < iterations) {
        if (log && (i % 1000 == 0)) {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

            double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                           + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

            fprintf(stderr,
                    "\rBlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs",
                    i, iterations, (double)i / iterations * 100.0, elapsed);
        }

        off_t max_blocks = filesize / ALIGN;
        if (max_blocks == 0) {
            fprintf(stderr, "File too small for chosen block size.\n");
            break;
        }

        // Collect batch_size operations
        int batch_count = 0;
        for (int b = 0; b < batch_size && i < iterations; b++, i++) {
            // RANDOM source / dest blocks
            off_t src_blk = rand() % max_blocks;
            off_t dst_blk = rand() % max_blocks;
            while (src_blk == dst_blk) dst_blk = rand() % max_blocks;

            off_t src_logical = src_blk * block_size;
            off_t dst_logical = dst_blk * block_size;

            pba_seg *src_pba = NULL;
            pba_seg *dst_pba = NULL;
            size_t src_pba_cnt = 0, dst_pba_cnt = 0;

            uint64_t fiemap_ns0 = 0, fiemap_ns1 = 0;

            if (get_pba(fd, src_logical, block_size,
                        &src_pba, &src_pba_cnt, &fiemap_ns0) != 0)
                continue;

            if (get_pba(fd, dst_logical, block_size,
                        &dst_pba, &dst_pba_cnt, &fiemap_ns1) != 0) {
                free(src_pba);
                continue;
            }

            if (src_pba_cnt != dst_pba_cnt) {
                fprintf(stderr, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
                fprintf(stderr, "Number of extents are not same. src_pba_cnt: %lu, dst_pba_cnt: %lu\n",
                        (unsigned long)src_pba_cnt, (unsigned long)dst_pba_cnt);
                free(src_pba);
                free(dst_pba);
                continue;
            }

            // Add to batch
            batch_params.pba_srcs[batch_count] = src_pba[0].pba;
            batch_params.pba_dsts[batch_count] = dst_pba[0].pba;
            batch_count++;

            free(src_pba);
            free(dst_pba);

            g_fiemap_ns += fiemap_ns0 + fiemap_ns1;
        }

        // Send batched RPC call
        if (batch_count > 0) {
            batch_params.count = batch_count;
            batch_params.block_size = block_size;

            struct timespec t_rpc0, t_rpc1;
            clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc0);
            int *rpc_res = write_pba_batch_1(&batch_params, clnt);
            clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc1);

            uint64_t rpc_total_ns = ns_diff(t_rpc0, t_rpc1);

            if (rpc_res == NULL || *rpc_res == -1) {
                fprintf(stderr, "RPC batch write failed\n");
                break;
            }

            g_rpc_total_ns += rpc_total_ns;
        }
    }

    if (log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

        double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                       + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

        fprintf(stderr,
                "\rBlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs\n",
                iterations, iterations, 100.0, elapsed);
    }

    struct timespec t_end0, t_end1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_end0);

    close(fd);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);
    t_end1 = t_total1;

    // Get server time
    get_server_ios *time_res = get_time_1(NULL, clnt);
    if (time_res == NULL) {
        fprintf(stderr, "RPC get server time failed\n");
        clnt_destroy(clnt);
        exit(1);
    }
    clnt_destroy(clnt);

    uint64_t server_read_ns  = time_res->server_read_time;
    uint64_t server_write_ns = time_res->server_write_time;
    uint64_t server_other_ns = time_res->server_other_time;

    uint64_t total_ns = ns_diff(t_total0, t_total1);
    uint64_t prep_ns  = ns_diff(t_prep0, t_prep1);
    uint64_t end_ns   = ns_diff(t_end0, t_end1);
    uint64_t fiemap_ns = g_fiemap_ns;

    uint64_t rpc_ns = g_rpc_total_ns
                      - server_read_ns
                      - server_write_ns
                      - server_other_ns;

    uint64_t io_ns = total_ns
                     - prep_ns
                     - end_ns
                     - fiemap_ns
                     - g_rpc_total_ns;

    if (prep_ns + end_ns + fiemap_ns + rpc_ns
        + server_read_ns + server_write_ns + server_other_ns + io_ns != total_ns) {
        fprintf(stderr, "Time calculation failed. Do not match with total_ns\n");
        exit(1);
    }

    long long total_bytes = (long long)iterations * block_size;
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0))
                             / get_elapsed(total_ns);

    if (csv) {
        printf("%lu,%ld,%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n",
               block_size / ALIGN,
               iterations,
               block_size / ALIGN * iterations,
               (double)filesize / (1024.0 * 1024.0 * 1024.0),
               get_elapsed(server_read_ns),
               get_elapsed(server_write_ns),
               get_elapsed(server_other_ns),
               get_elapsed(prep_ns),
               get_elapsed(end_ns),
               get_elapsed(fiemap_ns),
               get_elapsed(rpc_ns),
               get_elapsed(io_ns),
               get_elapsed(total_ns),
               batch_size);
        return 0;
    }

    printf("\n\n");
    printf("------------ RPC Test Results ------------\n");
    printf("Iterations attempted: %ld\n", iterations);
    printf("Block size: %zu bytes\n", block_size);
    printf("Batch size: %d\n", batch_size);
    printf("Seed: %ld\n", seed);
    printf("Log on: %s\n", log ? "true" : "false");
    printf("\n");
    printf("Server Result: \n");
    printf("  Read Elapsed time: %.3f seconds\n", get_elapsed(server_read_ns));
    printf("  Write Elapsed time: %.3f seconds\n", get_elapsed(server_write_ns));
    printf("  Other Elapsed time: %.3f seconds\n", get_elapsed(server_other_ns));
    printf("\n");
    printf("Client Main Result: \n");
    printf("  Fiemap Elapsed time: %.3f seconds\n", get_elapsed(fiemap_ns));
    printf("  RPC Elapsed time: %.3f seconds\n", get_elapsed(rpc_ns));
    printf("  I/O Elapsed time: %.3f seconds\n", get_elapsed(io_ns));
    printf("\n");
    printf("Client Other Result: \n");
    printf("  Prepare Elapsed time: %.3f seconds\n", get_elapsed(prep_ns));
    printf("  End Elapsed time: %.3f seconds\n", get_elapsed(end_ns));
    printf("\n");
    printf("Summary: \n");
    printf("  Server Elapsed time: %.3f seconds\n",
           get_elapsed(server_read_ns + server_write_ns + server_other_ns));
    printf("  Client Main time: %.3f seconds\n",
           get_elapsed(fiemap_ns + rpc_ns + io_ns));
    printf("  Client Other time: %.3f seconds\n",
           get_elapsed(prep_ns + end_ns));
    printf("\n");
    printf("  Total Elapsed time: %.3f seconds\n", get_elapsed(total_ns));
    printf("  Approx throughput: %.2f MB/s\n", throughput_mbps);
    printf("------------------------------------------\n");

    return 0;
}
