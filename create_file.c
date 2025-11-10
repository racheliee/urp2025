// filegen.c - Generate a large file filled with random A–Z letters, showing progress/speed/ETA.
// Usage:
//   filegen <output_path> <size_in_GB> [--gib] [--chunk <MiB>]
//   --gib         : interpret size as GiB (2^30) instead of GB (10^9)
//   --chunk <MiB> : chunk size in MiB (default 64)
//
// Cross-platform: Linux/macOS/Windows (MSVC)
//
// Notes:
// - Uses a fast xorshift64* PRNG seeded from time and address entropy.
// - Flushes to disk with fsync (POSIX) or _commit (Windows) to reduce caching effects.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #define fsync _commit
#else
  #include <unistd.h>
  #include <sys/time.h>
  #include <fcntl.h>
#endif

// ---------------------- Options ----------------------

#define DEFAULT_CHUNK_MIB 64

typedef struct {
    const char* path;
    long double sizeGB;     // requested size in GB or GiB units
    int useGiB;             // 0: GB(10^9)  1: GiB(2^30)
    size_t chunkMiB;        // chunk size in MiB
} Options;

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s <output_path> <size_in_GB> [--gib] [--chunk <MiB>]\n"
        "  --gib           : interpret size as GiB (1 GiB = 1024^3 bytes). Default is GB (10^9).\n"
        "  --chunk <MiB>   : write chunk size in MiB (default %d).\n",
        prog, DEFAULT_CHUNK_MIB
    );
}

static int parse_args(int argc, char** argv, Options* opt) {
    if (argc < 3) return 0;
    opt->path = argv[1];

    char* endp = NULL;
    errno = 0;
    long double sz = strtold(argv[2], &endp);
    if (errno != 0 || endp == argv[2] || sz <= 0.0L) return 0;
    opt->sizeGB = sz;

    opt->useGiB = 0;
    opt->chunkMiB = DEFAULT_CHUNK_MIB;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--gib") == 0) {
            opt->useGiB = 1;
        } else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            char* e2 = NULL;
            errno = 0;
            long long m = strtoll(argv[++i], &e2, 10);
            if (errno != 0 || e2 == argv[i] || m <= 0) return 0;
            opt->chunkMiB = (size_t)m;
        } else {
            return 0;
        }
    }
    return 1;
}

// ---------------------- Timing ----------------------

static double now_sec(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER cnt;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
#endif
}

// ---------------------- PRNG (xorshift64*) ----------------------

static uint64_t rng_state = 0;

static void rng_seed(uint64_t s) {
    if (s == 0) s = 0x9e3779b97f4a7c15ULL; // non-zero default
    rng_state = s;
}

static uint64_t rng_next(void) {
    // xorshift64*
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void seed_from_time_and_addr(void) {
    uint64_t s = (uint64_t)time(NULL);
    s ^= (uint64_t)(uintptr_t)&s;
    s ^= (uint64_t)(uintptr_t)&rng_state << 32;
#if defined(_WIN32)
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    s ^= (uint64_t)cnt.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s ^= ((uint64_t)tv.tv_sec << 32) ^ (uint64_t)tv.tv_usec;
#endif
    rng_seed(s);
}

static void fill_random_AZ(unsigned char* buf, size_t n) {
    // Fill with A..Z using bytes from the PRNG state.
    size_t i = 0;
    while (i < n) {
        uint64_t r = rng_next();
        for (int k = 0; k < 8 && i < n; ++k) {
            unsigned char b = (unsigned char)(r & 0xFF);
            buf[i++] = (unsigned char)('A' + (b % 26));
            r >>= 8;
        }
    }
}

// ---------------------- Humanize ----------------------

static void humanize(double bytes, char* out, size_t outlen, const char* unitSuffix) {
    const char* suf[] = {"", "K", "M", "G", "T", "P"};
    int idx = 0;
    double v = bytes;
    while (v >= 1024.0 && idx < 5) { v /= 1024.0; ++idx; }
    snprintf(out, outlen, "%.2f %s%s", v, suf[idx], unitSuffix);
}

// ---------------------- Main ----------------------

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        print_usage(argv[0]);
        return 1;
    }

    // Target bytes
    long double base = opt.useGiB ? (1024.0L * 1024.0L * 1024.0L) : (1000.0L * 1000.0L * 1000.0L);
    long double target_ld = opt.sizeGB * base;
    if (target_ld <= 0.0L) {
        fprintf(stderr, "Invalid size.\n");
        return 1;
    }
    uint64_t target = (uint64_t)(target_ld + 0.5L);

    // Open file for binary write
#if defined(_WIN32)
    FILE* fp = fopen(opt.path, "wb");
#else
    FILE* fp = fopen(opt.path, "wb");
#endif
    if (!fp) {
        fprintf(stderr, "fopen failed: %s\n", strerror(errno));
        return 1;
    }

#if defined(__linux__)
    // Provide sequential IO hint (best-effort)
    int fd_adv = fileno(fp);
    (void)fd_adv; // suppress unused warning if posix_fadvise is unavailable
    // posix_fadvise(fd_adv, 0, 0, POSIX_FADV_SEQUENTIAL); // Uncomment if available
#endif

    // Buffer
    const size_t chunkSize = opt.chunkMiB * 1024ULL * 1024ULL;
    unsigned char* buf = (unsigned char*)malloc(chunkSize);
    if (!buf) {
        fprintf(stderr, "malloc(%zu) failed\n", chunkSize);
        fclose(fp);
        return 1;
    }

    // Seed PRNG
    seed_from_time_and_addr();

    uint64_t written = 0;
    const double t0 = now_sec();
    double lastPrint = t0;

    // Progress printer
    const double printInterval = 0.25; // seconds

    // Write loop
    while (written < target) {
        size_t toWrite = (size_t)((target - written) < (uint64_t)chunkSize ? (target - written) : (uint64_t)chunkSize);
        fill_random_AZ(buf, toWrite);

        size_t n = fwrite(buf, 1, toWrite, fp);
        if (n != toWrite) {
            fprintf(stderr, "\nWrite error at %" PRIu64 " bytes: %s\n", written, strerror(errno));
            free(buf);
            fclose(fp);
            return 2;
        }
        written += n;

        double now = now_sec();
        if (now - lastPrint >= printInterval || written == target) {
            lastPrint = now;
            double elapsed = now - t0;
            double pct = target ? (100.0 * (double)written / (double)target) : 100.0;
            double speed = (elapsed > 0.0) ? ((double)written / elapsed) : 0.0;
            double eta = (speed > 0.0 && written < target) ? ((double)(target - written) / speed) : 0.0;

            char hWritten[64], hTarget[64], hSpeed[64];
            humanize((double)written, hWritten, sizeof(hWritten), "B");
            humanize((double)target,  hTarget,  sizeof(hTarget),  "B");
            humanize(speed,           hSpeed,   sizeof(hSpeed),   "B/s");

            // \r 로 진행 상태 덮어쓰기
            fprintf(stdout, "\r%6.1f%%  %12s / %12s  |  %10s  ETA: %5s",
                pct, hWritten, hTarget, hSpeed,
                (eta > 0.0) ? (snprintf(NULL, 0, "%ds", (int)eta), "") : ""
            );
            if (eta > 0.0) {
                // 위에서 ETA 문자열 길이를 계산만 했으므로 실제 출력:
                fprintf(stdout, "\r%6.1f%%  %12s / %12s  |  %10s  ETA: %5ds",
                        pct, hWritten, hTarget, hSpeed, (int)eta);
            }
            fflush(stdout);
        }
    }

    // Flush to disk
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    // Final line
    {
        char hTarget[64];
        humanize((double)target, hTarget, sizeof(hTarget), "B");
        fprintf(stdout, "\nDone: created \"%s\" (%s).\n", opt.path, hTarget);
    }

    free(buf);
    return 0;
}

