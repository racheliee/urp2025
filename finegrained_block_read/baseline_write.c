#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#include "baseline.h"


static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

static inline double get_elapsed(uint64_t ns) {
    return (double) ns / 1e3;
}

static inline uint64_t align_down_u64(uint64_t x, uint64_t a) {
    return (x / a) * a;
}
static inline uint64_t align_up_u64(uint64_t x, uint64_t a) {
    return ((x + a - 1) / a) * a;
}


static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <server_eternity> <file_path> [-b block_size] [-n iterations] [-s seed] [-l] [-t]\n"
        "Options:\n"
        "  -b bytes        Size of content (default: 8)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n"
        "  -s seed         Seed Number (default: -1)\n"
        "  -l log          Show Log (default: false)\n"
        "  -t test         Print result as csv form\n",
        prog);
}

static uint64_t g_read_ns = 0;
static uint64_t g_write_ns = 0;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];

    /* Options */
    size_t bytes_size = DEFAULT_BYTES_SIZE;
    long iterations = DEFAULT_ITERS;
    long seed = time(NULL);
    int log = 0;
    int csv = 0;

    int opt;
    while ((opt = getopt(argc, argv, "b:n:s:lt")) != -1) {
        switch (opt) {
        case 'b': {
            bytes_size = strtoul(optarg, NULL, 10);
            if (bytes_size <= 0) {
                fprintf(stderr, "Byte size must be positive number.\n");
                return 1;
            }
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

    
    /************ Prepare Stage ************/
    srand(seed);

    // Open file
    int fd = open(path, O_RDWR | O_DIRECT);
    if (fd < 0) {
        perror("open file");
        exit(1);
    }

    // get fstat
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        exit(1);
    }
    off_t filesize = st.st_size;
    
    /************ Prepare Stage End ************/

    //테스트 시작 전 시간 측정 (log용)
    struct timespec t_total0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    // Make write buffer
    char* write_buf = malloc(bytes_size);
    if(!write_buf) {
        perror("malloc write_buf failed");
        exit(1);
    }
    for(size_t i = 0; i < bytes_size; ++i) {
        write_buf[i] = 'A' + (char)(rand() % 26);
    }

    printf("Write buffer: %.*s\n", (int)bytes_size, write_buf);

    // Test Start
    for (long i = 0; i < iterations; i++) {
        if(log) {
            if(i % 1000 == 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

                double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                                + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

                fprintf(stderr, "\rFinegrained Write Baseline Test: %ld / %ld (%6.1f%% ) | %6.2fs", i, iterations, (double) i / iterations * 100, elapsed);
            }
        }
        
        off_t max_byte = filesize - bytes_size; //어딜 고르든 bytes size만큼 고를 수 있게
        off_t src_logical = (off_t)((double)rand() / RAND_MAX * max_byte); //random source

        // Get Block Segments
        off_t block_logical = src_logical - (src_logical % BLOCK_SIZE);
        off_t block_length = ((src_logical % BLOCK_SIZE) + bytes_size + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;

        off_t offset_in_block = src_logical - block_logical;
        
        void *buf;
        if (posix_memalign(&buf, ALIGN, block_length) != 0) {
            fprintf(stderr, "posix_memalign failed\n");
            break;
        }

        /************ Read ************/

        static struct timespec t_read0, t_read1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read0);

        ssize_t r = pread(fd, buf, block_length, block_logical);
        if (r == -1) { 
            perror("pread");
            free(buf);
            break;
        }
        if ((size_t)r != (size_t)block_length) {
            fprintf(stderr, "read only segments of block_length: %zu expected, but only %zu\n", (size_t)block_length, (size_t)r);
            free(buf);
            break;
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read1);

        /************ Read End ************/
        
        memcpy(buf+offset_in_block, write_buf, bytes_size);

        /************ Write ************/
        struct timespec t_write0, t_write1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write0);

        ssize_t w = pwrite(fd, buf, block_length, block_logical);
        if(w == -1) {
            perror("pwrite");
            free(buf);
            break;
        }
        if (w < block_length) {
            fprintf(stderr, "written only segments of block_length: %zu expected, but only %zu\n", (size_t)block_length, (size_t)w);
            free(buf);
            break;
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write1);
        
        /************ Write End ************/

        free(buf);
        
	/************ Time Check End ************/

        g_read_ns += ns_diff(t_read0, t_read1); //iteration 중 read에 소요한 시간 총합
        g_write_ns += ns_diff(t_write0, t_write1); //iteration 중 write에 소요한 시간 총합
    }

    if(log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

        double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                        + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

        fprintf(stderr, "\rFinegrained Write Baseline Test: %ld / %ld (%6.1f%% ) | %6.2fs", iterations, iterations, (double) 100, elapsed);
    }

    struct timespec t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);


    /************ End Stage ************/
    close(fd);
    /************ End Stage End ************/


    // Calculate elapsed time
    uint64_t total_ns = ns_diff(t_total0, t_total1); //total_ns를 iteration 자체에만 걸린 시간으로 바꿈!! end, prep 제외
    uint64_t read_ns = g_read_ns;
    uint64_t write_ns = g_write_ns;
    uint64_t io_ns = total_ns - read_ns - write_ns;
	
    // 계산 오류 핸들러.
    if(read_ns + write_ns + io_ns != total_ns) {
        fprintf(stderr, "Time calculation failed. Do not match with total_ns\n");
        exit(1);
    }

    // Calculate statistics
    double throughput_mbps = (bytes_size / (1024.0 * 1024.0)) / get_elapsed(total_ns);

    read_ns /= iterations;
    io_ns /= iterations;
    total_ns /= iterations;

    if(csv) {
        // byte_num, iteration, # of bytes write, file_size, Read time, Write time, I/O time, Total time
        printf("%lu,%ld,%ld,%.3f,%.3f,%.3f,%.3f,%.3f\n", 
            bytes_size, 
            iterations, 
            bytes_size * iterations, 
            (double)filesize / (1024.0 * 1024.0 * 1024.0),
            get_elapsed(read_ns),
            get_elapsed(write_ns),
            get_elapsed(io_ns),
            get_elapsed(total_ns)
        );
        return 0;
    }
    printf("\n\n");
    printf("------------ Finegrained Write Baseline Test Results ------------\n");
    printf("Iterations attempted: %ld\n", iterations);
    printf("Byte size: %zu bytes\n", bytes_size);
    printf("Seed: %ld\n", seed);
    printf("Log on: %s\n", log ? "true" : "false");
    printf("\n");
    printf("Client Main Result: \n");
    printf("  Read Elapsed time: %.3f seconds\n", get_elapsed(read_ns));
    printf("  Write Elapsed time: %.3f seconds\n", get_elapsed(write_ns));
    printf("  I/O Elapsed time: %.3f seconds\n", get_elapsed(io_ns));
    printf("\n");
    printf("  Total Elapsed time: %.3f seconds\n", get_elapsed(total_ns));
    printf("  Approx throughput: %.2f MB/s\n", throughput_mbps);
    //printf("  Operations per second: %.2f ops/s\n", ops_per_sec);
    printf("------------------------------------------\n");

    return 0;
}
