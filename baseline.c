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
        "Usage: %s [-b block_size] [-n iterations] <src_file> <dst_file>\n"
        "Options:\n"
        "  -b block_size   Segment size in bytes (default: 4096)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n",
        prog);
}

int main(int argc, char *argv[]) {
    size_t block_size = DEFAULT_BLOCK_SIZE;
    long iterations = DEFAULT_ITERS;

    int opt;
    while ((opt = getopt(argc, argv, "b:n:")) != -1) {
        switch (opt) {
        case 'b':
            block_size = strtoul(optarg, NULL, 10);
            if (block_size % ALIGN != 0) {
                fprintf(stderr, "Block size must be multiple of %d\n", ALIGN);
                return 1;
            }
            break;
        case 'n':
            iterations = strtol(optarg, NULL, 10);
            if (iterations <= 0) iterations = DEFAULT_ITERS;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind + 2 > argc) {
        usage(argv[0]);
        return 1;
    }

    const char *src_path = argv[optind];
    const char *dst_path = argv[optind + 1];

    int src = open(src_path, O_RDONLY | O_DIRECT);
    if (src < 0) { perror("open src"); return 1; }

    int dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (dst < 0) { perror("open dst"); close(src); return 1; }

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

    srand((unsigned)time(NULL));

    // Start timing
    struct timespec start_time, end_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        perror("clock_gettime start");
        free(buf); close(src); close(dst);
        return 1;
    }

    printf("Starting %ld random copy operations (block size: %zu bytes)...\n", 
           iterations, block_size);

    for (long i = 0; i < iterations; i++) {
        // pick random aligned offset
        off_t max_blocks = filesize / block_size;
        off_t block = rand() % max_blocks;
        off_t offset = block * block_size;

        ssize_t r = pread(src, buf, block_size, offset);
        if (r < 0) {
            perror("pread");
            free(buf); close(src); close(dst);
            return 1;
        }

        ssize_t written = 0;
        while (written < r) {
            ssize_t w = write(dst, (char*)buf + written, r - written);
            if (w < 0) {
                perror("write");
                free(buf); close(src); close(dst);
                return 1;
            }
            written += w;
        }
    }

    // End timing
    if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
        perror("clock_gettime end");
        free(buf); close(src); close(dst);
        return 1;
    }

    // Calculate elapsed time
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                    (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    
    // Calculate statistics
    long long total_bytes = (long long)iterations * block_size;
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / elapsed;
    double ops_per_sec = iterations / elapsed;

    printf("\nResults:\n");
    printf("  Total operations: %ld\n", iterations);
    printf("  Block size: %zu bytes\n", block_size);
    printf("  Total data copied: %.2f MB\n", total_bytes / (1024.0 * 1024.0));
    printf("  Elapsed time: %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f MB/s\n", throughput_mbps);
    printf("  Operations per second: %.2f ops/s\n", ops_per_sec);

    free(buf);
    close(src);
    close(dst);
    return 0;
}
