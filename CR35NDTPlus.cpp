#include "CR35NDTPlus.h"

CR35NDTPlus::CR35NDTPlus(Logger& logger, QWidget* parent) : QMainWindow(parent),
m_device(logger)
{
	ui.setupUi(this);

	connect(&logger, &Logger::newMessageLogged, ui.plainTextEditLog, &QPlainTextEdit::appendPlainText);
	connect(ui.pushButtonConnect, &QPushButton::clicked, this, [this]() {
		m_device.connectToDevice("192.168.177.101", 2006);
		});
	connect(ui.pushButtonDisconnect, &QPushButton::clicked, &m_device, &CR35Device::disconnectFromDevice);

	connect(ui.pushButtonStart, &QPushButton::clicked, this, [this]() {
		m_device.start(5); // hardcoded mode 5 for example
		});
	connect(ui.pushButtonStop, &QPushButton::clicked, &m_device, &CR35Device::stop);
	connect(&m_device, &CR35Device::imageDataReceived, this, &CR35NDTPlus::saveImage);

}

void CR35NDTPlus::saveImage(const uint16_t* data, int width, int height)
{
	if (!data || width <= 0 || height <= 0)
		return;

	// QImage expects scanlines to be 32-bit aligned for many formats.
	// The incoming buffer is tightly packed (width * 2 bytes) and may not satisfy that.
	QImage img(width, height, QImage::Format_Grayscale16);
	if (img.isNull())
		return;

	const int srcBytesPerLine = width * static_cast<int>(sizeof(uint16_t));
	for (int y = 0; y < height; ++y)
	{
		const uchar* src = reinterpret_cast<const uchar*>(data) + y * srcBytesPerLine;
		uchar* dst = img.scanLine(y);
		memcpy(dst, src, srcBytesPerLine);
	}

	img.save("CR35_Image.png");
}