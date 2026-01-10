#include "Logger.h"

#include <qdir.h>
#include <qfileinfo.h>
#include <qcoreapplication.h>
#include <qthread.h>


Logger::Logger(const QString& name) : QObject(nullptr)
{
	const QDir appDir = QCoreApplication::applicationDirPath();
	if (!appDir.exists("log")) appDir.mkdir("log");
	const QString logPath = appDir.absoluteFilePath("log/" + QFileInfo(name).baseName() + ".txt");
	m_logFile.setFileName(logPath);

	QThread *thread = new QThread();
	moveToThread(thread);
	
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	connect(this, &Logger::newMessageLogged, this, &Logger::writeLog);

	thread->start();
	thread->setPriority(QThread::LowPriority);
}

Logger::~Logger()
{
	// Signal the logger thread to close the file
	QMetaObject::invokeMethod(this, [this]() {
		if (m_logFile.isOpen())
			m_logFile.close();
	}, QThread::currentThread() == thread() ? Qt::DirectConnection : Qt::BlockingQueuedConnection);
	
	thread()->quit();
	thread()->wait();
}

void Logger::message(const QString& msg)
{
	QMetaObject::invokeMethod(this, &Logger::newMessageLogged, QDateTime::currentDateTime().toString("[yyyy-MM-dd hh:mm:ss.zzz] ") + msg);
}

void Logger::warning(const QString& msg)
{
	message("WARNING: " + msg);
}

void Logger::error(const QString& msg)
{
	message("ERROR: " + msg);
}

void Logger::writeLog(const QString& msg)
{
	if(!m_logFile.isOpen())
		m_logFile.open(QIODevice::Text | QIODevice::WriteOnly | QIODevice::Append);

	m_logFile.write((msg + "\n").toUtf8());
	m_logFile.flush();

	if (m_logFile.size() > LOG_MAX_SIZE)
		rotateLogFile();
}

void Logger::rotateLogFile()
{
	m_logFile.close();

	// Define the log file paths
	const QString logFilePath = m_logFile.fileName();
	const QFileInfo fileInfo(logFilePath);
	const QString rotatedLogFilePath = QString("%1/%2.1.%3")
		.arg(fileInfo.absolutePath())
		.arg(fileInfo.completeBaseName())
		.arg(fileInfo.suffix());

	// Remove the old rotated file if it exists
	if (QFile::exists(rotatedLogFilePath))
		QFile::remove(rotatedLogFilePath);

	// Rename the current log file to the rotated log file
	QFile::rename(logFilePath, rotatedLogFilePath);
}
