RPCGEN = rpcgen
CC = gcc
CFLAGS = -O2 -Wall # -I/usr/include/tirpc
LDLIBS  += -lnsl #-ltirpc
TARGETS = client_read client server 

all: $(TARGETS)

blockcopy.h blockcopy_clnt.c blockcopy_svc.c blockcopy_xdr.c: blockcopy.x
	$(RPCGEN) -C blockcopy.x

client: client.c client.h blockcopy_clnt.c blockcopy_xdr.c
	$(CC) $(CFLAGS) -o client client.c blockcopy_clnt.c blockcopy_xdr.c $(LDLIBS)

client_read: client_read.c client.h blockcopy_clnt.c blockcopy_xdr.c
	$(CC) $(CFLAGS) -o client_read client_read.c blockcopy_clnt.c blockcopy_xdr.c $(LDLIBS)

server: server.c server.h blockcopy_svc.c blockcopy_xdr.c
	$(CC) $(CFLAGS) -o server server.c blockcopy_svc.c blockcopy_xdr.c $(LDLIBS)

baseline: baseline.c
	$(CC) $(CFLAGS) -o baseline baseline.c

create_file: create_file.c
	$(CC) $(CFLAGS) -o create_file create_file.c

clean:
	rm -f $(TARGETS) *.o blockcopy.h blockcopy_clnt.c blockcopy_svc.c blockcopy_xdr.c
