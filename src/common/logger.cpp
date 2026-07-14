#include "logger.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace {

std::mutex g_log_mutex;

const char* level_tag(LogLevel level) {
	switch (level) {
		case LogLevel::DEBUG:
			return "DEBUG";
		case LogLevel::INFO:
			return "INFO";
	}
	return "?";
}

}  // anonymous namespace

void log_impl(LogLevel level, const char* file, int line, const char* fmt, ...) {
	// 1. Timestamp — all formatting happens BEFORE taking the lock.
	const auto now = std::chrono::system_clock::now();
	const std::time_t t = std::chrono::system_clock::to_time_t(now);
	const auto ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

	std::tm tm_buf;
	localtime_r(&t, &tm_buf);

	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

	// 2. Format the caller's message into a fixed-size stack buffer.
	char msg[1024];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	// 3. Assemble the full line, then do one write under the mutex.
	char linebuf[1280];
	std::snprintf(linebuf, sizeof(linebuf), "%s.%03d [%-5s] %s:%d  %s\n", ts,
	              static_cast<int>(ms.count()), level_tag(level), file, line, msg);

	std::lock_guard<std::mutex> guard(g_log_mutex);
	std::fputs(linebuf, stdout);
}
