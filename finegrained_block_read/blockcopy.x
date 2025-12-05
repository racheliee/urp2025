/* blockcopy.x - RPC protocol for block copying with physical block addresses */

struct finegrained_pba {
    hyper pba;
    int offset;
    int nbytes;
};

struct finegrained_read_params {
    finegrained_pba pba<>;
    int read_bytes;
};

struct finegrained_read_returns {
    char value<>;
};

struct finegrained_write_params {
    finegrained_pba pba<>;
    char value<>;
};

struct get_server_ios {
    unsigned hyper server_read_time;
    unsigned hyper server_write_time;
    unsigned hyper server_other_time;
};

program BLOCKCOPY_PROG {
    version BLOCKCOPY_VERS {
        finegrained_read_returns READ(finegrained_read_params) = 1;
        int WRITE(finegrained_write_params) = 2;
        get_server_ios GET_TIME(void) = 3;
        void RESET_TIME(void) = 4;
    } = 1;
} = 0x34567890;
