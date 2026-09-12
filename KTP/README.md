# KTP — KGP Transport Protocol

A reliable transport protocol implemented over UDP as part of **CS39006: Networks Laboratory**.

The project provides a custom socket-like API on top of UDP with **sliding-window flow control, cumulative acknowledgements, retransmissions, receiver buffering, packet-loss simulation, shared-memory IPC, and concurrent sender/receiver threads**.

## Project Overview

KTP provides reliable message delivery while using UDP as the underlying network protocol.

The implementation consists of:

- A KTP socket library (`ksocket.c` / `ksocket.h`)
- A background protocol process (`initksocket`)
- A file-sending application (`user1`)
- A receiving application (`user2`)
- Shared memory and semaphores for communication between applications and the protocol process

### Main Features

- Reliable data transfer over UDP
- Sliding-window flow control
- Cumulative ACKs
- Duplicate ACKs (DUPACK)
- Timeout-based retransmission
- Whole-window retransmission on timeout
- Receiver-side buffering and out-of-order packet tracking
- Configurable packet-loss simulation
- Shared-memory based socket management
- Semaphore-based synchronization
- Automatic garbage collection of abandoned sockets
- Concurrent sender and receiver threads

## Architecture

```text
                    ┌──────────────────┐
                    │      user1       │
                    │   File Sender    │
                    └────────┬─────────┘
                             │
                       k_sendto()
                             │
                    ┌────────▼─────────┐
                    │   Shared Memory  │
                    │    send_buf[]    │
                    └────────┬─────────┘
                             │
                       Sender Thread
                             │
                         UDP send
                             │
                    ╔════════▼════════╗
                    ║      UDP        ║
                    ╚════════╤════════╝
                             │
                         UDP recv
                             │
                     Receiver Thread
                             │
                    ┌────────▼─────────┐
                    │   Shared Memory  │
                    │    recv_buf[]    │
                    └────────┬─────────┘
                             │
                       k_recvfrom()
                             │
                    ┌────────▼─────────┐
                    │      user2       │
                    │  File Receiver   │
                    └──────────────────┘
```

`initksocket` manages the underlying UDP sockets and runs the protocol's sender/receiver logic, while `user1` and `user2` interact with the system through the KTP socket API.

## Components

### `ksocket.c`

Implements the KTP socket API:

- `k_socket()` — creates and registers a KTP socket
- `k_bind()` — associates source and destination addresses
- `k_sendto()` — queues data for transmission
- `k_recvfrom()` — retrieves received data in order
- `k_close()` — closes the KTP socket
- Shared-memory and semaphore utilities
- Packet-loss simulation through `dropMessage()`

### `initksocket.c`

Runs the protocol engine and handles:

- Socket creation and binding
- Sender and receiver threads
- Sliding-window management
- ACK/DUPACK processing
- Timeout detection
- Retransmissions
- Receiver buffering
- Socket cleanup
- Garbage collection

### `user1.c`

The sender application:

1. Creates and binds a KTP socket.
2. Reads a file in 512-byte chunks.
3. Queues each chunk using `k_sendto()`.
4. Waits until the messages are acknowledged.
5. Allows multiple files to be sent.

### `user2.c`

The receiver application:

1. Creates and binds a KTP socket.
2. Continuously receives messages using `k_recvfrom()`.
3. Displays received data.
4. Writes received data to `received_output.txt`.

## Reliability Mechanism

KTP provides reliability despite UDP being unreliable.

### Sequence Numbers

Every data message is assigned a sequence number. The receiver uses sequence numbers to detect duplicates, identify out-of-order packets, and deliver messages in order.

### Sliding Window

The sender can have multiple unacknowledged packets in flight instead of waiting for an ACK after every packet. The sender window is controlled using the receiver's advertised window (`rwnd`).

### Cumulative ACKs

ACKs indicate the latest sequence number received consecutively in order, allowing multiple successfully received packets to be acknowledged efficiently.

### Retransmission

If outstanding packets remain unacknowledged for the configured timeout interval, the sender retransmits the outstanding window.

### Duplicate ACKs

DUPACKs are used to communicate receiver-side conditions such as the receiver buffer becoming available.

### Packet-Loss Simulation

The implementation includes `dropMessage(p)`, which randomly drops packets with probability `p`. This allows the reliability and retransmission mechanisms to be tested under simulated packet loss.

## Shared Memory and Synchronization

The user processes communicate with `initksocket` through shared memory rather than directly operating on UDP sockets.

Shared memory contains:

- KTP socket entries
- Sender and receiver buffers
- Sender and receiver window state
- Socket state information

System V semaphores provide synchronization and mutual exclusion when accessing shared socket state.

## Build

### Requirements

- Linux/Unix environment
- GCC
- POSIX threads
- System V shared memory
- System V semaphores

Build the project using:

```bash
make
```

To remove generated files:

```bash
make clean
```

## Running the Project

Start the protocol process:

```bash
./initksocket
```

Start the receiver:

```bash
./user2 <src_ip> <src_port> <dst_ip> <dst_port>
```

Start the sender in another terminal:

```bash
./user1 <src_ip> <src_port> <dst_ip> <dst_port>
```

The sender will prompt for the filename to transfer. The receiver saves received data to:

```text
received_output.txt
```

Enter `EXIT` in the user applications to terminate them.

## File Structure

```text
KTP/
├── documentation.txt
├── ksocket.c
├── ksocket.h
├── initksocket.c
├── user1.c
├── user2.c
└── Makefile
```

## Notes

- Data is transferred in **512-byte** messages.
- The receiver can buffer up to **10 messages**.
- Sequence numbers use an 8-bit sequence-number space and skip sequence number `0`.
- `initksocket` performs the actual UDP socket operations.
- `user1` and `user2` interact with the system through the custom KTP API.

For detailed implementation, protocol design, testing methodology, and experimental results, refer to **`documentation.txt`**.
