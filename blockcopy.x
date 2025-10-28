/* blockcopy.x - RPC protocol for block copying with physical block addresses */

const MAX_CHUNK_SIZE = 16384;  /* 16KB max block size */

struct pba_write_params {
    hyper pba_src;                    /* physical block address (byte offset) */
    hyper pba_dst;                    /* physical block address (byte offset) */
    int nbytes;                       /* number of valid bytes */
};


program BLOCKCOPY_PROG {
    version BLOCKCOPY_VERS {
        unsigned hyper WRITE_PBA(pba_write_params) = 1;
    } = 1;
} = 0x34567890;
