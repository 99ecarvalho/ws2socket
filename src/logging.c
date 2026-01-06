/**
 * @file logging.c
 * @brief Logging system implementation
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * License: LGPL v3
 */

#include "logging.h"
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <pthread.h>
#include <time.h>

/** Global logger configuration */
static logger_config_t *g_logger_config = NULL;
/** Mutex for thread-safe logging */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Initialize logging system
 */
int log_init(logger_config_t *config)
{
    if (!config) {
        return -1;
    }

    pthread_mutex_lock(&g_log_mutex);

    g_logger_config = config;

    /* Initialize file output if needed */
    if ((config->targets & LOG_TARGET_FILE) && config->logfile) {
        config->fp = fopen(config->logfile, "a");
        if (!config->fp) {
            pthread_mutex_unlock(&g_log_mutex);
            return -1;
        }
        setvbuf(config->fp, NULL, _IOLBF, 1024);
    }

    /* Initialize syslog if needed */
    if (config->targets & LOG_TARGET_SYSLOG) {
        openlog("ws2socket", LOG_PID | LOG_CONS, config->syslog_facility);
    }

    pthread_mutex_unlock(&g_log_mutex);

    return 0;
}

/**
 * @brief Shutdown logging system
 */
int log_shutdown(void)
{
    pthread_mutex_lock(&g_log_mutex);

    if (g_logger_config) {
        if ((g_logger_config->targets & LOG_TARGET_FILE) && g_logger_config->fp) {
            fclose(g_logger_config->fp);
            g_logger_config->fp = NULL;
        }

        if (g_logger_config->targets & LOG_TARGET_SYSLOG) {
            closelog();
        }

        g_logger_config = NULL;
    }

    pthread_mutex_unlock(&g_log_mutex);

    return 0;
}

/**
 * @brief Get log level name
 */
static const char *log_level_name(int level)
{
    switch (level) {
        case LOG_DEBUG:    return "DEBUG";
        case LOG_INFO:     return "INFO";
        case LOG_WARN:     return "WARNING";
        case LOG_ERROR:    return "ERROR";
        case LOG_CRITICAL: return "CRITICAL";
        default:           return "UNKNOWN";
    }
}

/**
 * @brief Get syslog priority from log level
 */
static int log_level_to_syslog(int level)
{
    switch (level) {
        case LOG_DEBUG:    return LOG_DEBUG;
        case LOG_INFO:     return LOG_INFO;
        case LOG_WARN:     return LOG_WARNING;
        case LOG_ERROR:    return LOG_ERR;
        case LOG_CRITICAL: return LOG_CRIT;
        default:           return LOG_INFO;
    }
}

/**
 * @brief Log a message at specified level
 */
void log_message(int level, const char *fmt, ...)
{
    va_list args;
    char timestamp[32];
    time_t now;
    struct tm *tm_info;

    if (!g_logger_config || level < g_logger_config->level) {
        return;
    }

    /* Get current time for timestamp */
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    pthread_mutex_lock(&g_log_mutex);

    /* Log to console */
    if (g_logger_config->targets & LOG_TARGET_CONSOLE) {
        flockfile(stderr);  /* Atomic output across processes */
        va_start(args, fmt);
        fprintf(stderr, "[%s] %s: ", timestamp, log_level_name(level));
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
        funlockfile(stderr);
    }

    /* Log to file */
    if ((g_logger_config->targets & LOG_TARGET_FILE) && g_logger_config->fp) {
        va_start(args, fmt);
        fprintf(g_logger_config->fp, "[%s] %s: ", timestamp, log_level_name(level));
        vfprintf(g_logger_config->fp, fmt, args);
        fprintf(g_logger_config->fp, "\n");
        va_end(args);
    }

    /* Log to syslog */
    if (g_logger_config->targets & LOG_TARGET_SYSLOG) {
        va_start(args, fmt);
        vsyslog(log_level_to_syslog(level), fmt, args);
        va_end(args);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

/**
 * @brief Set logging level
 */
void log_set_level(int level)
{
    pthread_mutex_lock(&g_log_mutex);
    if (g_logger_config) {
        g_logger_config->level = level;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/**
 * @brief Get current logging level
 */
int log_get_level(void)
{
    int level = LOG_INFO;

    pthread_mutex_lock(&g_log_mutex);
    if (g_logger_config) {
        level = g_logger_config->level;
    }
    pthread_mutex_unlock(&g_log_mutex);

    return level;
}
