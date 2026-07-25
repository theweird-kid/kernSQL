#pragma once

#include <cstdint>

enum class LogLevel : uint8_t { DEBUG, INFO };

void log_impl(LogLevel level, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG_INFO(fmt, ...) \
	log_impl(LogLevel::INFO, __FILE_NAME__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#ifndef NDEBUG
#define LOG_DEBUG(fmt, ...) \
	log_impl(LogLevel::DEBUG, __FILE_NAME__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif
