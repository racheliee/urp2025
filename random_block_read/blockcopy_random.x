/* blockcopy_random.x - RPC protocol for block copying with physical block addresses */

const MAX_BATCH = 1024;

/* Single-block copy parameters (old version — KEEP THIS!) */
struct pba_write_params {
    hyper pba_src;      /* one physical block offset */
    hyper pba_dst;      /* one physical block offset */
    int   nbytes;       /* block size */
};

/* Batched copy parameters (new version) */
struct pba_batch_params {
    hyper pba_srcs[MAX_BATCH];   /* array of PBAs */
    hyper pba_dsts[MAX_BATCH];   /* array of PBAs */
    unsigned int count;           /* how many elements are valid */
    unsigned int block_size;      /* size of each block */
};

/* Timing data returned from server */
struct get_server_ios {
    unsigned hyper server_read_time;
    unsigned hyper server_write_time;
    unsigned hyper server_other_time;
};

program BLOCKCOPY_PROG {
    version BLOCKCOPY_VERS {
        int WRITE_PBA(pba_write_params) = 1;
        get_server_ios GET_TIME(void) = 2;
        void RESET_TIME(void) = 3;
        int WRITE_PBA_BATCH(pba_batch_params) = 4;
    } = 1;
} = 0x34567890;