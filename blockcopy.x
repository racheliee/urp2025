/* blockcopy.x - RPC protocol for block copying with physical block addresses */

const MAX_CHUNK_SIZE = 16384;  /* 16KB max block size */

struct pba_write_params {
    hyper pba;                        /* physical block address (byte offset) */
    opaque data<MAX_CHUNK_SIZE>;      /* data to write */
    int nbytes;                       /* number of valid bytes */
};

program BLOCKCOPY_PROG {
    version BLOCKCOPY_VERS {
        int WRITE_PBA(pba_write_params) = 1;
    } = 1;
} = 0x34567890;
