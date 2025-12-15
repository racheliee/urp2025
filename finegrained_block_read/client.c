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
    uint64_t pba; //sector aligned
    int offset;
    int nbytes;
} pba_seg;

/*
struct finegrained_write_params {
    finegrained_pba pba<>;
    char value<>;
};
*/


static inline uint64_t ns_diff(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull
         + (uint64_t)(b.tv_nsec - a.tv_nsec);
}

static inline double get_elapsed(uint64_t ns) {
    return (double) ns / 1e3;
    //convert into micro second unit
}

/* helper to get physical block address from logical offset */
//static int get_pba(int fd, off_t logical, size_t length, pba_seg **out, size_t* out_cnt, uint64_t* fiemap_ns) {
static int get_pba(int fd, off_t logical, size_t length, pba_seg **out, size_t *out_cnt, uint64_t* fiemap_ns) {
    struct timespec t_before, t_after;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_before);

    size_t size = sizeof(struct fiemap) + EXTENTS_MAX * sizeof(struct fiemap_extent);
    struct fiemap *fiemap = (struct fiemap*)calloc(1, size);
    if(!fiemap) return -1;

    /*
    uint64_t logical = logical_base + offset;
    uint64_t request_start = logical
    uint64_t request_end = logical + length;
    */

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
    size_t seg_count = 0;
    if(!vec) {
        result = -1;
        goto exit;
    }

    uint64_t req_start = logical;
    uint64_t req_end   = logical + length;
    const uint64_t sector_size=512; //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!


    for(size_t i = 0; i < fiemap->fm_mapped_extents; ++i) {
        struct fiemap_extent *e = &fiemap->fm_extents[i];

        uint64_t estart = e->fe_logical;
        uint64_t eend   = e->fe_logical + e->fe_length;

        uint64_t overlap_start = (req_start > estart) ? req_start : estart;
        uint64_t overlap_end   = (req_end   < eend ) ? req_end   : eend;

        if (overlap_start >=overlap_end) {continue;}

        uint64_t seg_len = overlap_end - overlap_start;

        uint64_t physical =
            e->fe_physical + (overlap_start - e->fe_logical);

        // ---- sector align ----
        uint64_t mask    = ~(uint64_t)(sector_size - 1);
        uint64_t aligned = physical & mask;
        uint32_t off     = (uint32_t)(physical - aligned);

        // segment 추가
        vec[seg_count].pba    = aligned;
        vec[seg_count].offset = (int)off;
        vec[seg_count].nbytes = (int)seg_len;

        seg_count++;
    }


    if (seg_count == 0) { free(vec); result = -1; goto exit; }
    *out = vec, *out_cnt = seg_count;

exit:
    free(fiemap);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_after);
    *fiemap_ns = (uint64_t)ns_diff(t_before, t_after);
    return result;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <server_eternity> <file_path> [-b bytes] [-n iterations] [-s seed] [-l] [-t]\n"
        "Options:\n"
        "  -b bytes        Size of content (default: 8)\n"
        "  -n iterations   Number of random copies (default: 1000000)\n"
        "  -s seed         Seed Number (default: -1)\n"
        "  -l log          Show Log (default: false)\n"
        "  -t test         Print result as csv form\n",
        prog);
}

// static _Atomic uint64_t g_fiemap_ns  = 0;
// static _Atomic uint64_t g_rpc_total_ns = 0;

static uint64_t g_fiemap_ns = 0;
static uint64_t g_rpc_total_ns = 0;
static uint64_t g_iter_ns = 0;

int main(int argc, char *argv[]) {
    //printf("start of main\n");
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
    int log = 0;
    int csv = 0;

    int opt;
    while ((opt = getopt(argc, argv, "b:n:s:lt")) != -1) {
        switch (opt) {
        case 'b': {
            bytes_size = strtoul(optarg, NULL, 10);
            if (bytes_size <= 0) {
                fprintf(stderr, "Block size must be positive number.\n");
                return 1;
            }
            //block_size = ALIGN * block_num;
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

    printf("parsed option\n");
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

    /*
    char *buf = malloc(bytes_size); //server한테 보낼 내용
    if (!buf) {
        perror("malloc");
        exit(1);
        }
    */

    /************ Prepare Stage End ************/

    /*********** Main Loop *******************/
    //테스트 시작 전 시간 측정 (log용)
    struct timespec t_total0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total0);

    // Test Start
    for (long i = 0; i < iterations; i++) {
        /*
        //파일 내용 읽기
        if (lseek(fd, src_logical, SEEK_SET) == (off_t)-1) {
            perror("lseek");
            continue;
        }


        ssize_t rd = read(fd, buf, bytes_size);
        if (rd < 0) {
            perror("read");
            continue;
        }

        if ((size_t)rd != bytes_size) {
            fprintf(stderr, "short read: expected %zu, got %zd\n",bytes_size, rd);
            continue;
        }
        */

        struct timespec t_iter0, t_iter1, t_total;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_iter0);
        t_total=t_iter0;

        if(log) {
            if(i % 1000 == 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

                double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                                + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

                fprintf(stderr, "\rBlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs", i, iterations, (double) i / iterations * 100, elapsed);
            }
        }

        off_t max_byte = filesize - bytes_size; //어딜 고르든 bytes size만큼 고를 수 있게

        off_t src_logical = (off_t)((double)rand() / RAND_MAX * max_byte); //random source

        pba_seg* seg = NULL;
        size_t seg_cnt = 0;
        //size_t src_pba_cnt;

        /************ Fiemap0 ************/
        uint64_t fiemap_ns0;
        if (get_pba(fd, src_logical, bytes_size, &seg, &seg_cnt, &fiemap_ns0) != 0) {
            continue;
        }
        //uint64_t fiemap_ns1;
        //if (get_pba(fd, src_logical, bytes_size, &src_pba, &src_pba_cnt, &fiemap_ns0) != 0)
        //if (get_pba(fd, src_logical, bytes_size, &src_pba, &fiemap_ns0) != 0)
            //continue;
        /************ Fiemap0 End ************/

        /************ Fiemap1 ************/
        //dst 없으니까 필요 X
        //if (get_pba(fd, dst_logical, bytes_size, &dst_pba, &dst_pba_cnt, &fiemap_ns1) != 0)
            //continue;
        /************ Fiemap1 End ************/

        /*
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
        */




        //write할 내용
        char* write_buf = malloc(bytes_size);
        if (!write_buf) {
                perror("failed to malloc write_buf");
                free(seg);
                break;
        }

        for (int i=0 ; i < bytes_size; i++) {
                write_buf[i] = (rand() % 255)+1;
        }

        finegrained_pba *pbas = malloc(sizeof(finegrained_pba) * seg_cnt);
        if (!pbas) {
                perror("malloc: pbas");
                free(write_buf);
                free(seg);
                break;
        }

        for (size_t j = 0; j < seg_cnt; j++) {
            pbas[j].pba    = seg[j].pba;
            pbas[j].offset = seg[j].offset;
            pbas[j].nbytes = seg[j].nbytes;
        }


        // fill finegrained_write_params
        finegrained_read_params rparams; //define

        // pba
        rparams.pba.pba_len = seg_cnt;
        rparams.pba.pba_val=pbas;
        rparams.read_bytes = bytes_size;

        finegrained_read_returns *rres = read_1(&rparams, clnt);
        if (rres == NULL || rres->value.value_len != bytes_size) {
            fprintf(stderr, "RPC read failed\n");
            free(pbas);
            free(seg);
            continue;
        }

        char *read_buf = malloc(bytes_size);
        memcpy(read_buf, rres->value.value_val, bytes_size);

        // ***** RPC READ END *****

        // ****** WRITE START *****
        finegrained_write_params params;
        params.pba.pba_len = seg_cnt;
        params.pba.pba_val = pbas;
        params.value.value_len = bytes_size;
        params.value.value_val = read_buf;

        struct timespec t_rpc0, t_rpc1;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc0);
        int *res = write_1(&params, clnt);
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_rpc1);

        if (res == NULL || *res == -1) {
            fprintf(stderr, "RPC write failed (iter=%ld)\n", i);
            free(read_buf);
            free(pbas);
            free(seg);
            break;
        }
        // ***** RPC WRITE END *****
        free(seg);
        free(pbas);
        free(write_buf);
        //free(dst_pba);


        clock_gettime(CLOCK_MONOTONIC_RAW, &t_iter1);
        /************ Time Check End ************/

        uint64_t rpc_total_ns = ns_diff(t_rpc0, t_rpc1); //이번 iteration에서 rpc에 걸린 시간
        uint64_t iter_total_ns = ns_diff(t_iter0, t_iter1);
        // atomic_fetch_add_explicit(&g_fiemap_ns, fiemap_ns0, memory_order_relaxed);
        // atomic_fetch_add_explicit(&g_fiemap_ns, fiemap_ns1, memory_order_relaxed);
        // atomic_fetch_add_explicit(&g_rpc_total_ns, rpc_total_ns, memory_order_relaxed);

        //g_fiemap_ns += fiemap_ns0 + fiemap_ns1; //iteration 중 fiemap에 소요한 시간 총합
        g_fiemap_ns += fiemap_ns0;
        g_rpc_total_ns += rpc_total_ns; //iteration 중 rpc에 소요한 시간 총합
        g_iter_ns += iter_total_ns; //총 시간에 이번 iteration 시간 더함

    }

    if(log) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

        double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                        + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

        fprintf(stderr, "\rBlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs", iterations, iterations, (double) 100, elapsed);
    }

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
    uint64_t server_read_ns = (res->server_read_time);
    uint64_t server_write_ns = (res->server_write_time);
    uint64_t server_other_ns = (res->server_other_time);

    // Calculate elapsed time
    //uint64_t total_ns = ns_diff(t_total0, t_total1);
    uint64_t total_ns = g_iter_ns; //total_ns를 iteration 자체에만 걸린 시간으로 바꿈!! end, prep 제외
    uint64_t fiemap_ns = g_fiemap_ns; //fiemap 두 번 걸린 시간 합
    uint64_t rpc_ns = g_rpc_total_ns - server_read_ns - server_write_ns - server_other_ns; //rpc 자체에만 드는 오버헤드
    uint64_t io_ns = total_ns - fiemap_ns - rpc_ns;

    // 계산 오류 핸들러.
    if(fiemap_ns + rpc_ns + server_read_ns + server_write_ns + server_other_ns + io_ns != total_ns) {
        fprintf(stderr, "Time calculation failed. Do not match with total_ns\n");
        exit(1);
    }
    printf("\nserver_read_ns = %ld\n", server_read_ns);
    printf("server_write_ns = %ld\n", server_write_ns);
    printf("total_ns = %ld\n", total_ns/100000);
    printf("fiemap_ns = %ld\n", fiemap_ns/100000);
    printf("rpc_ns = %ld\n", rpc_ns/100000);
    printf("io_ns = %ld\n", io_ns/100000);
    server_read_ns /= iterations;
    server_write_ns /= iterations;
    server_other_ns /= iterations;
    total_ns /= iterations;
    fiemap_ns /= iterations;
    rpc_ns /= iterations;
    io_ns /= iterations;


    // Calculate statistics
    long long total_bytes = (long long)iterations * bytes_size; // write한 바이트 수
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / get_elapsed(total_ns);

    if(csv) {
        // bytes_size, iteration, total size, file_size, Read time, Write time, (Server) Other time, Fiemap time, RPC time, I/O time, Total time
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
    printf("------------ RPC Test Results ------------\n");
    printf("Iterations attempted: %ld\n", iterations);
    printf("Bytes size: %zu bytes\n", bytes_size);
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
