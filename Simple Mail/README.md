# SimpleMail – Mini Project 2
## CS39006 Computer Networks Lab

## Files Submitted

| File | Description |
|---|---|
| `smserver.c` | The SimpleMail server |
| `smclient.c` | The SimpleMail client |
| `Makefile` | Builds both executables |
| `users.txt` | Sample user file with 5 users |
| `README.md` | This file |

---

## Design Decisions

### Concurrency
The server uses a single `select()` loop to handle multiple clients at the same time without threads or forking. Each client gets a slot in a fixed-size table (`MAX_CLIENTS = 64`). Every slot has its own line-read buffer so partial TCP reads don't cause data loss between `recv()` calls.

A 1-second timeout on `select()` lets the server periodically check if any client has been stuck without sending a MODE for over 30 seconds, and closes them if so.

### State Machine
Each client has a `State` variable that tracks where it is in the protocol (waiting for MODE, waiting for FROM, reading body lines, authenticating, etc.). Commands that arrive out of sequence are rejected with `ERR Bad sequence`.

### Mailbox File IDs
Each user's mail is stored in `mailboxes/<username>/` as `1.txt`, `2.txt`, and so on. To find the next ID, the server scans the directory for the highest existing number and uses `max + 1`. This means deleted IDs are never reused.

### Authentication (SMP)
The server sends a random 8-character nonce with `AUTH REQUIRED`. The client computes `djb2(password + nonce)` and sends the result as an unsigned decimal integer. The server does the same computation and compares. The raw password is never sent over the network.

### Dot Stuffing
On SMTP2 body send, any line starting with `.` gets an extra `.` prepended (stuffing). The server strips it on storage. On SMP READ, the server re-stuffs lines starting with `.` before sending, and the client strips them on display.

---

## Assumptions Made

Since the assignment did not specify these, the following limits were chosen and are noted here as required:

- Maximum simultaneous clients: **64**
- Maximum users in userfile: **100**
- Maximum recipients per mail: **50**
- On MODE timeout, the server closes the connection silently without sending an error message first.
- Blank lines in `users.txt` are silently skipped.
- The client reconnects fresh for each menu option rather than reusing the same TCP connection.

---

## Sample users.txt

```
alice secretpass1
bob hunter2
charlie x9Kp2mW
diana pass1234
eve qwerty99
```
