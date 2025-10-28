#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef O_DIRECT
#define O_DIRECT 00040000
#endif

#define DEFAULT_BLOCK_SIZE 4096
#define DEFAULT_ITERS 1000000
#define ALIGN 4096

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <server_eternity> <file_path> [-b block_size] [-n iterations] [-s seed] [-l log]\n"
        "Options:\n"
        "  -b block_number # of block number. Block is 4096B. (default: 1)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n"
        "  -s seed         Seed Number (default: -1)\n"
        "  -l log          Show Log (default: false)\n"
        "  -t test         Print result as csv form",
        prog);
}

int main(int argc, char *argv[]) {
    size_t block_size = DEFAULT_BLOCK_SIZE;
    long iterations = DEFAULT_ITERS;
    long seed = time(NULL);
    int log = 0;
    int csv = 0;

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
            if(s >= 0) seed = s;
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

    if (optind + 1 > argc) {
        usage(argv[0]);
        return 1;
    }

    srand(seed);

    // Start timing
    struct timespec start_time, end_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        perror("clock_gettime start");
        return 1;
    }

    const char *src_path = argv[optind];
    // const char *dst_path = argv[optind + 1];

    int src = open(src_path, O_RDWR  | O_DIRECT);
    if (src < 0) { perror("open src"); return 1; }

    // int dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    // if (dst < 0) { perror("open dst"); close(src); return 1; }

    // get source file size
    struct stat st;
    if (fstat(src, &st) < 0) { perror("fstat"); return 1; }
    off_t filesize = st.st_size;

    if (filesize < block_size) {
        fprintf(stderr, "Source file too small (%ld bytes)\n", (long)filesize);
        return 1;
    }

    void *buf;
    if (posix_memalign(&buf, ALIGN, block_size) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        return 1;
    }

    for (long i = 0; i < iterations; i++) {
        if(log) {
            if(i % 1000 == 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC, &now_ts);

                double elapsed = (now_ts.tv_sec - start_time.tv_sec)
                                + (now_ts.tv_nsec - start_time.tv_nsec) / 1e9;

                fprintf(stderr, "\rBlockCopy Baseline Test: %ld / %ld (%6.1f%% ) | %6.2fs", i, iterations, (double) i / iterations * 100, elapsed);
            }
        }

        // pick random aligned offset
        off_t max_blocks = filesize / block_size;
        off_t src_blk = rand() % max_blocks;
        off_t dst_blk = rand() % max_blocks;
        while(src_blk == dst_blk) dst_blk = rand() % max_blocks;


        off_t src_offset = src_blk * block_size;
        off_t dst_offset = dst_blk * block_size;

        ssize_t r = pread(src, buf, block_size, src_offset);
        if (r < 0) {
            perror("pread");
            free(buf); close(src);
            return 1;
        }

        ssize_t written = 0;
        while (written < r) {
            ssize_t w = pwrite(src, (char*)buf + written, r - written, dst_offset + written);
            if (w < 0) {
                perror("pwrite");
                free(buf); close(src);
                return 1;
            }
            written += w;
        }
    }

    if(log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);

        double elapsed = (now_ts.tv_sec - start_time.tv_sec)
                        + (now_ts.tv_nsec - start_time.tv_nsec) / 1e9;

        fprintf(stderr, "\rBlockCopy Baseline Test: %ld / %ld (%6.1f%% ) | %6.2fs", iterations, iterations, (double) 100, elapsed);
    }

    free(buf);
    close(src);

    // End timing
    if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
        perror("clock_gettime end");
        free(buf); close(src);
        return 1;
    }

    // Calculate elapsed time
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                    (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    // Calculate statistics
    long long total_bytes = (long long)iterations * block_size;
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / elapsed;
    //double ops_per_sec = iterations / elapsed;

    if(csv) {
        printf("%.3f\n", elapsed);
        return 0;
    }
    printf("\n\n");
    printf("---------- Baseline Results ----------\n");
    printf("Iterations attempted: %ld\n", iterations);
    printf("Block size: %zu bytes\n", block_size);
    printf("Seed: %ld\n", seed);
    printf("Log on: %s\n", log ? "true" : "false");
    printf("Result: \n");
    //printf("  Total data copied: %.2f MB\n", total_bytes / (1024.0 * 1024.0));
    printf("  Elapsed time: %.3f seconds\n", elapsed);
    printf("  Approx throughput: %.2f MB/s\n", throughput_mbps);
    //printf("  Operations per second: %.2f ops/s\n", ops_per_sec);
    printf("------------------------------------------\n");
    return 0;
}
