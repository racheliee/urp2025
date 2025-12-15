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

static inline uint64_t align_down_u64(uint64_t x, uint64_t a) {
    return (x / a) * a;
}
static inline uint64_t align_up_u64(uint64_t x, uint64_t a) {
    return ((x + a - 1) / a) * a;
}

/* helper to get physical block address from logical offset */
static int get_pba(int fd, off_t logical, size_t length, finegrained_pba **out, size_t* out_cnt, uint64_t* fiemap_ns) {
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

    finegrained_pba *vec = calloc(fiemap->fm_mapped_extents, sizeof(finegrained_pba));
    if(!vec) {
        result = -1;
        goto exit;
    }

    uint64_t req_start = (uint64_t)logical;
    uint64_t req_end   = req_start + (uint64_t)length;
    size_t n = 0;

    for(size_t i = 0; i < fiemap->fm_mapped_extents; ++i) {
        struct fiemap_extent *e = &fiemap->fm_extents[i];
        
        uint64_t ext_start = e->fe_logical;
        uint64_t ext_end   = e->fe_logical + e->fe_length;

        // overlap in logical space
        uint64_t ov_start = (req_start > ext_start) ? req_start : ext_start;
        uint64_t ov_end   = (req_end   < ext_end)   ? req_end   : ext_end;
        if (ov_start >= ov_end) continue;

        uint64_t piece_len = ov_end - ov_start;

        uint64_t phys = e->fe_physical + (ov_start - ext_start);

        uint64_t need_start = phys;
        uint64_t need_end   = phys + piece_len;

        uint64_t rd_start = align_down_u64(need_start, ALIGN);
        uint64_t rd_end   = align_up_u64(need_end, ALIGN);
        uint64_t rd_len   = rd_end - rd_start;
        uint64_t off_in_rd = need_start - rd_start;

        vec[n].pba = rd_start;
        vec[n].extent_bytes = (int)rd_len;
        vec[n].offset = (int)off_in_rd;
        vec[n].length = (int)piece_len;
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
        "  -b bytes        Size of content (default: 8)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n"
        "  -s seed         Seed Number (default: -1)\n"
        "  -c check        Check read text is true (default: false)\n"
        "  -l log          Show Log (default: false)\n"
        "  -t test         Print result as csv form\n",
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
    size_t bytes_size = DEFAULT_BYTES_SIZE;
    long iterations = DEFAULT_ITERS;
    long seed = time(NULL);
    int check = 0;
    int log = 0;
    int csv = 0;

    int opt;
    while ((opt = getopt(argc, argv, "b:n:s:clt")) != -1) {
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
        case 'c': {
            check = 1;
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

    // get fstat
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        exit(1);
    }
    off_t filesize = st.st_size;

    // aligned memory allocation in buf
    /*void *buf;
    if (posix_memalign(&buf, ALIGN, block_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }*/
    
    /************ Prepare Stage End ************/

    //테스트 시작 전 시간 측정 (log용)
    struct timespec t_total0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    // Test Start
    for (long i = 0; i < iterations; i++) {
        if(log) {
            if(i % 1000 == 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

                double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                                + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

                fprintf(stderr, "\rFinegrained Read RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs", i, iterations, (double) i / iterations * 100, elapsed);
            }
        }
        
        off_t max_byte = filesize - bytes_size; //어딜 고르든 bytes size만큼 고를 수 있게
        off_t src_logical = (off_t)((double)rand() / RAND_MAX * max_byte); //random source

        finegrained_pba* seg = NULL;
        size_t seg_cnt = 0;

        /************ Fiemap0 ************/
        uint64_t fiemap_ns0;
        if (get_pba(fd, src_logical, bytes_size, &seg, &seg_cnt, &fiemap_ns0) != 0)
            continue;
        /************ Fiemap0 End ************/
        
        if(check) {
            for(size_t j = 0; j < seg_cnt; ++j) {
                printf("PBA: %lu, extent_bytes: %d, offset: %d, length: %d\n", seg[j].pba, seg[j].extent_bytes, seg[j].offset, seg[j].length);
            }
            printf("\n");
        }
        
        finegrained_read_params params;
        params.pba.pba_len = seg_cnt;
        params.pba.pba_val = seg;
        params.read_bytes = (int)bytes_size;


        struct timespec t_rpc0, t_rpc1;

        /************ RPC ************/

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc0);
        finegrained_read_returns *res = read_1(&params, clnt);
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc1);

        /************ RPC End ************/

        free(seg);
        if (res == NULL || res->value.value_len == 0) {
            fprintf(stderr, "RPC read failed\n");
            break;
        }
        if(check) {    
            off_t block_logical = src_logical - (src_logical % BLOCK_SIZE);
            off_t block_length = ((src_logical % BLOCK_SIZE) + bytes_size + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;

            off_t offset_in_block = src_logical - block_logical;

            char* expected_buf = malloc(block_length);
            if(!expected_buf) {
                perror("malloc failed");
                break;
            }

            ssize_t r = pread(fd, expected_buf, block_length, block_logical);
            printf("Read %s from server.\n", res->value.value_val);
            if(r != (ssize_t)block_length) {
                fprintf(stderr, "pread for check failed\n");
                free(expected_buf);
                break;
            }

            if(memcmp(expected_buf + offset_in_block, res->value.value_val, bytes_size) != 0) {
                fprintf(stderr, "Data mismatch at iteration %ld, logical offset %ld\n", i, src_logical);
                fprintf(stderr, "expected: %.*s, rpc: %.*s\n\n", (int)bytes_size, expected_buf + offset_in_block, (int)bytes_size, res->value.value_val);
            }
        }
        
	/************ Time Check End ************/

        uint64_t rpc_total_ns = ns_diff(t_rpc0, t_rpc1); //이번 iteration에서 rpc에 걸린 시간

        g_fiemap_ns += fiemap_ns0; //iteration 중 fiemap에 소요한 시간 총합
        g_rpc_total_ns += rpc_total_ns; //iteration 중 rpc에 소요한 시간 총합
    }

    if(log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

        double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                        + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

        fprintf(stderr, "\rFinegrained Read RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs", iterations, iterations, (double) 100, elapsed);
    }

    struct timespec t_total1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);


    /************ End Stage ************/
    //free(buf);
    close(fd);
    /************ End Stage End ************/

    
    // Get server time
    get_server_ios *res = get_time_1(NULL, clnt);
    if (res == NULL) {
        fprintf(stderr, "RPC get server time failed\n");
        clnt_destroy(clnt);
        exit(1);
    }
    clnt_destroy(clnt);

    //per iteration 시간 구하기 위해 iterations로 나눔
    uint64_t server_read_ns = res->server_read_time;
    uint64_t server_write_ns = res->server_write_time;
    uint64_t server_other_ns = res->server_other_time;
    uint64_t server_total_ns = server_read_ns + server_write_ns + server_other_ns;

    // Calculate elapsed time
    uint64_t total_ns = ns_diff(t_total0, t_total1); //total_ns를 iteration 자체에만 걸린 시간으로 바꿈!! end, prep 제외
    uint64_t fiemap_ns = g_fiemap_ns; //fiemap 걸린 시간 합
    uint64_t rpc_ns = g_rpc_total_ns - server_total_ns; //rpc 자체에만 드는 오버헤드
    uint64_t io_ns = total_ns - fiemap_ns - rpc_ns - server_total_ns;
	
    // 계산 오류 핸들러.
    if(fiemap_ns + rpc_ns + server_read_ns + server_write_ns + server_other_ns + io_ns != total_ns) {
        fprintf(stderr, "Time calculation failed. Do not match with total_ns\n");
        exit(1);
    }

    // Calculate statistics
    double throughput_mbps = (bytes_size / (1024.0 * 1024.0)) / get_elapsed(total_ns);

    server_read_ns /= iterations;
    server_write_ns /= iterations;
    server_other_ns /= iterations;
    server_total_ns /= iterations;
    total_ns /= iterations;
    fiemap_ns /= iterations;
    rpc_ns /= iterations;
    io_ns /= iterations;

    if(csv) {
        // byte_num, iteration, # of block_copies, file_size, Read time, Write time, (Server) Other time, Fiemap time, RPC time, I/O time, Total time
        printf("%lu,%ld,%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", 
            bytes_size, 
            iterations, 
            bytes_size * iterations, 
            (double)filesize / (1024.0 * 1024.0 * 1024.0),
            get_elapsed(server_read_ns),
            get_elapsed(server_write_ns),
            get_elapsed(server_other_ns),
            get_elapsed(fiemap_ns),
            get_elapsed(rpc_ns),
            get_elapsed(io_ns),
            get_elapsed(total_ns)
        );
        return 0;
    }
    printf("\n\n");
    printf("------------ Finegrained Read RPC Test Results ------------\n");
    printf("Iterations attempted: %ld\n", iterations);
    printf("Byte size: %zu bytes\n", bytes_size);
    printf("Seed: %ld\n", seed);
    printf("Log on: %s\n", log ? "true" : "false");
    printf("Check on: %s\n", check ? "true" : "false");
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
    printf("Summary: \n");
    printf("  Server Elapsed time: %.3f seconds\n", get_elapsed(server_read_ns + server_write_ns + server_other_ns));
    printf("  Client Main time: %.3f seconds\n", get_elapsed(fiemap_ns + rpc_ns + io_ns));
    printf("\n");
    printf("  Total Elapsed time: %.3f seconds\n", get_elapsed(total_ns));
    printf("  Approx throughput: %.2f MB/s\n", throughput_mbps);
    //printf("  Operations per second: %.2f ops/s\n", ops_per_sec);
    printf("------------------------------------------\n");

    return 0;
}