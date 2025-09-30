#define _GNU_SOURCE
#include "server.h"
#include "blockcopy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int *write_pba_1_svc(pba_write_params *params, struct svc_req *rqstp) {
    static int result = 0;
    static int fd = -1;

    if (fd == -1) {
        fd = open(DEVICE_PATH, O_WRONLY | O_DIRECT);
        if (fd < 0) {
            perror("open device");
            result = -1;
            return &result;
        }
    }

    ssize_t w = pwrite(fd, params->data.data_val, params->nbytes, params->pba);
    if (w < 0) {
        perror("pwrite");
        result = -1;
    } else {
        result = 0;
    }

    // sanity check; remove later
    if (params->nbytes > 0) {
        unsigned char *bytes = (unsigned char *)params->data.data_val;
        printf("[Server] Wrote %d bytes at PBA=%lu FirstBytes=%02x %02x %02x %02x\n",
               params->nbytes, (unsigned long)params->pba,
               bytes[0], bytes[1], bytes[2], bytes[3]);
        fflush(stdout);
    } // end of sanity check

    return &result;
}
