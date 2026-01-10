#pragma once

#include <qstring.h>
#include <qfile.h>

/**
 * @brief Logger class for thread-safe logging to a file with log rotation.
 *
 * This class provides methods to log messages, warnings, and errors to a file.
 * It supports log file rotation when the file exceeds a maximum size.
 * Logging operations are performed in a separate thread.
 */
class Logger : public QObject {
	Q_OBJECT

public:
	/**
	 * @brief Constructs a Logger object and opens the log file.
	 * @param name The base name of the log file.
	 */
	Logger(const QString& name);

	/**
	 * @brief Destructor. Closes the log file and cleans up resources.
	 */
	~Logger();

public slots:
	/**
	 * @brief Logs a general message.
	 * @param msg The message to log.
	 */
	void message(const QString& msg);

	/**
	 * @brief Logs a warning message.
	 * @param msg The warning message to log.
	 */
	void warning(const QString& msg);

	/**
	 * @brief Logs an error message.
	 * @param msg The error message to log.
	 */
	void error(const QString& msg);

private slots:
	/**
	 * @brief Writes a message to the log file (internal use).
	 * @param msg The message to write.
	 */
	void writeLog(const QString& msg);

signals:
	/**
	 * @brief Signal emitted when a new message is logged.
	 * @param msg The message that was logged.
	 */
	void newMessageLogged(const QString&);

protected:
	/**
	 * @brief Maximum allowed log file size in bytes before rotation (1 MB).
	 */
	enum : int {
		LOG_MAX_SIZE = 1024 * 1024,	///< 1 MB max
	};

	/**
	 * @brief Rotates the log file when it exceeds the maximum size.
	 */
	void rotateLogFile();

	QFile m_logFile;        ///< Log file object
};