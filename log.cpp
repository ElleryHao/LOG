// Multi-threaded validation prototype for the async logger (DESIGN.md §9).
// No command-line parsing: the library never parses config/argv (user decision 2) — this main()
// hand-builds the LogConfig the way any real caller would.
#include "clog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if LOG_USE_FILESYSTEM
  #include <filesystem>
#endif

namespace {

constexpr int kThreads   = 4;
constexpr int kPerThread = 2000;
const char*   kDemoLog   = "demo.log";

std::atomic<long long> g_produced{ 0 };

void writerThread(int id)
{
	for (int i = 0; i < kPerThread; ++i)
	{
		// Rotate through every level; TRACE/DEBUG are below the INFO threshold and must be
		// dropped by the filter before they ever reach the queue (R3/B7).
		LogLevel lvl = static_cast<LogLevel>(i % 6);
		if (lvl == INFO || lvl == WARN || lvl == ERROR || lvl == FATAL)
			g_produced.fetch_add(1, std::memory_order_relaxed);
		Log::getInstance().writeLog(lvl, "thread=%d seq=%d level=%s", id, i, levelName(lvl));
	}
}

struct FileTally { long long lines = 0; long long noticeLines = 0; long long droppedNoticeSum = 0; };

// Reads back demo.log + demo.log.1.. and reports how many lines actually made it to disk, plus
// the total the worker itself reported via its "[logger] dropped N messages" notices (R6/B4).
// `lines` counts every persisted line, including the notices themselves; `noticeLines` counts
// just the notices, so real business-message lines == lines - noticeLines.
FileTally tallyLogFiles(const std::string& base)
{
	FileTally t;
	std::vector<std::string> candidates{ base };
	for (int k = 1; k <= 32; ++k) candidates.push_back(base + "." + std::to_string(k));

	const std::string marker = "dropped ";
	for (const auto& path : candidates)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.is_open()) continue;
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty()) continue;
			++t.lines;
			auto pos = line.find(marker);
			if (pos == std::string::npos) continue;
			auto numStart = pos + marker.size();
			auto numEnd = line.find(' ', numStart);
			if (numEnd == std::string::npos) continue;
			try
			{
				t.droppedNoticeSum += std::stoll(line.substr(numStart, numEnd - numStart));
				++t.noticeLines;
			}
			catch (...) {}
		}
	}
	return t;
}

} // namespace

int main()
{
	// [B8/H4 self-proof] Log before init(): auto-starts on the default file name (own exe name),
	// completely independent of the LogConfig main is about to build.
	LOG_INFO("before init");

	LogConfig cfg;
	cfg.level          = INFO;
	cfg.filePath       = kDemoLog;
	cfg.maxFileSize    = 4096;   // small on purpose -> forces size-based rotation quickly (R4)
	cfg.maxBackupCount = 3;
	cfg.maxBackupDays  = 30;
	cfg.maxQueueSize   = 200;    // small on purpose -> 4 threads x 2000 msgs can overrun it (R6)

	// [H4] Already auto-started above -> init() stops that worker, applies cfg, restarts on demo.log.
	Log::getInstance().init(cfg);

	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int i = 0; i < kThreads; ++i) threads.emplace_back(writerThread, i);
	for (auto& th : threads) th.join();

	Log::getInstance().flush();   // waits on the drain barrier: everything queued is now on disk (C1)

	// Time-based retention demo (L5): age demo.log.3 by 40 days, then ask the worker to clean up.
	// The writer threads have joined and flush() has returned, so no concurrent writer touches the
	// rotated files here; only the worker will read them, via triggerMaintenance's mutex/notify,
	// which establishes the required happens-before with this main-thread mtime write.
#if LOG_USE_FILESYSTEM
	{
		std::error_code ec;
		std::string oldBackup = std::string(kDemoLog) + ".3";
		if (std::filesystem::exists(oldBackup, ec))
		{
			std::filesystem::last_write_time(
				oldBackup,
				std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 40),
				ec);
		}
	}
#endif
	Log::getInstance().triggerMaintenance();
	// FIX-MED: flush() also waits out a pending maintenance request, so cleanupOldBackups() is
	// guaranteed to have already run by the time this call returns (deterministic, not flaky).
	Log::getInstance().flush();

	FileTally tally    = tallyLogFiles(kDemoLog);
	long long produced = g_produced.load();
	long long dropped  = tally.droppedNoticeSum;
	long long written   = tally.lines - tally.noticeLines;   // real message lines, excluding notices
	std::printf("produced=%lld written=%lld dropped=%lld\n", produced, written, dropped);

	Log::getInstance().shutdown();   // graceful shutdown: drain + fflush + join (atexit also covers this)
	return 0;
}
