# File Transfer Protocol 1

One TCP connection carries one file. All integers are unsigned and in network
byte order (big-endian). TCP write/read boundaries have no protocol significance.
No C++ structs are written directly to the wire.

## Successful exchange

| Step | Direction | Bytes |
| --- | --- | --- |
| Header | Sender → receiver | Eight ASCII bytes `FTAPP001`, then an unsigned 64-bit payload length. Total: 16 bytes. |
| Ready | Receiver → sender | Eight-byte status frame with code `0`. |
| Payload | Sender → receiver | Exactly the announced number of bytes; zero bytes is valid. |
| Checksum | Sender → receiver | Unsigned 32-bit CRC32 of the payload. |
| End | Sender → receiver | TCP write-half shutdown (FIN); the sender continues reading. |
| Complete | Receiver → sender | Eight-byte status frame with code `1`, after verification and file publication. |

A status frame is the four ASCII bytes `FTAK` followed by an unsigned 32-bit code:

- `0`: ready to receive the file.
- `1`: verified and saved.
- `2`: rejected or failed; consult the receiver's local error output.

The sender must wait for `0` before sending payload and for `1` before reporting
success. Unknown status codes, a status in the wrong phase, or invalid magic are
errors. No names, paths, timestamps, or file permissions are sent.

CRC is CRC-32/ISO-HDLC (reflected polynomial `0xEDB88320`, initial state
`0xFFFFFFFF`, final XOR `0xFFFFFFFF`). It is compatible with `zlib.crc32`.
The empty payload has CRC `0x00000000`; ASCII `123456789` has CRC `0xCBF43926`.
Checksum calculation and disk writing are streamed, with no whole-file buffer.

## Failure behavior

The receiver rejects unsupported magic/version, an advertised size above its
configured maximum, early EOF, a missing/incorrect checksum, or extra data after
the checksum. It also requires FIN; a sender that never finishes is subject to
the inactivity timeout. Disk errors prevent a success response.

The receiver creates a temporary output in the destination directory and removes
it on ordinary failure. Only a fully verified file is published. A receiver error
after accepting a connection triggers a best-effort status `2`; a broken socket,
cancellation, or error before connection acceptance may prevent that response.
Both programs exit after one connection, including malformed/failed transfers.

The success acknowledgement comes after publication. Consequently a lost
acknowledgement does **not** imply the file was not saved. There is no transfer ID,
retry negotiation, or distributed commit protocol. The sender reports failure if
it cannot confirm success, and the user must check the receiver before retrying.

CRC32 detects accidental corruption; it provides no authenticity. Protocol 1 has
no encryption or peer authentication and is intended for trusted networks. It is
incompatible with the original unframed implementation.

## Platform implementation references

- [Winsock nonblocking connection completion and select](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select)
- [POSIX link: creation without replacing an existing name](https://man7.org/linux/man-pages/man2/link.2.html)
- [Windows MoveFileExW flags and replacement behavior](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
