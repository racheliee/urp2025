# --- Tools ---
RPCGEN      = rpcgen
CC          = gcc
PKG_CONFIG ?= pkg-config

# --- Flags ---
CFLAGS   = -O2 -Wall -pthread
CPPFLAGS = -D_GNU_SOURCE
LDFLAGS  = -pthread
TIRPC_CFLAGS := $(shell $(PKG_CONFIG) --cflags libtirpc 2>/dev/null)
TIRPC_LIBS   := $(shell $(PKG_CONFIG) --libs   libtirpc 2>/dev/null)

ifeq ($(strip $(TIRPC_CFLAGS)),)
  CPPFLAGS += -I/usr/include/tirpc
else
  CPPFLAGS += $(TIRPC_CFLAGS)
endif

ifeq ($(strip $(TIRPC_LIBS)),)
  LDLIBS += -ltirpc
else
  LDLIBS += $(TIRPC_LIBS)
endif

RPCGEN_FLAGS = -C -M

TARGETS = client server server_mt baseline create_file compare

.PHONY: all clean
all: $(TARGETS)

blockcopy.h blockcopy_clnt.c blockcopy_svc.c blockcopy_xdr.c: blockcopy.x
	$(RPCGEN) $(RPCGEN_FLAGS) blockcopy.x

client: client.c client.h blockcopy_clnt.c blockcopy_xdr.c blockcopy.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ client.c blockcopy_clnt.c blockcopy_xdr.c $(LDFLAGS) $(LDLIBS)

server: server.c server.h blockcopy_svc.c blockcopy_xdr.c blockcopy.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ server.c blockcopy_svc.c blockcopy_xdr.c $(LDFLAGS) $(LDLIBS)

# 의존성 체크용 server_mt: 실제 MT 플래그 없이 동일하게 빌드만 수행
server_mt: server.c server.h blockcopy_svc.c blockcopy_xdr.c blockcopy.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ server.c blockcopy_svc.c blockcopy_xdr.c $(LDFLAGS) $(LDLIBS)

baseline: baseline.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ baseline.c $(LDFLAGS) $(LDLIBS)

create_file: create_file.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ create_file.c $(LDFLAGS) $(LDLIBS)

compare: compare.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ compare.c $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGETS) *.o blockcopy.h blockcopy_clnt.c blockcopy_svc.c blockcopy_xdr.c
