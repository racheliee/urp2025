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

static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

static inline double get_elapsed(uint64_t ns) {
    return (double) ns / 1e9;
}

/* helper to get physical block address from logical offset */
static int get_pba(int fd, off_t logical, size_t length, pba_seg **out, size_t* out_cnt, uint64_t* fiemap_ns) {
    struct timespec t_before, t_after;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_before);

    size_t size = sizeof(struct fiemap) + EXTENTS_MAX * sizeof(struct fiemap_extent);
    struct fiemap *fiemap = (struct fiemap*)calloc(1, size);
    if(!fiemap) return -1;

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
        fprintf(stderr, "More mapped extentes needed: mapped %ld, but need %u\n", (long)EXTENTS_MAX, fiemap->fm_mapped_extents);
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
    if(!vec) {
        result = -1;
        goto exit;
    }

    for(size_t i = 0; i < fiemap->fm_mapped_extents; ++i) {
        struct fiemap_extent *e = &fiemap->fm_extents[i];

        vec[n].pba = e->fe_physical + (logical - e->fe_logical);
        vec[n].len = length;
        n++;
    }

    if (n == 0) { free(vec); result = -1; goto exit; }
    *out = vec, *out_cnt = n;

exit:
    free(fiemap);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_after);
    *fiemap_ns = (uint64_t)ns_diff(t_before, t_after);
    return result;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <server_eternity> <file_path> [-b block_size] [-n iterations] [-s seed] [-l] [-t]\n"
        "Options:\n"
        "  -b block_number # of block number. Block is 4096B. (default: 1)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n"
        "  -s seed         Seed Number (default: -1)\n"
        "  -l log          Show Log (default: false)\n"
        "  -t test         Print result as csv form",
        prog);
}

// static _Atomic uint64_t g_fiemap_ns  = 0;
// static _Atomic uint64_t g_rpc_total_ns = 0;

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

    // RPC connect
    CLIENT *clnt = clnt_create(server_host, BLOCKCOPY_PROG, BLOCKCOPY_VERS, "tcp");
    if (!clnt) {
        clnt_pcreateerror(server_host);
        exit(1);
    }

    // Open file
    int fd = open(path, O_RDONLY | O_DIRECT);
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

    // aligned memory allocation in buf
    void *buf;
    if (posix_memalign(&buf, ALIGN, block_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_prep1);

    /************ Prepare Stage End ************/

    // Test Start
    for (long i = 0; i < iterations; i++) {
        if(log) {
            if(i % 1000 == 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

                double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                                + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

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

        /************ Fiemap0 ************/
        uint64_t fiemap_ns0, fiemap_ns1, rpc_ns;
        if (get_pba(fd, src_logical, block_size, &src_pba, &src_pba_cnt, &fiemap_ns0) != 0)
            continue;
        /************ Fiemap0 End ************/

        /************ Fiemap1 ************/
        if (get_pba(fd, dst_logical, block_size, &dst_pba, &dst_pba_cnt, &fiemap_ns1) != 0)
            continue;
        /************ Fiemap1 End ************/
        
        if(src_pba_cnt != dst_pba_cnt) {
            fprintf(stderr, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
            fprintf(stderr, "Number of extents are not same. src_pba_cnt: %lu, dst_pba_cnt: %lu\n", src_pba_cnt, dst_pba_cnt);
            for(int j = 0; j < src_pba_cnt; ++j) {
                fprintf(stderr, "src_pba[%d]: %lu, len: %lu\n", j, src_pba[j].pba, src_pba[j].len);
            }
            for(int j = 0; j < dst_pba_cnt; ++j) {
                fprintf(stderr, "dst_pba[%d]: %lu, len: %lu\n", j, dst_pba[j].pba, dst_pba[j].len);
            }
            fprintf(stderr, "\n");
        }

        pba_write_params params;
        params.pba_src = src_pba[0].pba;
        params.pba_dst = dst_pba[0].pba;
        params.nbytes = src_pba[0].len;

        struct timespec t_rpc0, t_rpc1;

        /************ RPC ************/

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc0);
        int *res = write_pba_1(&params, clnt);
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc1);

        /************ RPC End ************/

        if (res == NULL || *res == -1) {
            fprintf(stderr, "RPC write failed at PBA %lu to %lu\n", (unsigned long)src_pba[0].pba, dst_pba[0].pba);
            break;
        }
        

        uint64_t rpc_total_ns = ns_diff(t_rpc0, t_rpc1);
        // atomic_fetch_add_explicit(&g_fiemap_ns, fiemap_ns0, memory_order_relaxed);
        // atomic_fetch_add_explicit(&g_fiemap_ns, fiemap_ns1, memory_order_relaxed);
        // atomic_fetch_add_explicit(&g_rpc_total_ns, rpc_total_ns, memory_order_relaxed);

        g_fiemap_ns += fiemap_ns0 + fiemap_ns1;
        g_rpc_total_ns += rpc_total_ns;
    }

    if(log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

        double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                        + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

        fprintf(stderr, "\rBlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs", iterations, iterations, (double) 100, elapsed);
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
    
    // Get server time
    get_server_ios *res = get_time_1(NULL, clnt);
    if (res == NULL) {
        fprintf(stderr, "RPC get server time failed\n");
        clnt_destroy(clnt);
        exit(1);
    }
    clnt_destroy(clnt);
    uint64_t server_read_ns = res->server_read_time;
    uint64_t server_write_ns = res->server_write_time;
    uint64_t server_other_ns = res->server_other_time;

    // Calculate elapsed time
    uint64_t total_ns = ns_diff(t_total0, t_total1);
    uint64_t prep_ns = ns_diff(t_prep0, t_prep1);
    uint64_t end_ns = ns_diff(t_end0, t_end1);
    uint64_t fiemap_ns = g_fiemap_ns;
    uint64_t rpc_ns = g_rpc_total_ns - server_read_ns - server_write_ns - server_other_ns;
    uint64_t io_ns = total_ns - prep_ns - end_ns - fiemap_ns - g_rpc_total_ns;

    if(prep_ns + end_ns + fiemap_ns + rpc_ns + server_read_ns + server_write_ns + server_other_ns + io_ns != total_ns) {
        fprintf(stderr, "Time calculation failed. Do not match with total_ns\n");
        exit(1);
    }

    // Calculate statistics
    long long total_bytes = (long long)iterations * block_size;
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / get_elapsed(total_ns);

    if(csv) {
        // block_num, iteration, # of block_copies, file_size, Server Read Time, Server Write Time, Server Other Time, Prep Time, End Time, Fiemap time, RPC time, I/O time, Total time
        printf("%lu,%ld,%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", 
            block_size/ALIGN, 
            iterations, 
            block_size/ALIGN * iterations, 
            (double)filesize / (1024.0 * 1024.0 * 1024.0),
            get_elapsed(server_read_ns),
            get_elapsed(server_write_ns),
            get_elapsed(server_other_ns),
            get_elapsed(prep_ns),
            get_elapsed(end_ns),
            get_elapsed(fiemap_ns),
            get_elapsed(rpc_ns),
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
    printf("  Server Elapsed time: %.3f seconds\n", get_elapsed(server_read_ns + server_write_ns + server_other_ns));
    printf("  Client Main time: %.3f seconds\n", get_elapsed(fiemap_ns + rpc_ns + io_ns));
    printf("  Client Other time: %.3f seconds\n", get_elapsed(prep_ns + end_ns));
    printf("\n");
    printf("  Total Elapsed time: %.3f seconds\n", get_elapsed(total_ns));
    printf("  Approx throughput: %.2f MB/s\n", throughput_mbps);
    //printf("  Operations per second: %.2f ops/s\n", ops_per_sec);
    printf("------------------------------------------\n");

    return 0;
}
