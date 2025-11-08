#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <tirpc/rpc/rpc.h>
#include <tirpc/rpc/svc_rqst.h>
#include "blockcopy.h"
#include "server.h"

static void usage(char* argv) {
    fprintf(stderr, "Usage: %s <thread_num>\n", argv);
}

int g_worker_count = 0;
__thread int g_worker_idx = -1;

pub_slot_t g_pub[MAX_WORKERS];

static void *rqst_worker(void *arg) {
    g_worker_idx = *(int*)arg;
    svc_rqst_thrd_run(NULL);
    return NULL;
}

static void on_sigint(int signo) {
    svc_exit();
}

int main(int argc, char** argv) {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if(argc < 2) {
        usage(argv[0]);
        exit(1);
    }

    g_worker_count = strtoul(argv[1], NULL, 10);
    if(g_worker_count <= 0 || g_worker_count > MAX_WORKERS) {
        usage(argv[0]);
        exit(1);
    }

    register SVCXPRT *transp;

	pmap_unset (BLOCKCOPY_PROG, BLOCKCOPY_VERS);

	transp = svcudp_create(RPC_ANYSOCK);
	if (transp == NULL) {
		fprintf (stderr, "%s", "cannot create udp service.");
		exit(1);
	}
	if (!svc_register(transp, BLOCKCOPY_PROG, BLOCKCOPY_VERS, blockcopy_prog_1, IPPROTO_UDP)) {
		fprintf (stderr, "%s", "unable to register (BLOCKCOPY_PROG, BLOCKCOPY_VERS, udp).");
		exit(1);
	}

	transp = svctcp_create(RPC_ANYSOCK, 0, 0);
	if (transp == NULL) {
		fprintf (stderr, "%s", "cannot create tcp service.");
		exit(1);
	}
	if (!svc_register(transp, BLOCKCOPY_PROG, BLOCKCOPY_VERS, blockcopy_prog_1, IPPROTO_TCP)) {
		fprintf (stderr, "%s", "unable to register (BLOCKCOPY_PROG, BLOCKCOPY_VERS, tcp).");
		exit(1);
	}

    pthread_t *th = malloc(sizeof(pthread_t) * g_worker_count);
    if(!th) {
        perror("malloc");
        exit(1);
    }

    int *idx = malloc(sizeof(int)*g_worker_count);
    if(!idx) {
        perror("malloc");
        free(th);
        exit(1);
    }

    for (int i = 0; i < g_worker_count; ++i) {
        idx[i] = i;
        if (pthread_create(&th[i], NULL, rqst_worker, &idx[i]) != 0) {
            perror("pthread_create");
            free(th);
            free(idx);
            exit(1);
        }
    }

    for (int i = 0; i < g_worker_count; ++i) pthread_join(th[i], NULL);
    free(th);
    free(idx);
    return 0;
}