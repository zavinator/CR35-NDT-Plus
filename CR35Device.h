#pragma once

#include <qtcpsocket.h>
#include <qlist.h>
#include <qtimer.h>
#include <qdatetime.h>
#include <qimage.h>

#include "Logger.h"

#include <cstdint>


/**
 * @brief Network-backed driver for a CR35 imaging device.
 *
 * The `CR35Device` class manages a TCP connection to a CR35 device and
 * implements the device protocol for requesting tokens, sending commands
 * and reading streaming or single-packet responses. It exposes a small
 * public API to connect/disconnect and to start/stop acquisition.
 */
class CR35Device : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Construct a CR35Device with optional parent QObject.
     * @param parent Optional QObject parent for Qt ownership semantics.
     */
    CR35Device(Logger &logger, QObject* parent = nullptr);
    /**
     * @brief Destructor. Disconnects from device if still connected.
     */
    ~CR35Device();

    /**
     * @brief Device operational states.
     */
    enum State : uint32_t {
		STATE_UNKNOWN   = 0,
		STATE_READY     = 2,
		STATE_SCANNING  = 4,
        STATE_STOPPING  = 5,
		STATE_WAITING   = 6
	};

    /**
     * @brief Get the current device state.
     * @return Current device state as a State enum value.
	 */
	uint32_t getState() const { return m_state; }

    /**
     * @brief Initiate TCP connection to the device.
     *
     * This starts connection establishment to the provided IPv4/IPv6
     * address and TCP port. When the socket emits `connected()`, the
     * device initialization sequence is started automatically.
     *
     * @param ipAddress Device IP address or hostname.
     * @param port TCP port number.
     */
    void connectToDevice(const QString &ipAddress, quint16 port);

signals:
    void connected(); ///< Emitted when the internal socket has connected and the device is ready.
	void disconnected(); ///< Emitted when the internal socket has disconnected.
	void error(const QString& errorString); ///< Emitted when a socket or protocol error occurs.
	void started(); ///< Emitted when acquisition has started.
	void stopped(); ///< Emitted when acquisition has stopped.
	void imageDataReceived(const QImage& image); ///< Emitted when a complete image has been received.

public slots:

    /**
     * @brief Close the TCP connection to the device.
     *
     * This triggers a graceful disconnect where possible. It is safe to
     * call when not connected.
     */
    void disconnectFromDevice();

    /**
     * @brief Start acquisition using the given mode identifier.
     * @param mode Mode numeric identifier (device-specific).
     */
    void start(int mode);

    /**
     * @brief Stop acquisition.
     *
     * The implementation may queue StopRequest/Stop commands as required
     * by the device protocol.
     */
    void stop();

private slots:

	void readData(); ///< Slot connected to QTcpSocket::readyRead to receive incoming bytes.
    void init(); ///< Called once the TCP socket connects to perform initialization.
    void sendCommand(); ///< Send the next queued command or token request to the device.
	void sendImageDataRequest(); ///< Send a request for image data from the device.

private:
    /**
     * @brief Packet data type identifiers used by the protocol.
     */
    enum Type : uint16_t {
		TYPE_UNKNOWN    = 0x0000,
        TYPE_U32        = 0x0002,
        TYPE_STRING     = 0x0007,
        TYPE_U16        = 0x000B,
        TYPE_BLOB       = 0x0008
    };

	/**
	 * @brief Tokens that must be translated into session IDs before use.
	 *
	 * The driver requests a numeric token from the device for each of these
	 * string identifiers during initialization.
	 */
	static constexpr std::initializer_list<const char*> TOKEN_REQUESTS = {
		"Connect",
		"Disconnect",
		"UserId",
        "SystemDate",
        "ImageData",
        "Start",
        "Stop",
        "Mode",
        "PollingOnly",
        "StopRequest",
        "SystemState",
        "DeviceId",
        "Erasor",
        "Version",
		"ModeList"
	};

    /**
     * @brief Data markers used in incoming data streams.
	 */
    enum DataMarker : uint16_t {
        DATA_MARKER_IMAGE_END = 0xFFFBu, ///< End of image: Marks end of image data block
        DATA_MARKER_CONFIG = 0xFFFCu, ///< Config: Next word is size of JSON, then JSON data
		DATA_MARKER_NOP = 0xFFFDu, ///< No-op: Padding word, ignore
        DATA_MARKER_LINE_START = 0xFFFEu, ///< Start of line: Next word is left x padding
		DATA_MARKER_LINE_END = 0xFFFFu  ///< End of line: Next word is right x padding
    };

    /**
     * @brief Packet kinds used when building outgoing packets.
     */
    enum Packet : uint16_t {
		PACKET_UNKNOWN    = 0x0000u,
        PACKET_READ_TOKEN = 0x0003u,
		PACKET_READ_DATA  = 0x0010u,
        PACKET_COMMAND    = 0x0011u,
    };

    /**
     * @brief Representation of a pending command or read request.
     *
     * `packet` selects whether the item is a read (PACKET_READ_DATA) or
     * a typed command (PACKET_COMMAND). `type` defines the payload format
     * when `packet == PACKET_COMMAND`.
     */
    struct Command {
        QByteArray name;
        Packet packet = PACKET_UNKNOWN;
        Type type = TYPE_UNKNOWN;
        QVariant value;

        /**
         * @brief Equality operator for Command structures.
         * @param other Other Command to compare against.
		 * @return true when all fields are equal, false otherwise.
         */
        bool operator==(const Command& other) const 
        {
            return name == other.name && packet == other.packet &&
                   type == other.type && value == other.value;
		}

        Command() { }
		Command(const QByteArray& n, Packet p = PACKET_READ_DATA) : name(n), packet(p) { }
        Command(const QByteArray& n, Type t, const QVariant& v) : name(n), packet(PACKET_COMMAND), type(t), value(v) { }
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
    static constexpr int HEADER_SIZE = sizeof(ServerHeader);

	/**
	 * @brief Parse a server header from raw bytes.
	 * @param data Byte array containing at least HEADER_SIZE bytes.
	 * @return Parsed ServerHeader structure. Fields are zeroed on insufficient data.
	 */
	static ServerHeader parseHeader(const QByteArray& data);

	/**
	 * @brief Parse a ModeList text payload returned by the device.
	 * @param data Raw payload bytes (may contain trailing binary data).
	 * @return Ordered list of human-readable mode names prefixed by their numeric id.
	 */
	static QStringList parseModeList(const QByteArray& data);

	/**
	 * @brief Extract read-data payload from a received buffer using the header.
	 *
	 * This helper interprets fragmentation flags and returns the contiguous
	 * payload for the current logical message when available.
	 *
	 * @param data Full buffer received from the socket.
	 * @param header Header parsed via parseHeader.
	 * @param payload Output parameter that receives the extracted payload when available.
	 * @return true when payload contains a full logical message, false when more data is expected.
	 */
	bool getReadDataPayload(const QByteArray& data, const ServerHeader &header, QByteArray& payload) const;

    /** 
     * @brief Create a token request packet for the given token name.
     * @param token Token name as defined in TOKEN_REQUESTS.
     * @return Byte array containing the serialized request packet.
	 */
    QByteArray createRequestTokenPacket(const QString& token) const;
    /** 
     * @brief Create a command packet for the given command.
     * @param command Command structure defining name, type and value.
	 * @return Byte array containing the serialized command packet.
	 */
	QByteArray createCommandPacket(const Command& command) const;
    /** 
     * @brief Create a read-data packet for the given command.
     * @param command Command structure defining name.
     * @return Byte array containing the serialized read-data packet.
	 */
	QByteArray createReadDataPacket(const Command& command) const;

	/**
	 * @brief Lookup numeric token id for a token name previously requested from the device.
	 * @param token Token name as returned in TOKEN_REQUESTS.
	 * @return Numeric token id or (uint32_t)-1 when not found.
	 */
	uint32_t getTokenId(const QByteArray& token) const;

    /**
     * @brief Enqueue a command or read request to be sent to the device.
     * @param command Command structure defining the request.
	 */
	void enqueueCommand(const Command& command); 

	/**
	 * @brief Parse JSON configuration data from the device.
     * @param jsonData Raw JSON data received from the device.
	 * @return number of pixels per line (extracted from JSON) or -1 on error.
	 */
	int parseJsonConfig(const QByteArray& jsonData) const;

	void processImageData(); ///< Process assembled image data packet when complete.
	


    static constexpr int IMAGE_DATA_REQUEST_INTERVAL_MS = 300; ///< Interval between image data requests.
	static constexpr int TIMEOUT_MS = 2000; ///< Command response timeout in milliseconds.
	static constexpr int COMMAND_QUEUE_INTERVAL_MS = 10; ///< Interval between sending queued commands.

	QTcpSocket m_socket; ///< Internal TCP socket for device communication.
	QByteArray m_buffer; ///< Buffer for incoming data assembly.
	QByteArray m_imageData; ///< Buffer for assembling image data packets.

	QByteArray m_clientId; ///< Random client identifier.
	QHash<QString, int> m_tokens; ///< Map of token names to numeric session IDs.

	Command m_currentCommand; ///< Currently pending command being sent.
	QList<Command> m_commands; ///< Queue of pending commands to send.

	QTimer m_dataTimer; ///< Timer for periodic data requests.
	uint32_t m_state = STATE_UNKNOWN; ///< Current device operational state.
	bool m_started = false; ///< Whether acquisition has been started.
	bool m_wasScanning = false; ///< Whether the device was previously in scanning state.

	QTimer m_commandQueueTimer; ///< Timer to trigger sending queued commands.
	QDateTime m_lastCommandTime; ///< Timestamp of the last sent command.

	Logger& m_logger; ///< Logger instance for logging messages.
};
