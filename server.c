#define _GNU_SOURCE
#include "blockcopy.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

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

    return &result;
}
