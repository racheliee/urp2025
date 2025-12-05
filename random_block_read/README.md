# Random Block Copy

## Project Structure

```
.
├── blockcopy_random.x          # RPC specification file
├── client_random.h             # Client header
├── server_random.h             # Server header
├── client_random.c             # Client implementation
├── server_random.c             # Server implementation
├── Makefile                    # Build configuration
└── README.md                   # This file
```

## Basic Usage

```
make rebuild

# Generate RPC stubs only
make rpc

# Build only client
make client_random

# Build only server
make server_random
```

## Running the Server
To run the server, use the following command:
```
sudo ./server_random
```


## Running the Client
To run the client, use the following command:
```
./client_random <server_ip_address> <file_name> <options>

# Example:
./client_random eternity2 /mnt/nvme/1gb.txt -l
```


### Options
- `b <block_number>` - Number of blocks (1 block = 4096B, default: 1)
- `n <iterations>` - Number of random copy operations (default: 1000000)
- `s <seed>` - Random seed for reproducibility (default: current time)
- `l` - Enable progress logging
- `t` - Output results in CSV format
- `B <size>` - Batch size for RPC calls (default: 100, max: 1024)

