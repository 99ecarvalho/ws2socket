/**
 * @file logging.h
 * @brief Logging system for ws2socket
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Provides logging functionality with configurable levels and output targets.
 * Supports syslog and file output.
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_LOGGING_H
#define WS2SOCKET_LOGGING_H

#include <stdio.h>
#include <time.h>

/**
 * @defgroup LogLevels Logging Levels
 * @{
 */

/** Debug level - detailed diagnostic information */
#define LOG_DEBUG 0
/** Info level - general informational messages */
#define LOG_INFO 1
/** Warning level - warning messages */
#define LOG_WARN 2
/** Error level - error messages */
#define LOG_ERROR 3
/** Critical level - critical system errors */
#define LOG_CRITICAL 4

/** @} */

/**
 * @defgroup LogTargets Logging Targets
 * @{
 */

/** Log to console (stderr) */
#define LOG_TARGET_CONSOLE 1
/** Log to syslog */
#define LOG_TARGET_SYSLOG 2
/** Log to file */
#define LOG_TARGET_FILE 4

/** @} */

/**
 * @struct logger_config
 * @brief Logger configuration
 * 
 * Contains logging configuration including level, targets, and output file.
 */
typedef struct {
    /** Logging level (see @ref LogLevels) */
    int level;
    /** Logging targets bitmask (see @ref LogTargets) */
    int targets;
    /** Output file path (if FILE target enabled) */
    const char *logfile;
    /** File handle for logging */
    FILE *fp;
    /** Use syslog facility (if SYSLOG target enabled) */
    int syslog_facility;
} logger_config_t;

/**
 * @brief Initialize logging system
 * 
 * Sets up the logging system with the specified configuration.
 * 
 * @param config Pointer to logger_config_t structure
 * @return WS_SUCCESS on success, error code otherwise
 * 
 * @note Must be called before any logging functions
 */
int log_init(logger_config_t *config);

/**
 * @brief Shutdown logging system
 * 
 * Closes file handles and cleans up logging resources.
 * 
 * @return WS_SUCCESS on success, error code otherwise
 */
int log_shutdown(void);

/**
 * @brief Log a message at specified level
 * 
 * @param level Logging level (see @ref LogLevels)
 * @param fmt Format string (printf-style)
 * @param ... Variable arguments
 * 
 * @note Thread-safe implementation
 */
void log_message(int level, const char *fmt, ...);

/**
 * @brief Log debug message
 * 
 * @param fmt Format string (printf-style)
 * @param ... Variable arguments
 */
#define log_debug(fmt, ...) log_message(LOG_DEBUG, fmt, ##__VA_ARGS__)

/**
 * @brief Log info message
 * 
 * @param fmt Format string (printf-style)
 * @param ... Variable arguments
 */
#define log_info(fmt, ...) log_message(LOG_INFO, fmt, ##__VA_ARGS__)

/**
 * @brief Log warning message
 * 
 * @param fmt Format string (printf-style)
 * @param ... Variable arguments
 */
#define log_warn(fmt, ...) log_message(LOG_WARN, fmt, ##__VA_ARGS__)

/**
 * @brief Log error message
 * 
 * @param fmt Format string (printf-style)
 * @param ... Variable arguments
 */
#define log_error(fmt, ...) log_message(LOG_ERROR, fmt, ##__VA_ARGS__)

/**
 * @brief Log critical error message
 * 
 * @param fmt Format string (printf-style)
 * @param ... Variable arguments
 */
#define log_critical(fmt, ...) log_message(LOG_CRITICAL, fmt, ##__VA_ARGS__)

/**
 * @brief Set logging level
 * 
 * @param level New logging level (see @ref LogLevels)
 */
void log_set_level(int level);

/**
 * @brief Get current logging level
 * 
 * @return Current logging level
 */
int log_get_level(void);

#endif /* WS2SOCKET_LOGGING_H */
