#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
        "Usage: %s <file_path> [-b block_size] [-n iterations] [-s seed] [-l] [-t]\n"
        "Options:\n"
        "  -b block_number # of block number. Block is 4096B. (default: 1)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n"
        "  -s seed         Seed Number (default: -1)\n"
        "  -l log          Show Log (default: false)\n"
        "  -t test         Print result as csv form\n",
        prog);
}

static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

static inline double get_elapsed(uint64_t ns) {
    return (double) ns / 1e3;
}

// static _Atomic uint64_t g_read_ns  = 0;
// static _Atomic uint64_t g_write_ns = 0;

static uint64_t g_read_ns = 0;
static uint64_t g_write_ns = 0;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];

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

/************ Time Check Start ************/

    struct timespec t_total0, t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    /************ Prepare Stage ************/

    struct timespec t_prep0, t_prep1;
    t_prep0 = t_total0;

    srand(seed);

    int fd = open(path, O_RDWR | O_DIRECT);
    if (fd < 0) { perror("open file"); return 1; }

    // get source file size
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
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

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_prep1);

    /************ Prepare Stage End ************/

    for (long i = 0; i < iterations; i++) {
        if(log) {
            if(i % 1000 == 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

                double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                                + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

                fprintf(stderr, "\rBlockCopy Baseline Test: %ld / %ld (%6.1f%% ) | %6.2fs", i, iterations, (double) i / iterations * 100, elapsed);
            }
        }

        off_t max_blocks = filesize / block_size;
        off_t src_blk = rand() % max_blocks;
        off_t dst_blk = rand() % max_blocks;

        // Set dst_blk randomly to not overlap with src_blk
        while(src_blk == dst_blk) dst_blk = rand() % max_blocks;

        /************ Read ************/

        off_t src_offset = src_blk * block_size;
        off_t dst_offset = dst_blk * block_size;

        struct timespec t_read0, t_read1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read0);

        ssize_t r = pread(fd, buf, block_size, src_offset);
        if (r < 0 || (size_t) r < block_size) {
            perror("pread");
            free(buf); close(fd);
            return 1;
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_read1);

        /************ Read End ************/

        /************ Write ************/

        struct timespec t_write0, t_write1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write0);

        ssize_t w = pwrite(fd, (char*)buf, r, dst_offset);
        if(w < 0 || (size_t) w < r) {
            perror("pwrite");
            free(buf); close(fd);
            return 1;
        }

	if (fsync(fd) == -1) {
    	    perror("fsync");
	    free(buf);
	    break;
	}

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_write1);

        /************ Write End ************/

        uint64_t read_ns = ns_diff(t_read0, t_read1);
        uint64_t write_ns = ns_diff(t_write0, t_write1);

        // atomic_fetch_add_explicit(&g_read_ns, read_ns, memory_order_relaxed);
        // atomic_fetch_add_explicit(&g_write_ns, write_ns, memory_order_relaxed);

        g_read_ns += read_ns;
        g_write_ns += write_ns;
    }

    if(log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

        double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                        + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

        fprintf(stderr, "\rBlockCopy Baseline Test: %ld / %ld (%6.1f%% ) | %6.2fs", iterations, iterations, (double) 100, elapsed);
    }

    /************ End Stage ************/

    struct timespec t_end0, t_end1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_end0);

    free(buf);
    close(fd);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);
    t_end1 = t_total1;

    /************ End Stage End ************/

/************ Time Check End ************/

    // Calculate elapsed time
    uint64_t total_ns = ns_diff(t_total0, t_total1);
    uint64_t prep_ns = ns_diff(t_prep0, t_prep1);
    uint64_t end_ns = ns_diff(t_end0, t_end1);
    uint64_t read_ns = g_read_ns;
    uint64_t write_ns = g_write_ns;
    uint64_t io_ns = total_ns - prep_ns - end_ns - read_ns - write_ns;

    if(prep_ns + end_ns + read_ns + write_ns + io_ns != total_ns) {
        fprintf(stderr, "Time calculation failed. Do not match with total_ns\n");
        exit(1);
    }

    // Calculate statistics
    long long total_bytes = (long long)iterations * block_size;
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / get_elapsed(total_ns);

    read_ns /= iterations;
    write_ns /= iterations;
    io_ns /= iterations;
    total_ns /= iterations;



    if(csv) {
        // block_num, iteration, # of block_copies, file_size, read Time, write Time, I/O time, Total time
        printf("%lu,%ld,%ld,%.3f,%.3f,%.3f,%.3f,%.3f\n", 
            block_size/ALIGN, 
            iterations, 
            block_size/ALIGN * iterations, 
            (double)filesize / (1024.0 * 1024.0 * 1024.0),
            get_elapsed(read_ns),
            get_elapsed(write_ns),
            //get_elapsed(prep_ns),
            //get_elapsed(end_ns),
            get_elapsed(io_ns),
            get_elapsed(total_ns)
        );
        return 0;
    }
    printf("\n\n");
    printf("------------ RPC Test Results ------------\n");
    printf("Iterations attempted: %ld\n", iterations);
    printf("Block size: %zu bytes\n", block_size);
    printf("Seed: %ld\n", seed);
    printf("Log on: %s\n", log ? "true" : "false");
    printf("\n");
    printf("Client Main Result: \n");
    printf("  Read Elapsed time: %.3f seconds\n", get_elapsed(read_ns));
    printf("  Write Elapsed time: %.3f seconds\n", get_elapsed(write_ns));
    printf("  I/O Elapsed time: %.3f seconds\n", get_elapsed(io_ns));
    printf("\n");
    printf("Client Other Result: \n");
    printf("  Prepare Elapsed time: %.3f seconds\n", get_elapsed(prep_ns));
    printf("  End Elapsed time: %.3f seconds\n", get_elapsed(end_ns));
    printf("\n");
    printf("Summary: \n");
    printf("  Client Main time: %.3f seconds\n", get_elapsed(read_ns + write_ns + io_ns));
    printf("  Client Other time: %.3f seconds\n", get_elapsed(prep_ns + end_ns));
    printf("\n");
    printf("  Total Elapsed time: %.3f seconds\n", get_elapsed(total_ns));
    printf("  Approx throughput: %.2f MB/s\n", throughput_mbps);
    //printf("  Operations per second: %.2f ops/s\n", ops_per_sec);
    printf("------------------------------------------\n");

    return 0;
}
