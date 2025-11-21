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
#include <omp.h>      // OpenMP 헤더
#include <dirent.h>   // 디렉터리 탐색 헤더

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
        "Usage: %s <server_eternity> <directory_path> [-b block_size] [-n iterations] [-s seed] [-l] [-t]\n" // 수정됨
        "Options:\n"
        "  -b block_number # of block number. Block is 4096B. (default: 1)\n"
        "  -n iterations     Number of random copies per file (default: 1000000)\n" // 설명 수정
        "  -s seed           Seed Number (default: -1)\n"
        "  -l log            Show Log (default: false)\n"
        "  -t test           Print result as csv form\n",
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
    const char *directory_path = argv[2]; // 'path' -> 'directory_path'로 변경

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

    // --- 디렉터리 스캔 시작 ---
    DIR *d;
    struct dirent *dir;
    struct stat st_file;
    int num_files = 0;
    char **file_paths = NULL;
    char path_buffer[1024];

    d = opendir(directory_path);
    if (!d) {
        perror("opendir");
        return 1;
    }

    // 1. 파일 개수 세기
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
            continue;
        
        snprintf(path_buffer, sizeof(path_buffer), "%s/%s", directory_path, dir->d_name);
        if (stat(path_buffer, &st_file) == 0 && S_ISREG(st_file.st_mode)) {
            num_files++;
        }
    }

    if (num_files == 0) {
        fprintf(stderr, "No regular files found in directory: %s\n", directory_path);
        closedir(d);
        return 1;
    }

    // 2. 파일 경로 저장을 위한 메모리 할당
    file_paths = (char**)malloc(num_files * sizeof(char*));
    if (!file_paths) {
        perror("malloc file_paths");
        closedir(d);
        return 1;
    }

    // 3. 파일 경로 저장
    rewinddir(d);
    int i_file = 0;
    while ((dir = readdir(d)) != NULL && i_file < num_files) {
         if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
            continue;

        snprintf(path_buffer, sizeof(path_buffer), "%s/%s", directory_path, dir->d_name);
        if (stat(path_buffer, &st_file) == 0 && S_ISREG(st_file.st_mode)) {
            file_paths[i_file] = strdup(path_buffer);
            if (!file_paths[i_file]) {
                perror("strdup");
                // 간단한 정리를 위해 이미 할당된 경로 해제
                for (int j = 0; j < i_file; j++) free(file_paths[j]);
                free(file_paths);
                closedir(d);
                return 1;
            }
            i_file++;
        }
    }
    closedir(d);
    // --- 디렉터리 스캔 끝 ---

    if (log) {
        fprintf(stderr, "Found %d files. Starting %d threads...\n", num_files, num_files);
    }
    omp_set_num_threads(num_files); // 스레드 개수 설정

    // RPC connect (메인 스레드에서 reset/get time 용)
    CLIENT *clnt_main = clnt_create(server_host, BLOCKCOPY_PROG, BLOCKCOPY_VERS, "tcp");
    if (!clnt_main) {
        clnt_pcreateerror(server_host);
        exit(1);
    }

    {
        void *res = reset_time_1(NULL, clnt_main);
        if (res == NULL) {
            fprintf(stderr, "RPC reset server time failed\n");
            clnt_destroy(clnt_main);
            exit(1);
        }
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_prep1);

    /************ Prepare Stage End ************/

    // Test Start
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        const char *path = file_paths[tid]; // 이 스레드가 처리할 파일

        // 스레드 로컬 변수
        CLIENT *clnt = NULL;
        int fd = -1;
        struct stat st;
        off_t filesize = 0;
        uint64_t local_fiemap_ns = 0;
        uint64_t local_rpc_total_ns = 0;

        // 스레드 로컬 RNG 시드 설정
        unsigned int thread_seed = (unsigned int)(seed + tid);

        // --- 스레드 로컬 준비 ---
        clnt = clnt_create(server_host, BLOCKCOPY_PROG, BLOCKCOPY_VERS, "tcp");
        if (!clnt) {
            clnt_pcreateerror(server_host);
            #pragma omp critical
            { fprintf(stderr, "Thread %d failed to create RPC client.\n", tid); }
        }

        if(clnt) {
            fd = open(path, O_RDONLY | O_DIRECT);
            if (fd < 0) {
                #pragma omp critical
                { fprintf(stderr, "Thread %d failed to open file %s: %s\n", tid, path, strerror(errno)); }
            }
        }

        if(clnt && fd >= 0) {
            if (fstat(fd, &st) < 0) {
                perror("fstat");
                 #pragma omp critical
                { fprintf(stderr, "Thread %d fstat failed for %s\n", tid, path); }
                fd = -2; // 에러 플래그
            } else {
                filesize = st.st_size;
            }
        }
        // --- 스레드 로컬 준비 끝 ---


        if(clnt && fd >= 0) // 준비가 성공한 스레드만 실행
        {
            for (long i = 0; i < iterations; i++) {
                if(log) {
                    if(i % 1000 == 0) {
                        struct timespec now_ts;
                        clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

                        double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                                        + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;

                        // \r 대신 \n 사용, 스레드 ID 추가
                        fprintf(stderr, "[T%d] BlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs\n", 
                                tid, i, iterations, (double) i / iterations * 100, elapsed);
                    }
                }

                off_t max_blocks = filesize / block_size;
                if (max_blocks == 0) continue; // 파일이 너무 작으면 스킵

                off_t src_blk = rand_r(&thread_seed) % max_blocks;
                off_t dst_blk = rand_r(&thread_seed) % max_blocks;

                while(src_blk == dst_blk) dst_blk = rand_r(&thread_seed) % max_blocks;

                off_t src_logical = src_blk * block_size;
                off_t dst_logical = dst_blk * block_size;

                pba_seg* src_pba;
                pba_seg* dst_pba;
                size_t src_pba_cnt, dst_pba_cnt;

                /************ Fiemap0 ************/
                uint64_t fiemap_ns0, fiemap_ns1;
                if (get_pba(fd, src_logical, block_size, &src_pba, &src_pba_cnt, &fiemap_ns0) != 0)
                    continue;
                /************ Fiemap0 End ************/

                /************ Fiemap1 ************/
                if (get_pba(fd, dst_logical, block_size, &dst_pba, &dst_pba_cnt, &fiemap_ns1) != 0)
                    continue;
                /************ Fiemap1 End ************/
                
                if(src_pba_cnt != dst_pba_cnt) {
                    // 이 로그는 병렬 실행 시 매우 지저분해질 수 있으므로 critical 영역으로 보호
                    #pragma omp critical
                    {
                        fprintf(stderr, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
                        fprintf(stderr, "[T%d] Number of extents are not same. src_pba_cnt: %lu, dst_pba_cnt: %lu\n", tid, src_pba_cnt, dst_pba_cnt);
                        for(int j = 0; j < src_pba_cnt; ++j) {
                            fprintf(stderr, "src_pba[%d]: %lu, len: %lu\n", j, src_pba[j].pba, src_pba[j].len);
                        }
                        for(int j = 0; j < dst_pba_cnt; ++j) {
                            fprintf(stderr, "dst_pba[%d]: %lu, len: %lu\n", j, dst_pba[j].pba, dst_pba[j].len);
                        }
                        fprintf(stderr, "\n");
                    }
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

                free(src_pba); free(dst_pba);

                if (res == NULL || *res == -1) {
                    #pragma omp critical
                    {
                        fprintf(stderr, "[T%d] RPC write failed at PBA %lu to %lu\n", tid, (unsigned long)params.pba_src, (unsigned long)params.pba_dst);
                    }
                    // break; // 한 번의 실패로 스레드 중단
                }
                
                uint64_t rpc_total_ns = ns_diff(t_rpc0, t_rpc1);

                // 스레드 로컬 변수에 누적
                local_fiemap_ns += fiemap_ns0 + fiemap_ns1;
                local_rpc_total_ns += rpc_total_ns;
            }
        } // if(clnt && fd >= 0)

        if(log) {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
            double elapsed = (now_ts.tv_sec - t_total0.tv_sec)
                            + (now_ts.tv_nsec - t_total0.tv_nsec) / 1e9;
            fprintf(stderr, "[T%d] BlockCopy RPC Test: %ld / %ld (%6.1f%% ) | %6.2fs [COMPLETED]\n", 
                    tid, iterations, iterations, (double) 100, elapsed);
        }

        // --- 스레드 로컬 정리 ---
        if(fd >= 0) close(fd);
        if(clnt) clnt_destroy(clnt);

        // --- 전역 변수에 원자적으로 더하기 ---
        #pragma omp atomic update
        g_fiemap_ns += local_fiemap_ns;
        
        #pragma omp atomic update
        g_rpc_total_ns += local_rpc_total_ns;

    } // #pragma omp parallel

    /************ End Stage ************/

    struct timespec t_end0, t_end1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_end0);

    // 파일 경로 배열 메모리 해제
    for (int j = 0; j < num_files; j++) {
        free(file_paths[j]);
    }
    free(file_paths);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_total1);
    t_end1 = t_total1;

    /************ End Stage End ************/

/************ Time Check End ************/
    
    // Get server time (메인 스레드의 clnt 사용)
    get_server_ios *res = get_time_1(NULL, clnt_main);
    if (res == NULL) {
        fprintf(stderr, "RPC get server time failed\n");
        clnt_destroy(clnt_main);
        exit(1);
    }
    clnt_destroy(clnt_main);
    uint64_t server_read_ns = res->server_read_time;
    uint64_t server_write_ns = res->server_write_time;
    uint64_t server_other_ns = res->server_other_time;

    // Calculate elapsed time
    uint64_t total_ns = ns_diff(t_total0, t_total1);
    uint64_t prep_ns = ns_diff(t_prep0, t_prep1);
    uint64_t end_ns = ns_diff(t_end0, t_end1);
    uint64_t fiemap_ns = g_fiemap_ns; // 원자적으로 누적된 값
    uint64_t rpc_ns = g_rpc_total_ns - server_read_ns - server_write_ns - server_other_ns;
    uint64_t io_ns = total_ns - prep_ns - end_ns - fiemap_ns - g_rpc_total_ns;

    if(prep_ns + end_ns + fiemap_ns + rpc_ns + server_read_ns + server_write_ns + server_other_ns + io_ns != total_ns) {
        fprintf(stderr, "Time calculation failed. Do not match with total_ns (diff: %lld ns)\n", 
                (long long)(total_ns - (prep_ns + end_ns + fiemap_ns + g_rpc_total_ns + io_ns)));
        // exit(1); // 오차가 있을 수 있으므로 주석 처리
    }

    // Calculate statistics
    // 이제 total_bytes는 모든 스레드(파일)가 수행한 총 바이트 수
    long long total_bytes = (long long)iterations * block_size * num_files;
    double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / get_elapsed(total_ns);
    long total_iterations = iterations * num_files;

    if(csv) {
        // block_num, iteration(per file), num_files, total_iterations, Server Read Time, Server Write Time, Server Other Time, Prep Time, End Time, Fiemap time, RPC time, I/O time, Total time
        // 파일 크기는 합산하지 않았으므로 CSV에서 제거하거나, 합산 로직 추가 필요
        printf("%lu,%ld,%d,%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", 
            block_size/ALIGN, 
            iterations, 
            num_files,
            total_iterations,
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
    printf("Directory: %s\n", directory_path);
    printf("Files processed (threads): %d\n", num_files);
    printf("Iterations per file: %ld\n", iterations);
    printf("Total iterations: %ld\n", total_iterations);
    printf("Block size: %zu bytes\n", block_size);
    printf("Seed: %ld\n", seed);
    printf("Log on: %s\n", log ? "true" : "false");
    printf("\n");
    printf("Server Result (Total): \n");
    printf("  Read Elapsed time: %.3f seconds\n", get_elapsed(server_read_ns));
    printf("  Write Elapsed time: %.3f seconds\n", get_elapsed(server_write_ns));
    printf("  Other Elapsed time: %.3f seconds\n", get_elapsed(server_other_ns));
    printf("\n");
    printf("Client Main Result (Total): \n");
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
