RPCGEN = rpcgen
CC = gcc
CFLAGS = -O2 -Wall
TARGETS = client server create_file baseline

all: $(TARGETS)

blockcopy.h blockcopy_clnt.c blockcopy_svc.c blockcopy_xdr.c: blockcopy.x
	$(RPCGEN) -C blockcopy.x

client: client.c client.h blockcopy_clnt.c blockcopy_xdr.c
	$(CC) $(CFLAGS) -o client client.c blockcopy_clnt.c blockcopy_xdr.c -lnsl

server: server.c server.h blockcopy_svc.c blockcopy_xdr.c
	$(CC) $(CFLAGS) -o server server.c blockcopy_svc.c blockcopy_xdr.c -lnsl

baseline: baseline.c
	$(CC) $(CFLAGS) -o baseline baseline.c

create_file: create_file.c
	$(CC) $(CFLAGS) -o create_file create_file.c

clean:
	rm -f $(TARGETS) *.o blockcopy.h blockcopy_clnt.c blockcopy_svc.c blockcopy_xdr.c
