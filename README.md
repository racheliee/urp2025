# RPC Layer for Remote Block Copying

This project implements a block-level file transfer mechanism using **Remote Procedure Calls (RPC)**. Instead of copying files through high-level abstractions, the system operates directly on the block mappings provided by the underlying filesystem. By leveraging the Linux **FIEMAP** interface, the client discovers the physical extents of a file and transmits block data over the network to a remote server. The server reconstructs the file by writing the received blocks to its own filesystem.

## Motivation

Traditional file transfer tools (e.g., `scp`, `rsync`) operate at the file-content level, reading sequentially and transmitting every byte. In contrast, a block-level RPC copy system enables:

* **Efficiency**: Copy only allocated blocks, skip sparse regions.
* **Parallelism**: Transfer independent blocks concurrently to maximize throughput.
* **Flexibility**: Support low-level experiments in distributed storage, replication, and filesystem research.

## Architecture

1. **Client**

   * Queries FIEMAP via `ioctl(FS_IOC_FIEMAP)` to discover file extents (logical ↔ physical block mapping).
   * Reads blocks (`pread`) from the source file according to extent layout.
   * Streams blocks to the remote server via RPC calls.

2. **RPC Layer**

   * Provides an abstraction for block operations (`BeginFile`, `WriteBlock`, `EndFile`).
   * Handles marshalling, retries, and integrity checks (CRC/SHA).
   * Can be implemented using simple sockets or frameworks such as **gRPC**.

3. **Server**

   * Receives block write requests.
   * Writes blocks to the destination file at the correct logical offsets (`pwrite`).
   * Finalizes the file, performs integrity validation, and commits the result atomically.

## Research Goals

* Demonstrate how low-level filesystem metadata (extents, inodes, physical addresses) can be integrated into a distributed copying protocol.
* Evaluate performance trade-offs between logical sequential copy and extent-aware copy.
* Provide a sandbox for studying **address translation**, **sparse file handling**, and **RPC-based storage replication**.

## Potential Extensions

* **Parallel Streaming RPC** for faster transfer of large files.
* **Resume Support**: restart partially completed copies by tracking block ranges.
* **Cross-Filesystem Experiments**: copy blocks between ext4, XFS, and btrfs.
* **Fault Tolerance**: add retries, checkpointing, and failure recovery.


## Running the RPC

```
# client
sudo ./client eternity2 /mnt/nvme/a.txt -b 4096 -n 1000


# server
sudo ./server
```
