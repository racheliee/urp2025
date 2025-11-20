// filecmp.c — buffered file comparator (diff-like)
// - O_DIRECT 제거 (버퍼드 읽기)
// - posix_fadvise로 캐시 힌트만 제공
// - 빠른 동일 블록 스킵(memcmp)
// - 64-bit XOR + 0xFFULL 바이트 마스크로 정확한 바이트 차이 카운트
// - EOF 길이 차이도 바이트 단위로 보고
// - 위치 출력은 1-based

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

static void usage(const char* prog){
    fprintf(stderr,
        "Usage: %s [-b buf_bytes] [-m max_print] file1 file2\n"
        "  -b : buffer size (default: 8MiB; accepts K/M/G suffix)\n"
        "  -m : max differences to print (default: 100; 0 = print none; -1 = no limit)\n",
        prog);
}

static size_t parse_size(const char* s){
    // accepts K/M/G suffix (powers of 1024)
    char *end; unsigned long long v = strtoull(s, &end, 10);
    if (*end=='K'||*end=='k') v <<= 10;
    else if (*end=='M'||*end=='m') v <<= 20;
    else if (*end=='G'||*end=='g') v <<= 30;
    return (size_t)v;
}

int main(int argc, char** argv){
    size_t buf_sz = 8ULL<<20;     // 8 MiB
    long long max_print = 100;    // print cap
    int opt;
    while((opt = getopt(argc, argv, "b:m:")) != -1){
        if(opt=='b') buf_sz = parse_size(optarg);
        else if(opt=='m') max_print = atoll(optarg);
        else { usage(argv[0]); return 2; }
    }
    if(argc - optind != 2){ usage(argv[0]); return 2; }

    const char* f1p = argv[optind];
    const char* f2p = argv[optind+1];

    int fd1 = open(f1p, O_RDONLY|O_BINARY|O_DIRECT);
    if(fd1<0){ perror("open file1"); return 1; }
    int fd2 = open(f2p, O_RDONLY|O_BINARY|O_DIRECT);
    if(fd2<0){ perror("open file2"); close(fd1); return 1; }

    // 힌트: 순차 읽기이므로 캐시 최적화
#ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(fd1, 0, 0, POSIX_FADV_SEQUENTIAL);
    posix_fadvise(fd2, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#ifdef POSIX_FADV_NOREUSE
    posix_fadvise(fd1, 0, 0, POSIX_FADV_NOREUSE);
    posix_fadvise(fd2, 0, 0, POSIX_FADV_NOREUSE);
#endif

    // 버퍼 할당(정렬 불필요; 일반 read 사용)
    uint8_t *buf1 = (uint8_t*)malloc(buf_sz);
    uint8_t *buf2 = (uint8_t*)malloc(buf_sz);
    if(!buf1 || !buf2){
        fprintf(stderr,"malloc failed\n");
        close(fd1); close(fd2);
        free(buf1); free(buf2);
        return 1;
    }

    unsigned long long total_diffs = 0;
    unsigned long long printed = 0;
    unsigned long long pos = 0; // 1-based 출력용으로 +1

    for(;;){
        ssize_t n1 = read(fd1, buf1, buf_sz);
        if(n1 < 0){ perror("read file1"); break; }
        ssize_t n2 = read(fd2, buf2, buf_sz);
        if(n2 < 0){ perror("read file2"); break; }
        if(n1==0 && n2==0) break;

        size_t n_common = (n1 < n2) ? (size_t)n1 : (size_t)n2;
        size_t n_max    = (n1 > n2) ? (size_t)n1 : (size_t)n2;

        // 빠른 동일 블록 스킵 (공통 길이가 같고 완전 동일하면)
        if(n1 == n2 && n1 > 0 && memcmp(buf1, buf2, (size_t)n1) == 0){
            pos += (unsigned long long)n1;
            continue;
        }

        // (1) 공통 구간 비교: 64비트 단위로 빠르게, 나머지는 바이트 단위
        size_t i = 0;
        size_t w_end = n_common & ~(size_t)7; // 8바이트 경계
        while(i < w_end){
            uint64_t a = *(const uint64_t*)(buf1 + i);
            uint64_t b = *(const uint64_t*)(buf2 + i);
            uint64_t x = a ^ b;
            if(x==0){
                i += 8;
                continue;
            }
            // 8바이트 중 다른 바이트들만 출력
            while (x){
                unsigned bit = __builtin_ctzll(x);       // 가장 낮은 set bit 위치(0..63)
                unsigned byte_off = bit >> 3;            // 0..7
                size_t idx = i + byte_off;

                // 해당 바이트만 카운트/출력
                if (buf1[idx] != buf2[idx]) {
                    total_diffs++;
                    if(max_print < 0 || printed < (unsigned long long)max_print){
                        printf("[%llu] 0x%02X -> 0x%02X\n",
                               (unsigned long long)(pos + idx + 1),
                               buf1[idx], buf2[idx]);
                        printed++;
                    }
                }

                // 한 바이트(8비트) 제거 — 0xFFULL 사용 중요!
                x &= ~(0xFFULL << (byte_off*8));
            }
            i += 8;
        }
        for(; i < n_common; ++i){
            if(buf1[i] != buf2[i]){
                total_diffs++;
                if(max_print < 0 || printed < (unsigned long long)max_print){
                    printf("[%llu] 0x%02X -> 0x%02X\n",
                           (unsigned long long)(pos + i + 1),
                           buf1[i], buf2[i]);
                    printed++;
                }
            }
        }

        // (2) 길이가 다른 경우: 더 긴 쪽의 나머지 바이트는 EOF 대비 차이로 처리
        if (n1 != n2){
            const uint8_t *longer = (n2 > n1) ? buf2 : buf1;
            size_t start = n_common;
            size_t tail  = n_max - n_common;
            for(size_t k=0; k<tail; ++k){
                total_diffs++;
                if(max_print < 0 || printed < (unsigned long long)max_print){
                    if(n1 < n2){
                        // file1: EOF, file2: byte
                        printf("[%llu] EOF -> 0x%02X\n",
                               (unsigned long long)(pos + start + k + 1),
                               longer[start + k]);
                    }else{
                        // file1: byte, file2: EOF
                        printf("[%llu] 0x%02X -> EOF\n",
                               (unsigned long long)(pos + start + k + 1),
                               longer[start + k]);
                    }
                    printed++;
                }
            }
        }

        // 이번 라운드에서 비교한 최대 길이만큼 진행
        pos += (unsigned long long)n_max;
    }

    if(total_diffs==0){
        printf("두 파일은 완전히 동일합니다.\n");
    }else{
        printf("총 서로 다른 바이트: %llu\n", total_diffs);
        if(max_print >= 0 && (unsigned long long)max_print < total_diffs){
            printf("(표시 제한 %lld개로 일부만 출력됨)\n", max_print);
        }
    }

    free(buf1); free(buf2);
    close(fd1); close(fd2);
    return 0;
}

