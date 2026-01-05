//#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define DEFAULT_BLOCK_SIZE 4096
#define DEFAULT_ITERS 1000000
#define ALIGN 4096

typedef struct timespec timespec_t;

static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

static inline double get_elapsed(uint64_t ns) {
    return (double)ns / 1e3;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <file_path> [options]\n"
        "Options:\n"
        "  -b block_number    # of blocks (1 block = 4096B, default: 1)\n"
        "  -n iterations      Number of random copies (default: 1000000)\n"
        "  -s seed            Random seed (default: current time)\n"
        "  -l                 Show progress log\n"
        "  -t                 Output CSV format\n",
        prog);
}

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
    int block_num = 1;

    int opt;
    while ((opt = getopt(argc, argv, "b:n:s:lt")) != -1) {
        switch (opt) {
        case 'b': {
            block_num = strtoul(optarg, NULL, 10);
            if (block_num <= 0) {
                fprintf(stderr, "Block size must be positive.\n");
                return 1;
            }
            block_size = ALIGN * block_num;
            break;
        }
        case 'n':
            iterations = strtol(optarg, NULL, 10);
            break;
        case 's': {
            long s = strtol(optarg, NULL, 10);
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

    srand(seed);

    timespec_t t_total0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);


    int fd = open(path, O_RDWR | O_DIRECT | O_SYNC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }

    off_t filesize = st.st_size;
    //off_t max_blocks = filesize / block_size;
    off_t max_blocks = filesize / ALIGN;
    if (max_blocks == 0) {
        fprintf(stderr, "File is too small for chosen block size.\n");
        return 1;
    }

    
    void *buf;
    if (posix_memalign(&buf, ALIGN, block_size) != 0) {
        perror("posix_memalign");
        return 1;
    }


    uint64_t total_read_ns = 0;
    uint64_t total_write_ns = 0;
    uint64_t total_iter_ns = 0;

    uint64_t iter_read_ns = 0;
    uint64_t iter_write_ns = 0;
    // ******** Preparation End ********
    
    //************ Iteration Start *************
    for (long i = 0; i < iterations; i++) {
	timespec_t t_iter0, t_iter1;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t_iter0);

        if (log && (i % 1000 == 0)) {
            timespec_t now;
            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            double elapsed = (now.tv_sec - t_total0.tv_sec)
                           + (now.tv_nsec - t_total0.tv_nsec) / 1e9;
            fprintf(stderr, "\rBaseline Random: %ld/%ld (%4.1f%%) | %.2fs",
                    i, iterations, (double)i/iterations * 100.0, elapsed);
        }

	for (int j = 0 ; j < block_num ; j++) {

            off_t src_blk = rand() % max_blocks;
            off_t dst_blk = rand() % max_blocks;
	    while (dst_blk == src_blk) {dst_blk = rand() % max_blocks;}

            off_t src_off = src_blk * ALIGN;
            off_t dst_off = dst_blk * ALIGN;

            /* READ */
            timespec_t t_r0, t_r1;
            clock_gettime(CLOCK_MONOTONIC_RAW, &t_r0);
            ssize_t r = pread(fd, (char*)buf + (size_t)j * (ALIGN), ALIGN, src_off);
            clock_gettime(CLOCK_MONOTONIC_RAW, &t_r1);

            if (r != (ssize_t)ALIGN) {
                fprintf(stderr, "pread failed at %ld: %s\n", (long)src_off, strerror(errno));
                break;
            }
            iter_read_ns = ns_diff(t_r0, t_r1);

            /* WRITE */
            timespec_t t_w0, t_w1;
            clock_gettime(CLOCK_MONOTONIC_RAW, &t_w0);
            ssize_t w = pwrite(fd, (char*)buf + (size_t)j * (ALIGN), ALIGN, dst_off);
            clock_gettime(CLOCK_MONOTONIC_RAW, &t_w1);

            if (w != (ssize_t)ALIGN) {
                fprintf(stderr, "pwrite failed at %ld: %s\n", (long)dst_off, strerror(errno));
                break;
            }
            iter_write_ns = ns_diff(t_w0, t_w1);
	    total_read_ns += iter_read_ns;
            total_write_ns += iter_write_ns;

        }

	clock_gettime(CLOCK_MONOTONIC_RAW, &t_iter1);

        	total_iter_ns += ns_diff(t_iter0, t_iter1);
    }

    //************** Iteration End **************
    free(buf);
    close(fd);


    uint64_t total_ns = total_iter_ns;
    uint64_t read_ns  = total_read_ns;
    uint64_t write_ns = total_write_ns;

    uint64_t io_ns = total_ns - read_ns - write_ns;
    if (io_ns + read_ns + write_ns != total_ns) {
        uint64_t accounted =  read_ns + write_ns;
        io_ns = (total_ns > accounted) ? (total_ns - accounted) : 0;
    }

    total_ns /= iterations;
    read_ns /= iterations;
    write_ns /= iterations;

    long long bytes_total = (long long)iterations * block_size;
    double throughput = (bytes_total / (1024.0 * 1024.0)) / get_elapsed(total_ns);

    if (csv) {
        printf("%lu,%ld,%ld,%.3f,%.3f,%.3f,%.3f,%.3f\n",
               block_size / ALIGN, 
	       iterations,
               block_size / ALIGN * iterations,
               (double)filesize / (1024.0 * 1024.0 * 1024.0),
               get_elapsed(read_ns),
               get_elapsed(write_ns),
               get_elapsed(io_ns),
               get_elapsed(total_ns));
        return 0;
    }

    printf("\n\n------------ Baseline Random Results ------------\n");
    printf("Iterations: %ld\n", iterations);
    printf("Block size: %zu bytes\n", block_size);
    printf("Seed: %ld\n", seed);
    printf("\n");
    printf("Read time:  %.3f s\n", get_elapsed(read_ns));
    printf("Write time: %.3f s\n", get_elapsed(write_ns));
    printf("IO other:   %.3f s\n", get_elapsed(io_ns));
    printf("\nTotal time: %.3f s\n", get_elapsed(total_ns));
    printf("Throughput: %.2f MB/s\n", throughput);
    printf("--------------------------------------------------\n");
    return 0;
}
