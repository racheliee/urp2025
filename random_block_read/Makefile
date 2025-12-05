# Makefile for batched random block copy client/server

# Compiler and flags
CC = gcc
CFLAGS = -Wall -O2 -D_GNU_SOURCE
LDFLAGS = 

# RPC compiler
RPCGEN = rpcgen

# Target executables
CLIENT = client_random
SERVER = server_random
BASELINE = baseline_random

# RPC specification file
RPC_SPEC = blockcopy_random.x

# Generated RPC files
RPC_CLNT_STUB = blockcopy_random_clnt.c
RPC_SVC_STUB = blockcopy_random_svc.c
RPC_XDR = blockcopy_random_xdr.c
RPC_HEADER = blockcopy_random.h

# Source files
CLIENT_SRC = client_random.c
SERVER_SRC = server_random.c
BASELINE_SRC = baseline_random.c

# Object files
CLIENT_OBJS = client_random.o blockcopy_random_clnt.o blockcopy_random_xdr.o
SERVER_OBJS = server_random.o blockcopy_random_svc.o blockcopy_random_xdr.o
BASELINE_OBJS = baseline_random.o

# Default target
all: $(CLIENT) $(SERVER) $(BASELINE)

# Generate RPC stubs and headers from .x file
rpc: $(RPC_SPEC)
	$(RPCGEN) $(RPC_SPEC)
	@# Fix uint32 type issue in generated header
	@if grep -q "uint32 " $(RPC_HEADER) 2>/dev/null; then \
		echo "Patching $(RPC_HEADER) to fix uint32 type..."; \
		sed -i.bak 's/uint32 /u_int32_t /g' $(RPC_HEADER); \
	fi

# Client executable
$(CLIENT): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Server executable
$(SERVER): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Baseline executable
$(BASELINE): $(BASELINE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Client object file
client_random.o: $(CLIENT_SRC) $(RPC_HEADER) client_random.h
	$(CC) $(CFLAGS) -c $(CLIENT_SRC)

# Server object file
server_random.o: $(SERVER_SRC) $(RPC_HEADER) server_random.h
	$(CC) $(CFLAGS) -c $(SERVER_SRC)

# Baseline object file
baseline_random.o: $(BASELINE_SRC)
	$(CC) $(CFLAGS) -c $(BASELINE_SRC)

# RPC client stub
blockcopy_random_clnt.o: $(RPC_CLNT_STUB) $(RPC_HEADER)
	$(CC) $(CFLAGS) -c $(RPC_CLNT_STUB)

# RPC server stub
blockcopy_random_svc.o: $(RPC_SVC_STUB) $(RPC_HEADER)
	$(CC) $(CFLAGS) -c $(RPC_SVC_STUB)

# RPC XDR functions
blockcopy_random_xdr.o: $(RPC_XDR) $(RPC_HEADER)
	$(CC) $(CFLAGS) -c $(RPC_XDR)

# Clean generated files
clean:
	rm -f $(CLIENT) $(SERVER) $(BASELINE) *.o
	rm -f $(RPC_CLNT_STUB) $(RPC_SVC_STUB) $(RPC_XDR) $(RPC_HEADER) $(RPC_HEADER).bak

# Clean only object files and executables (keep RPC generated files)
clean-build:
	rm -f $(CLIENT) $(SERVER) $(BASELINE) *.o

# Rebuild everything from scratch
rebuild: clean rpc all

# Install targets (optional - modify paths as needed)
install: all
	install -m 755 $(CLIENT) /usr/local/bin/
	install -m 755 $(SERVER) /usr/local/bin/
	install -m 755 $(BASELINE) /usr/local/bin/

# Uninstall
uninstall:
	rm -f /usr/local/bin/$(CLIENT)
	rm -f /usr/local/bin/$(SERVER)
	rm -f /usr/local/bin/$(BASELINE)

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build client, server, and baseline (default)"
	@echo "  rpc          - Generate RPC stubs from .x file"
	@echo "  client       - Build only client"
	@echo "  server       - Build only server"
	@echo "  baseline     - Build only baseline"
	@echo "  clean        - Remove all generated files"
	@echo "  clean-build  - Remove only executables and objects"
	@echo "  rebuild      - Clean and rebuild everything"
	@echo "  install      - Install binaries to /usr/local/bin"
	@echo "  uninstall    - Remove installed binaries"
	@echo "  help         - Show this help message"

.PHONY: all rpc clean clean-build rebuild install uninstall help