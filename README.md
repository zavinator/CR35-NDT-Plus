# Duerr CR-35 NDT Plus Protocol

This project implements a driver for the CR35 imaging device. The protocol implementation is based on **reverse engineering** efforts using Wireshark packet captures and static analysis with Ghidra.

> **Disclaimer**: This is not an official driver. Protocol details are inferred from observation and testing. While it successfully connects and acquires images, some edge cases or advanced features may be missing or simplified.
> 
> **Note**: The code is meant as research not a production code and you can use it at you own risk. There will be also no maintenance of this code.
>
> **AI Usage**: Protocol analysis and description were assisted by AI tools. Please verify details independently.

## Protocol Overview

The communication occurs over a standard **TCP/IP** connection.

- **Byte Order**: Network Byte Order (**Big-Endian**) is used for all Protocol Headers and Command Packets. However, the Image Data payload itself is **Little-Endian**.
- **Packet Structure**: Every packet (Command or Response) starts with a 14-byte header.

### Response Header (RX) (14 Bytes)

| Offset | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0 | `Flags` | 1 | `0x01` = Fragmented (more to follow), `0x00` = Last/End. |
| 1 | `Type` | 1 | `0x11` = Data Payload, `0x00` = Footer/Control. |
| 2 | `Block` | 2 | Sequence counter (starts at 0). |
| 4 | `Token` | 4 | Token Identifier. |
| 8 | `Size` | 4 | Payload size (bytes) following this header. |
| 12 | `Mode` | 2 | `0x0008` = Fragmented Stream, `0x0007` = Single Packet. |

### Outbound Packet Structures (TX) (14 Bytes)

The device protocol uses different header structures for sending commands requests versus receiving data. All outbound headers are 14 bytes.

**1. Command Packet (0x0011)**

Used for sending configuration and trigger commands.

| Offset | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0 | `Cmd` | 2 | `0x0011` (PACKET_COMMAND) |
| 2 | `Flags` | 2 | Usually 0. |
| 4 | `Token` | 4 | Token Identifier. |
| 8 | `Length` | 4 | Length of following payload. |
| 12 | `Type` | 2 | Payload Type ID (e.g. TYPE_U32). |

**2. Token Request Packet (0x0003)**

Used during initialization to resolve string names to token IDs.

| Offset | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0 | `Cmd` | 2 | `0x0003` (PACKET_READ_TOKEN) |
| 2 | `Reserved` | 2 | 0 |
| 4 | `Length` | 2 | Length of token string payload. |
| 6 | `Reserved` | 2 | 0 |
| 8 | `ClientId` | 6 | Random Client ID bytes. |

**3. Read Data Packet (0x0010)**

Used to poll for status or image data.

| Offset | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0 | `Cmd` | 2 | `0x0010` (PACKET_READ_DATA) |
| 2 | `Reserved` | 2 | 0 |
| 4 | `Token` | 4 | Token Identifier. |
| 8 | `ClientId` | 6 | Random Client ID bytes. |

### Data Fragmentation

Large payloads (specifically `ImageData`) are not sent as a single continuous stream. Instead, the device splits them into **64KB (0x10000) blocks**.

1.  **Block Structure**: Each block begins with a standard 14-byte **Response Header**, followed by up to 65,522 bytes of payload data.
2.  **Header Injection**: The protocol injects these headers precisely every 65,536 bytes.
3.  **Fields behavior in blocks**:
    -   `Flags`: `0x01` for all intermediate blocks, `0x00` for the final block.
    -   `Block`: Incremental counter (`0x0000`, `0x0001`, ...).
    -   `Size`: Decreasing value representing total bytes remaining.
4.  **Driver Handling**:
    -   The `getReadDataPayload` function detects fragmentation mode (`0x0008`).
    -   It iterates through the raw buffer, extracting payload chunks.
    -   It skips the 14-byte headers that appear at 64KB boundaries.
    -   **Simplification**: We simply **skip** these intermediate headers without validating the block counter or size fields.
    -   **Result**: A seamless, contiguous byte array for the image processor.

## Command System (Token-Based)

The protocol uses a dynamic token system. The client does not use hardcoded command IDs for everything; instead, it requests numeric tokens for specific command strings during initialization.

1.  **Request Token**: Client sends a `PACKET_READ_TOKEN` (0x03) request with a string (e.g., "Start").
2.  **Receive Token**: Server responds with a 4-byte numeric ID (e.g., `0x00001001`).
3.  **Use Token**: Client uses this ID in the header of subsequent commands.

**Known Tokens**:
`Connect`, `Disconnect`, `UserId`, `SystemDate`, `ImageData`, `Start`, `Stop`, `Mode`, `PollingOnly`, `StopRequest`, `SystemState`, `DeviceId`, `Erasor`, `Version`, `ModeList`.

### Payload types

Commands carry typed payloads:

- `TYPE_U32` (0x02)
- `TYPE_STRING` (0x07)
- `TYPE_BLOB` (0x08)
- `TYPE_U16` (0x0B)

## Driver Workflows

### 1. Initialization

Upon connection, the driver performs the following handshake:
1.  **Token Discovery**: Requests IDs for all known command strings.
2.  **Login**:
    -   `Connect`: 1
    -   `UserId`: "user@BACKUP"
    -   `SystemDate`: Current date string.
3.  **State Check**: Requests `ModeList` and `SystemState`.

### 2. Acquisition

To start scanning:

1.  **Select Mode**: Sends `Mode` command with the integer mode ID.
2.  **Config**: Sends `PollingOnly` = 1.
3.  **Trigger**: Sends `Start` = 1.
4.  **Polling Loop**: The driver periodically requests `SystemState` and `ImageData` until the scan is complete.

## Image Data Format

The `ImageData` payload differs from the command protocol. It behaves as a continuous stream of **16-bit Little-Endian** words.

### Stream Structure

The stream contains pixel data interleaved with special **Control Markers** (values `0xFFF9` - `0xFFFF`).

- **Pixels**: Standard 16-bit grayscale values.
- **Markers**:
    -   `0xFFFE` (**Line Start**): Followed by `Left Padding` (X-offset).
    -   `0xFFFF` (**Gap / Skip**): Followed by a 16-bit value indicating how many pixels to skip (advance X) inside the current line.
    -   `0xFFFC` (**Config**): Followed by size `N`, then `N` bytes of **JSON** metadata (e.g., Model Name, BitsStored).
    -   `0xFFFB` (**Image End**): Marks the end of the frame.
    -   `0xFFFD` (**NOP**): Padding/Keep-alive (?)

The driver parses this stream line-by-line, stripping the control words, to reconstruct the final image.

## Simplifications & Notes

-   **Client ID**: We generate a random 6-byte Client ID string on connect. The exact format requirement is unknown, but random works.
-   **Padding**: The driver calculates the bounding box of valid pixels (ignoring left/right padding) to produce the smallest valid image rectangle.
-   **Polling**: We strictly use polling (`ImageData` requests). An interrupt/push mechanism may exist but is not implemented.
-   **JSON**: The embedded JSON configuration often provides crucial details like `PixLine` (width) and `BitsStored`.
