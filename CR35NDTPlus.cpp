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
		m_device.start(5);
		});
	connect(ui.pushButtonStop, &QPushButton::clicked, &m_device, &CR35Device::stop);
	connect(&m_device, &CR35Device::imageDataReceived, this, &CR35NDTPlus::saveImage);

}

void CR35NDTPlus::saveImage(const QImage& img)
{
	img.save("cr35_image.png");
}