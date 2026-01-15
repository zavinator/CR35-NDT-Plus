#include "CR35Device.h"

#include <qrandom.h>
#include <qeventloop.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <algorithm>
#include <limits>
#include <vector>


CR35Device::CR35Device(Logger &logger, QObject* parent) : QObject(parent), 
    m_logger(logger)
{
	connect(&m_socket, &QTcpSocket::connected, this, &CR35Device::init);
	connect(&m_socket, &QTcpSocket::connected, this, &CR35Device::connected);
	connect(&m_socket, &QTcpSocket::disconnected, this, &CR35Device::disconnected);
	connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError socketError) {
		emit error(m_socket.errorString());
	});
	connect(&m_socket, &QTcpSocket::readyRead, this, &CR35Device::readData);

    m_dataTimer.setSingleShot(true);
	m_dataTimer.setInterval(IMAGE_DATA_REQUEST_INTERVAL_MS);
	connect(&m_dataTimer, &QTimer::timeout, this, &CR35Device::sendImageDataRequest);

	m_commandQueueTimer.setInterval(COMMAND_QUEUE_INTERVAL_MS);
	connect(&m_commandQueueTimer, &QTimer::timeout, this, &CR35Device::sendCommand);
}

CR35Device::~CR35Device()
{
	disconnectFromDevice();
}

void CR35Device::connectToDevice(const QString& ipAddress, quint16 port)
{ 
    m_currentCommand = {};
    m_commands.clear();
    m_buffer.clear();
    m_state = STATE_UNKNOWN;
	m_started = false;

	m_clientId.clear();
    for (int i = 0; i < 6; ++i)
        m_clientId.append(static_cast<char>(QRandomGenerator::global()->bounded(256)));

	m_logger.message("Connecting to device at " + ipAddress + ":" + QString::number(port));	
	m_socket.connectToHost(ipAddress, port);
}

void CR35Device::disconnectFromDevice()
{
    if (m_socket.state() == QAbstractSocket::UnconnectedState)
    {
        m_commandQueueTimer.stop();
        return;
    }

    // Stop periodic requests first so the command queue cannot be refilled during shutdown.
    const bool wasStarted = m_started;
    stop();

    // If we were running, wait until device confirms stop (or timeout).
    if (wasStarted)
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);

        connect(this, &CR35Device::stopped, &loop, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

        timeout.start(TIMEOUT_MS);
        loop.exec();
    }

    m_logger.message("Command queue size: " + QString::number(m_commands.size()));

    m_commandQueueTimer.stop();
    if (m_socket.state() == QAbstractSocket::ConnectedState)
    {
		m_logger.message("Disconnecting from device");
        m_socket.disconnectFromHost();
        if (m_socket.state() != QAbstractSocket::UnconnectedState)
        {
            if (!m_socket.waitForDisconnected(TIMEOUT_MS))
                m_socket.abort();
        }
    }
}

uint32_t CR35Device::getTokenId(const QByteArray& token) const
{
	return static_cast<uint32_t>(m_tokens.value(token, -1));
}

void CR35Device::readData()
{
	m_buffer.append(m_socket.readAll());
	if (m_buffer.size() < HEADER_SIZE)
        return;

    auto header = parseHeader(m_buffer);

	// process token response
	if (m_currentCommand.packet == PACKET_READ_TOKEN)
    {
		m_tokens[m_currentCommand.name] = header.token;
    }
	else // process response to command
    {
		QByteArray payload;
        if (!getReadDataPayload(m_buffer, header, payload))
			return; // wait for more data
        
        if (header.token == getTokenId("ModeList"))
        {
            m_modeList = parseModeList(payload);
			m_logger.message("Received ModeList with " + QString::number(m_modeList.size()) + " modes");
            m_logger.message("ModeList modes: " + m_modeList.join(", "));
        }
        else if (header.token == getTokenId("ImageData"))
        {
			m_logger.message("Received ImageData of size: " + QString::number(payload.size()));
			m_imageData.append(payload);
			if (payload.size() > 32) // only for large packets
			    emit newDataReceived();

			if (m_state == STATE_WAITING && m_wasScanning && m_imageData.size() >= sizeof(uint16_t))
			{
				const uint16_t lastWord = qFromLittleEndian<uint16_t>(m_imageData.constData() + m_imageData.size() - sizeof(uint16_t));
				if (lastWord == DATA_MARKER_IMAGE_END)
				{
					processImageData();
					m_wasScanning = false;
					m_imageData.clear();
				}
			}
			
			if (m_started) m_dataTimer.start(); // enqueue next packet
		}
        else if (header.token == getTokenId("SystemState"))
        {
            if (payload.size() == sizeof(uint32_t))
            {
                m_state = qFromBigEndian<uint32_t>(payload.constData());
				m_logger.message("SystemState: " + QString::number(m_state));
                if (m_state == STATE_SCANNING)
                {
                    m_wasScanning = true;
                }
                else if (m_state == STATE_STOPPING && m_wasScanning)
                {
                    processImageData();
                    m_wasScanning = false;
                    m_imageData.clear();
                }
            }
		}
        else if (header.token == getTokenId("Start"))
        {
			m_logger.message("Acquisition started");
			m_started = true;
			emit started();
            m_dataTimer.start();
        }
        else if (header.token == getTokenId("Stop"))
        {
			m_logger.message("Acquisition stopped");
            m_started = false;
			emit stopped();
            enqueueCommand(Command("SystemState"));
        }
    }

	m_logger.message("Received packet: Flags=" + QString::number(header.flags) +
                     " Type=" + QString::number(header.packetType) +
                     " Block=" + QString::number(header.block) +
                     " Token=" + QString::number(header.token) +
                     " Size=" + QString::number(header.size) +
                     " Mode=" + QString::number(header.mode));

	// command processed
	m_currentCommand = {};
    m_buffer.clear();
}

ServerHeader CR35Device::parseHeader(const QByteArray& data)
{
    // Parses the server-side RX packet header.
    // Structure (big-endian): [Cmd:2] [Flags:2] [Token:4] [Len:4] [Type:2]
    // Offsets: cmd=0, flags=2, token=4, len=8, type=12. Total = 14 bytes.
    ServerHeader header;
    if (data.size() < HEADER_SIZE)
        return header; // return zeroed header on insufficient data

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.constData());

    header.flags = ptr[0];
	header.packetType = ptr[1];
    header.block   = qFromBigEndian<uint16_t>(ptr + 2);
    header.token   = qFromBigEndian<uint32_t>(ptr + 4);
    header.size    = qFromBigEndian<uint32_t>(ptr + 8);
    header.mode  = qFromBigEndian<uint16_t>(ptr + 12);

    return header;
}

QStringList CR35Device::parseModeList(const QByteArray& data)
{
    // ModeList is INI-like text with sections [Mode-{...}] and key/value pairs.
    // We extract the preferred display name per section (ModeName_en, then ModeName, then ModeName_de)
    // and prefix it with the mode id from the section header.

    QString text = QString::fromLatin1(data);

    // Trim at first NUL (device may append binary / padding after textual config)
    const int nulPos = text.indexOf(QChar(u'\0'));
    if (nulPos >= 0)
        text.truncate(nulPos);

    // Normalize newlines
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');

    QStringList result;

    QString currentSectionId;
    QString currentEn;
    QString current;
    bool inModeSection = false;

    auto flushSection = [&]() {
        if (!inModeSection)
            return;

        QString name = currentEn;
        if (name.isEmpty())
            name = current;

        name = name.trimmed();
        if (!name.isEmpty())
        {
            const QString id = currentSectionId.isEmpty() ? QString() : (currentSectionId + " - ");
            result.push_back(id + name);
        }

        currentSectionId.clear();
        currentEn.clear();
        current.clear();
    };

    const QStringList lines = text.split('\n');
    for (QString line : lines)
    {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        // INI comment lines
        if (line.startsWith(';'))
            continue;

        // If XML-ish payload appears, stop parsing (example shows "<!--<paramDescription")
        if (line.startsWith("<!--"))
            break;

        if (line.startsWith('[') && line.endsWith(']'))
        {
            flushSection();

            inModeSection = line.startsWith("[Mode-");
            if (inModeSection)
            {
                // Example: [Mode-{00000001}]
                const int l = line.indexOf("{");
                const int r = line.indexOf("}");
                if (l >= 0 && r > l)
                    currentSectionId = line.sliced(l + 1, r - l - 1).trimmed();
            }
            continue;
        }

        if (!inModeSection)
            continue;

        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;

        const QString key = line.left(eq).trimmed();
        const QString value = line.sliced(eq + 1).trimmed();

        if (key.compare("ModeName_en", Qt::CaseInsensitive) == 0)
            currentEn = value;
        else if (key.compare("ModeName", Qt::CaseInsensitive) == 0)
            current = value;
    }

    flushSection();

    // De-dup while preserving order
    QStringList unique;
    QSet<QString> seen;
    for (const auto& n : result)
    {
        const QString k = n.trimmed();
        if (k.isEmpty() || seen.contains(k))
            continue;
        seen.insert(k);
        unique.push_back(k);
    }

    return unique;
}

bool CR35Device::getReadDataPayload(const QByteArray& data, const ServerHeader& header, QByteArray& payload) const
{
    if (data.size() < HEADER_SIZE + header.size)
		return false; // wait for more data

	auto footerHeader = parseHeader(data.sliced(data.size() - HEADER_SIZE));
    if(footerHeader.flags != 0 || 
        footerHeader.packetType != 0 || 
        footerHeader.block != 0 || 
        footerHeader.token != header.token)
    {
		return false; // invalid footer - need more data
	}

    if (header.mode == 0x08)
    {
		payload.reserve(header.size);

        // A block is 64KB total. 14B is header, so payload is 65522.
        constexpr int MAX_CHUNK_SIZE = 0x10000 - HEADER_SIZE;

        int offset = HEADER_SIZE;
        const int endOfData = data.size() - HEADER_SIZE; // Stop before footer

        while (offset < endOfData)
        {
            // Calculate safe chunk size (prevent crash on last partial block)
            const int bytesRemaining = endOfData - offset;
            const int chunkSize = std::min(bytesRemaining, MAX_CHUNK_SIZE);

            payload.append(data.sliced(offset, chunkSize));

            offset += chunkSize;

            // If we processed a full chunk and have more data, skip the injected header
            if (chunkSize == MAX_CHUNK_SIZE && offset < endOfData)
                offset += HEADER_SIZE;
        }

		if (payload.size() != header.size)
        {
            m_logger.warning("Fragmented payload size mismatch: " + QString::number(payload.size()) + " != " + QString::number(header.size));
            
        #ifdef _DEBUG
            assert(payload.size() == header.size);
        #endif
        }
	}
    else
    {
        const int payloadLen = data.size() - (HEADER_SIZE * 2);
        if (payloadLen > 0)
            payload = data.sliced(HEADER_SIZE, payloadLen);

        if (payload.size() != header.size)
			m_logger.warning("Single packet size mismatch: " + QString::number(payload.size()) + " != " + QString::number(header.size));
    }

    return true;
}

QByteArray CR35Device::createCommandPacket(const Command& command) const
{
    QByteArray payload;

    switch (command.type)
    {
        case TYPE_U32:
        {
            const quint32 v = command.value.toUInt();
            const quint32 be = qToBigEndian<quint32>(v);
            payload.append(reinterpret_cast<const char*>(&be), sizeof(be));
            break;
        }
        case TYPE_U16:
        {
            const quint16 v = static_cast<quint16>(command.value.toUInt());
            const quint16 be = qToBigEndian<quint16>(v);
            payload.append(reinterpret_cast<const char*>(&be), sizeof(be));
            break;
        }
        case TYPE_STRING:
        {
            payload = command.value.toString().toUtf8();
            payload.append('\x00'); // end with 0
            break;
        }
        case TYPE_BLOB:
        default:
        {
            if (command.value.canConvert<QByteArray>())
                payload = command.value.toByteArray();
            else
                payload = command.value.toString().toUtf8();
            break;
        }
    }

    // Protocol header matches server RX header layout (big-endian):
    // [Cmd:2] [Flags:2] [Token:4] [Len:4] [Type:2] then (clientId?) then payload
    constexpr quint16 cmd_id = PACKET_COMMAND; // command packet
    const quint16 flags = 0;

    const quint32 token = getTokenId(command.name);
    const quint32 length = static_cast<quint32>(payload.size());
    const quint16 typeId = static_cast<quint16>(command.type);

    QByteArray header;
    header.reserve(2 + 2 + 4 + 4 + 2);

    appendBE16(header, cmd_id);
    appendBE16(header, flags);
    appendBE32(header, token);
    appendBE32(header, length);
    appendBE16(header, typeId);

    return header + payload;
}

QByteArray CR35Device::createRequestTokenPacket(const QString& token) const
{
	constexpr quint16 cmd_id = PACKET_READ_TOKEN; // 0x03 for token request

	QByteArray payload = token.toUtf8();
    payload.append('\x00'); // end with 0
    const quint16 reserved = 0;
    const quint16 length = static_cast<quint16>(payload.size());

    QByteArray header;
    quint16 be16;

    be16 = qToBigEndian<quint16>(cmd_id);
    header.append(reinterpret_cast<const char*>(&be16), sizeof(be16));
    be16 = qToBigEndian<quint16>(reserved);
    header.append(reinterpret_cast<const char*>(&be16), sizeof(be16));
    be16 = qToBigEndian<quint16>(length);
    header.append(reinterpret_cast<const char*>(&be16), sizeof(be16));
    be16 = qToBigEndian<quint16>(0);
    header.append(reinterpret_cast<const char*>(&be16), sizeof(be16));

    // append CLIENT_ID 6 bytes
    header.append(m_clientId);

	const QByteArray packet = header + payload;
	return packet;
}

QByteArray CR35Device::createReadDataPacket(const Command& command) const
{
    constexpr quint16 cmd_id = PACKET_READ_DATA; // 0x10 for read data request
    const quint32 token_id = getTokenId(command.name);

    QByteArray packet;
    packet.reserve(2 + 2 + 4 + m_clientId.size() + 4 + 2);

    appendBE16(packet, cmd_id);
    appendBE16(packet, 0); // reserved

    // Token id (4 bytes)
    appendBE32(packet, token_id);

    // Device expects CLIENT_ID immediately after token id
    packet.append(m_clientId);

    return packet;
}

void CR35Device::sendCommand()
{
    // handle command queue
	if (m_commands.isEmpty()) return;

    if (m_currentCommand.packet != PACKET_UNKNOWN)
    {
		if (m_lastCommandTime.toMSecsSinceEpoch() + TIMEOUT_MS > QDateTime::currentMSecsSinceEpoch())
            return; // wait for current command to complete
        else
			m_logger.warning("Command timeout for: " + QString::fromUtf8(m_currentCommand.name));
    }

	m_currentCommand = m_commands.takeFirst();
	m_lastCommandTime = QDateTime::currentDateTime();

	QByteArray packet;
	switch (m_currentCommand.packet)
    {
        case PACKET_READ_TOKEN:
            packet = createRequestTokenPacket(m_currentCommand.name);
            break;
        case PACKET_READ_DATA:
            packet = createReadDataPacket(m_currentCommand);
            break;
        case PACKET_COMMAND:
        default:
            packet = createCommandPacket(m_currentCommand);
            break;
    }
	
    m_logger.message("Sending packet: " + QString::fromUtf8(m_currentCommand.name) + " Data= " + packet.toHex());
	m_socket.write(packet);
}

void CR35Device::init()
{
	m_logger.message("Socket connected to device");

    // enqueue tokens
    for (auto& token : TOKEN_REQUESTS)
	{
        if (!m_tokens.contains(token))
            enqueueCommand(Command(token, PACKET_READ_TOKEN));
	}

	// login sequence
	enqueueCommand(Command("Connect", TYPE_U16, 1));
	enqueueCommand(Command("UserId", TYPE_STRING, "user@BACKUP"));
	QString system_date = QDateTime::currentDateTimeUtc().toString("ddd, dd MMM yyyy HH:mm:ss 'GMT'");
	enqueueCommand(Command("SystemDate", TYPE_STRING, system_date));
	enqueueCommand(Command("ModeList"));
    //enqueueCommand(Command("DeviceId"));
    //enqueueCommand(Command("Version"));
    enqueueCommand(Command("SystemState"));

    m_commandQueueTimer.start();
}

void CR35Device::start(int mode)
{
    if (m_started || m_socket.state() != QAbstractSocket::ConnectedState) return;

	m_logger.message("Start Acquisition with mode: " + QString::number(mode));

    // start sequence
	enqueueCommand(Command("Mode", TYPE_U32, mode));
    enqueueCommand(Command("PollingOnly", TYPE_U32, 1));
    enqueueCommand(Command("Start", TYPE_U16, 1));

    m_imageData.clear();
}

void CR35Device::stop()
{
	if (!m_started || m_socket.state() != QAbstractSocket::ConnectedState) return;

    m_logger.message("Stop Acquisition");
	m_dataTimer.stop();

    // stop sequence
	enqueueCommand(Command("StopRequest", TYPE_U16, 1));
	enqueueCommand(Command("Stop", TYPE_U16, 1));
}

void CR35Device::sendImageDataRequest()
{
    if (!m_started) return;

    enqueueCommand(Command("SystemState"));
    enqueueCommand(Command("ImageData"));
}

void CR35Device::enqueueCommand(const Command& command)
{
	for (auto cmd : m_commands)
    {
        if(cmd == command) return; // already queued
    }

	m_commands.push_back(command);
}

void CR35Device::processImageData()
{
    if (m_imageData.isEmpty())
        return;

    m_logger.message("Processing received image data of size: " + QString::number(m_imageData.size()));

#ifdef _DEBUG
    QFile debugFile("CR35_Image.bin");
    if (debugFile.open(QIODevice::WriteOnly))
    {
        debugFile.write(m_imageData);
        debugFile.close();
	}
#endif

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(m_imageData.constData());
    const uint8_t* end = ptr + m_imageData.size();

	LineAssembler assembler;
	bool parsingPixels = false;
	int pixLine = 0; // maximum width of image

    while (ptr + UINT16_SIZE <= end)
    {
        uint16_t word = qFromLittleEndian<uint16_t>(ptr);
        ptr += UINT16_SIZE;

        // Check if the word is a Control Marker
        if (word >= 0xFFF9u) 
        {
            switch (word) 
            {
                case DATA_MARKER_START: 
                {
                    if (ptr + UINT16_SIZE > end) break;
				    // New line begins. Flush any previously open line now.
				    assembler.flushLine();
				    parsingPixels = false;

				    assembler.currentLine = {};
				    assembler.currentSeg = {};
				    assembler.inLine = true;
				    assembler.x = qFromLittleEndian<uint16_t>(ptr);
				    ptr += UINT16_SIZE;
				    parsingPixels = true;
                    break;
                }

                case DATA_MARKER_GAP:
                {
                    if (ptr + UINT16_SIZE > end) break;
				    const uint16_t gap = qFromLittleEndian<uint16_t>(ptr);
				    ptr += UINT16_SIZE;

				    if (assembler.inLine)
				    {
					    assembler.flushSegment();
						assembler.x = static_cast<uint16_t>(assembler.x + gap);
					    parsingPixels = true;
				    }
                    break;
                }

                case DATA_MARKER_CONFIG: 
                {
                    if (ptr + UINT16_SIZE > end) break;
                    uint16_t size = qFromLittleEndian<uint16_t>(ptr);
                    ptr += UINT16_SIZE;

                    if (ptr + size <= end)
                    {
                        QByteArray jsonData(reinterpret_cast<const char*>(ptr), size - 1);
                        ptr += size; // Read JSON data
                        m_logger.message("Parsing JSON config of size: " + QString::number(size));
                        pixLine = parseJsonConfig(jsonData);
                    }
                    else
                    {
                        ptr = end; // Skip incomplete data
                    }
                    break;
                }

                case DATA_MARKER_NOP:
                    break;
                case DATA_MARKER_IMAGE_END:
					assembler.flushLine();
					parsingPixels = false;
                    break;

                default:
					m_logger.warning("Unknown data marker: " + QString::number(word, 16));
                    break; // Ignore Heartbeats/Padding
            }
        }
        // Process Pixel Data
        else if (parsingPixels) 
        {
			if (!assembler.inLine)
				continue;

			if (!assembler.currentSeg.pixelDataPtr)
			{
				assembler.currentSeg.xStart = assembler.x;
				assembler.currentSeg.pixelDataPtr = reinterpret_cast<const uint16_t*>(ptr - UINT16_SIZE);
			}
			assembler.currentSeg.pixelCount++;
			assembler.x++;
        }
    }

	// If stream ended without explicit IMAGE_END, still flush whatever we parsed.
	assembler.flushLine();
	std::vector<ScanLine>& image = assembler.image;

	m_logger.message("Total lines received in image: " + QString::number(image.size()));

    if (image.empty())
        return;

    int minLeft = std::numeric_limits<int>::max();
    int maxRight = 0;

    // Calculate bounding box (crop empty space)
    for (const auto& line : image)
    {
		for (const auto& seg : line.segments)
		{
			if (seg.pixelCount <= 0)
				continue;
			minLeft = std::min(minLeft, seg.xStart);
			maxRight = std::max(maxRight, seg.xStart + seg.pixelCount);
		}
    }

    if (maxRight == 0) // No pixels found
        return;

    const int width = maxRight - minLeft;
    const int height = static_cast<int>(image.size());

	uint16_t* img = new uint16_t[width * height];
	memset(img, 0xFFFF, width * height * sizeof(uint16_t)); // initialize to white

    for (int y = 0; y < height; ++y)
    {
        const auto& line = image[y];
        uint16_t* dst = img + (y * width);

		if (pixLine > 0)
		{
			if (line.endX != pixLine)
			{
				m_logger.warning("Scanline width mismatch: line=" + QString::number(y) +
					" endX=" + QString::number(line.endX) +
					" pixLine=" + QString::number(pixLine) +
					" segments=" + QString::number(static_cast<int>(line.segments.size())));
				#ifdef _DEBUG
				assert(line.endX == pixLine);
				#endif
			}
		}

		for (const auto& seg : line.segments)
		{
			if (!seg.pixelDataPtr || seg.pixelCount <= 0)
				continue;

			const int offset = seg.xStart - minLeft;
			if (offset < 0)
				continue;

			const int copyCount = std::min(seg.pixelCount, width - offset);
			if (copyCount > 0)
				memcpy(dst + offset, seg.pixelDataPtr, copyCount * sizeof(uint16_t));
		}
    }

    emit imageDataReceived(img, width, height);
}

int CR35Device::parseJsonConfig(const QByteArray& jsonData) const
{
    // Device JSON strings may contain 8-bit characters
    // which is invalid UTF-8 for QJsonDocument. Convert from Latin-1 to UTF-8.
    const QString jsonText = QString::fromLatin1(jsonData);
    const QByteArray jsonBytes = jsonText.toUtf8();
    QJsonParseError jerr;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &jerr);
    if (jerr.error != QJsonParseError::NoError && doc.isNull())
        m_logger.warning("JSON parse failed: " + jerr.errorString());
        
    m_logger.message("Image JSON: " + jsonText);
    const QJsonObject root = doc.object();
    // Try to read a few useful fields for logging.
    const QString deviceModel = root.value("ManufacturerModelName").toString();
    const int bitsStored = root.value("BitsStored").toInt();
    int pixLine = -1;
    int slotCount = -1;
    if (root.contains("AdditionalScanInfo") && root.value("AdditionalScanInfo").isObject())
    {
        const QJsonObject asi = root.value("AdditionalScanInfo").toObject();
        pixLine = asi.value("PixLine").toInt(-1);
        slotCount = asi.value("SlotCount").toInt(-1);
    }
    m_logger.message("Image header parsed: model='" + deviceModel + "' bitsStored=" + QString::number(bitsStored) +
        " pixLine=" + QString::number(pixLine) + " slotCount=" + QString::number(slotCount));

	return pixLine;
}