#pragma once

#include <vector>
#include <cstdint>

#include <qbytearray.h>
#include <qendian.h>

constexpr int IMAGE_DATA_REQUEST_INTERVAL_MS = 300; ///< Interval between image data requests.
constexpr int TIMEOUT_MS = 2000; ///< Command response timeout in milliseconds.
constexpr int COMMAND_QUEUE_INTERVAL_MS = 10; ///< Interval between sending queued commands.

constexpr size_t UINT16_SIZE = sizeof(uint16_t);

/**
 * @brief Packet data type identifiers used by the protocol.
 */
enum DataType : uint16_t {
	TYPE_UNKNOWN = 0x0000,
	TYPE_U32 = 0x0002,
	TYPE_STRING = 0x0007,
	TYPE_U16 = 0x000B,
	TYPE_BLOB = 0x0008
};

/**
 * @brief Data markers used in incoming data streams.
 */
enum DataMarker : uint16_t {
	DATA_MARKER_IMAGE_END = 0xFFFBu, ///< End of image: Marks end of image data block
	DATA_MARKER_CONFIG = 0xFFFCu, ///< Config: Next word is size of JSON, then JSON data
	DATA_MARKER_NOP = 0xFFFDu, ///< No-op: Padding word, ignore
	DATA_MARKER_START = 0xFFFEu, ///< Start of line: Next word is left x padding
	DATA_MARKER_GAP = 0xFFFFu  ///< Data gap: Next word is number of missing pixels
};

/**
 * @brief Packet kinds used when building outgoing packets.
 */
enum Packet : uint16_t {
	PACKET_UNKNOWN = 0x0000u,		///< Unknown packet type
	PACKET_READ_TOKEN = 0x0003u,	///< Read token: Requests a token from the device
	PACKET_READ_DATA = 0x0010u,		///< Read data: Requests data from the device
	PACKET_COMMAND = 0x0011u,		///< Command: Sends a command to the device
};

// 14 bytes header
#pragma pack(push, 1)
struct ServerHeader {
	uint8_t flags;       ///< 0x01 = More fragments follow, 0x00 = Last fragment or End
	uint8_t packetType;  ///< 0x11 = Data payload, 0x00 = Footer/Control packet
	uint16_t block;      ///< Sequence counter, starts at 0 (Big Endian)
	uint32_t token;      ///< Session ID / Stream identifier (Big Endian)
	uint32_t size;       ///< Total bytes remaining to be read including this block (Big Endian)
	uint16_t mode;       ///< 0x0008 = Fragmented Stream, 0x0007 = Single Packet (Big Endian)
};
#pragma pack(pop)
static constexpr int HEADER_SIZE = sizeof(ServerHeader); ///< Size of the server packet header in bytes

/**
 * @brief Structures for assembling image pixel data from the device.
 */
struct PixelSegment {
	int xStart = 0; ///< Starting X coordinate of the segment
	const uint16_t* pixelDataPtr = nullptr; ///< Pointer to the pixel data
	int pixelCount = 0; ///< Number of pixels in the segment
};
/**
 * @brief A single scan line composed of multiple pixel segments.
 */
struct ScanLine {
	std::vector<PixelSegment> segments; ///< List of pixel segments in the scan line
	int endX = 0; ///< Logical line end position (includes gaps), measured in pixels from x=0
};

/**
 * @brief Helper structure for assembling lines and segments from incoming data.
 */
struct LineAssembler {
	std::vector<ScanLine> image; ///< Assembled image composed of scan lines
	ScanLine currentLine; ///< Current scan line being assembled
	PixelSegment currentSeg; ///< Current pixel segment being assembled
	bool inLine = false; ///< Whether currently inside a scan line
	uint16_t x = 0; ///< Current x position within the open scan line (includes gaps)

	/** 
	 * @brief Flush the current pixel segment to the current line.
	 */
	void flushSegment()
	{
		if (currentSeg.pixelDataPtr && currentSeg.pixelCount > 0)
			currentLine.segments.push_back(currentSeg);
		currentSeg = {};
	}

	/** 
	 * @brief Flush the current scan line to the image.
	 */
	void flushLine()
	{
		if (!inLine)
			return;
		flushSegment();
		currentLine.endX = static_cast<int>(x);
		if (!currentLine.segments.empty())
			image.push_back(currentLine);
		currentLine = {};
		inLine = false;
		x = 0;
	}
};

/**
 * @brief Append a 16-bit big-endian value to a QByteArray.
 * @param out QByteArray to append to.
 * @param v 16-bit unsigned integer value to append.
 */
inline void appendBE16(QByteArray& out, quint16 v)
{
	const quint16 be = qToBigEndian<quint16>(v);
	out.append(reinterpret_cast<const char*>(&be), sizeof(be));
}

/**
 * @brief Append a 32-bit big-endian value to a QByteArray.
 * @param out QByteArray to append to.
 * @param v 32-bit unsigned integer value to append.
 */
inline void appendBE32(QByteArray& out, quint32 v)
{
	const quint32 be = qToBigEndian<quint32>(v);
	out.append(reinterpret_cast<const char*>(&be), sizeof(be));
}
