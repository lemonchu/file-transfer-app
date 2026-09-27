# File Transfer Application

A small command-line tool for sending one file directly between Windows, macOS,
and Linux computers. Written in C++17, with no third-party runtime dependencies.

`sender` connects to `recver`, which verifies the file and saves it to the path
you choose. Both programs exit after one transfer.

## Build

Requirements: CMake 3.16+ and a C++17 compiler (GCC 9+, modern Clang, or Visual
Studio 2019+). Python 3.8+ is needed only for tests. Windows builds use the system
Winsock and IP Helper libraries.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel
```

On Linux/macOS the executables are `build/sender` and `build/recver`. With Visual
Studio they are `build/Release/sender.exe` and `build/Release/recver.exe`. MinGW
is also supported; use the `MinGW Makefiles` generator if needed.

Optional installation:

```sh
cmake --install build --config Release --prefix ./package
```

The executables will be under `package/bin/`.

## Quick start

On the **receiving computer**, list its addresses and start the receiver:

```sh
./build/recver --list-ips
./build/recver 9000 received.zip
```

On the **sending computer**, use the receiver's reachable IP address:

```sh
./build/sender 192.168.1.10 9000 original.zip
```

Use the corresponding `.exe` paths on Windows. Quote paths containing spaces.
The receiver's firewall must allow the selected TCP port. Use a LAN address for
another computer; `127.0.0.1` and `::1` refer to the current computer. There is no
automatic device discovery, relay, or NAT traversal.

Both sides show bytes transferred, percentage, and average MiB/s. The sender
reports success only after the receiver verifies and saves the file. Transfer
progress reaching 100% alone does not confirm that the file has been saved.

## Commands and options

```text
sender <host> <port> <file> [options]
recver <port> <output> [options]
```

The original command names and positional arguments are preserved. `host` may
be an IPv4 address, IPv6 address (without URL brackets), or hostname. Ports must
be integers from 1 to 65535.

| Option | Available on | Meaning |
| --- | --- | --- |
| `--timeout <seconds>` | Both | Network inactivity limit, 1–86400 seconds; default **30**. Also limits waiting for an incoming connection and connecting to a peer. |
| `--quiet` | Both | Suppress progress, listening, and success messages; errors remain visible. |
| `--help`, `-h` | Both | Show usage when used alone. |
| `--version` | Both | Show versions when used alone. |
| `--` | Both | Treat remaining arguments as positional, including names starting with `-`. |
| `--bind <address>` | Receiver | Listen address; defaults to `0.0.0.0` (all IPv4 interfaces). |
| `--force` | Receiver | Replace an existing regular file **only after** successful verification. |
| `--max-size <bytes>` | Receiver | Reject files above this size; default is no limit. `0` accepts empty files only. |
| `--list-ips` | Receiver | List local addresses when used alone. No arguments also prints usage and addresses. |

Examples:

```sh
# Allow five minutes to start sending; allow at most 1 GiB.
./build/recver 9000 archive.zip --timeout 300 --max-size 1073741824

# Replace an existing destination after verification.
./build/recver 9000 archive.zip --force

# Restrict reception to this computer.
./build/recver 9000 output.bin --bind 127.0.0.1

# IPv6 (availability and dual-stack behavior depend on the OS).
./build/recver 9000 output.bin --bind ::
./build/sender ::1 9000 input.bin

# A source name beginning with a dash.
./build/sender --quiet -- 192.168.1.10 9000 -input.bin
```

Exit codes: `0` means success, `1` means a transfer/system error, and `2` means
invalid arguments. Help, version, and address listing return `0`.

## Reliability and scope

- A versioned header carries the 64-bit file size. Exact-length I/O handles
  partial sends, fragmented reads, interruptions, and network failures.
- A streaming CRC32 checksum detects accidental corruption without reading the
  source twice. Memory use stays bounded using 64 KiB transfer buffers.
- Reception writes to an exclusively created `.file-transfer-*.part` file in
  the destination directory. The destination appears only after the expected
  length, checksum, and end of stream are checked and file data flushed.
- Existing destinations are refused by default, even if another process creates
  one during reception. `--force` permits replacement; failed transfers leave
  the original intact. Directories and existing symlinks are not replacement targets.
- Ordinary failures and Ctrl+C clean up temporary files. A forced process kill
  or machine crash can leave a `.part` file; remove it after confirming the
  corresponding receiver is no longer running.
- On Unix, publishing without `--force` uses a hard link to prevent overwrite
  races, so the destination filesystem must support hard links. Windows uses
  `MoveFileExW`. Received files use fresh permissions/metadata; source permissions,
  timestamps, and filenames are not transmitted.

This version is intended for **trusted networks**. It has no encryption or
peer authentication; CRC32 is not a security check. The first client to connect
occupies the receiver. There is no resume, directory transfer, compression, or
concurrent reception. Do not modify the source during transmission: shrinking
or growing files are detected, but in-place edits are not reliably detectable.

Timeouts measure network inactivity, not total transfer duration. DNS lookup
and local filesystem operations use OS behavior and are outside that timeout.
If the connection fails after the receiver saves the file but before its final
acknowledgement arrives, the sender reports unconfirmed delivery; inspect the
destination before retrying.

**Compatibility:** both endpoints must use this version. The original 2023
programs sent an unframed byte stream and cannot interoperate with protocol 1.
See [the wire protocol](docs/protocol.md).

## Test and develop

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
cd build
ctest -C Release --output-on-failure
```

The Python standard-library suite launches real processes on loopback sockets.
It covers file-size boundaries, multi-megabyte data, Unicode paths, IPv6,
independent Python protocol peers, fragmented messages, checksum failures,
truncation, timeouts, cancellation, acknowledgements, and destination protection.
IPv6 is skipped when unavailable; signal and symlink tests run on Unix. Linux
also runs test-only binaries that force short socket reads/writes, `EINTR`, and
`EAGAIN` to verify retry behavior deterministically.

GitHub Actions builds, tests, and packages executables separately for Windows,
macOS, and Linux. Runs upload one archive per platform. Binaries are not committed.

```text
src/net.*          Socket lifetime, nonblocking I/O, deadlines, address lookup
src/transfer.*     CLI, protocol, CRC32, progress, safe file publication
src/sender.cpp     Sender entry point
src/recver.cpp     Receiver entry point
tests/             Black-box integration tests
docs/protocol.md   Wire format and failure semantics
```

## License

[MIT](LICENSE).
