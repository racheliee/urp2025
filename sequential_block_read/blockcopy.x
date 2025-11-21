/* blockcopy.x - RPC protocol for block copying with physical block addresses */

struct pba_write_params {
    hyper pba_src;                    /* physical block address (byte offset) */
    hyper pba_dst;                    /* physical block address (byte offset) */
    int nbytes;                       /* number of valid bytes */
};

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
    } = 1;
} = 0x34567890;
