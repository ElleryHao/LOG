// Multi-threaded validation prototype for the async logger (DESIGN.md §9).
// No command-line parsing: the library never parses config/argv (user decision 2) — this main()
// hand-builds each LogConfig the way any real caller would.
//
// FIX (test-stage repair round): earlier revisions of this file recomputed `written` by rescanning
// demo.log/.1/.2/.3 after the run. Rows that a size-based rotation or a backup-count/age cleanup
// legitimately deletes are invisible to that rescan, so `written` was systematically undercounted
// and `written == produced - dropped` could never hold. This version reads Log::stats() instead —
// a reliable in-memory counter maintained by the library itself, independent of what rotation later
// does to the files on disk — and splits the old single stress test into a few scenarios that are
// each deterministic for the specific AC they prove (see DESIGN.md AC-R2-1/R4-1/R4-2/R4-3/R6-1).
#include "clog.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
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

// Existence check without depending on <filesystem> (works in both the C++17 and the
// -DLOG_NO_FILESYSTEM/C++11 build).
bool fileExists(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return f.good();
}

// Writes kPerThread messages cycling through all 6 levels; counts into `produced` only the ones
// at/above INFO — the ones the library's level filter actually accepts (R3/B7). TRACE/DEBUG are
// issued too, so the accounting scenario also doubles as the level-filter check (AC-R3-1).
void writeAllLevels(int id, std::atomic<long long>* produced)
{
	for (int i = 0; i < kPerThread; ++i)
	{
		LogLevel lvl = static_cast<LogLevel>(i % 6);
		if (static_cast<int>(lvl) >= static_cast<int>(INFO))
			produced->fetch_add(1, std::memory_order_relaxed);
		Log::getInstance().writeLog(lvl, "thread=%d seq=%d level=%s", id, i, levelName(lvl));
	}
}

// Writes kPerThread INFO-level messages back-to-back with no filtering ambiguity, to maximize
// producer/consumer contention for the drop scenario (R6).
void writeBurstInfo(int id, std::atomic<long long>* produced)
{
	for (int i = 0; i < kPerThread; ++i)
	{
		produced->fetch_add(1, std::memory_order_relaxed);
		Log::getInstance().writeLog(INFO, "thread=%d seq=%d burst", id, i);
	}
}

void runThreads(void (*fn)(int, std::atomic<long long>*), std::atomic<long long>* produced)
{
	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int i = 0; i < kThreads; ++i) threads.emplace_back(fn, i, produced);
	for (auto& th : threads) th.join();
}

// Prints "produced=.. written=.. dropped=.. writeErrors=.." for one scenario and asserts the
// library-level tally, computed as a delta of two Log::stats() snapshots so it is unaffected by
// whatever earlier scenarios (or the pre-init auto-start self-proof) already wrote (AC-R2-1).
void printAndVerify(const char* label, long long produced, const LogStats& before, const LogStats& after)
{
	long long written = static_cast<long long>(after.written - before.written);
	long long dropped = static_cast<long long>(after.dropped - before.dropped);
	std::printf("[%s] produced=%lld written=%lld dropped=%lld writeErrors=%llu\n",
	            label, produced, written, dropped, after.writeErrors);
	assert(written + dropped == produced);   // core invariant, independent of rotation/backup deletion
}

// AC-R2-1 (+ AC-R3-1 level filter, AC-R5-1 ms timestamp / no CRLF): unbounded queue and no size
// rotation, so this scenario's own file is never disturbed by drop-newest or backup deletion —
// the cleanest possible setting to prove written + dropped == produced.
void runAccountingScenario()
{
	const char* path = "acct.log";
	LogConfig cfg;
	cfg.level          = INFO;
	cfg.filePath       = path;
	cfg.maxFileSize    = 0;   // no size-based rotation (sentinel, §4)
	cfg.maxBackupCount = 5;
	cfg.maxBackupDays  = 30;
	cfg.maxQueueSize   = 0;   // unbounded (sentinel, §4) -> guaranteed zero drops
	Log::getInstance().init(cfg);

	LogStats before = Log::getInstance().stats();
	std::atomic<long long> produced{ 0 };
	runThreads(&writeAllLevels, &produced);
	Log::getInstance().flush();
	LogStats after = Log::getInstance().stats();

	printAndVerify("accounting", produced.load(), before, after);
	assert(after.dropped == before.dropped);   // unbounded queue -> literally cannot drop

	// AC-R3-1/AC-R5-1: format checks against the single, un-rotated file this scenario wrote.
	std::ifstream in(path, std::ios::binary);
	std::string line;
	bool sawCR = false, sawFilteredLevel = false;
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') sawCR = true;
		// Line format is "<ts> <LEVEL> | <body>" (writeLog) — " TRACE | " / " DEBUG | " right
		// after the timestamp is the level field itself, not a coincidental substring of body.
		if (line.find(" TRACE | ") != std::string::npos || line.find(" DEBUG | ") != std::string::npos)
			sawFilteredLevel = true;
	}
	assert(!sawCR);              // AC-R5-1: fopen("ab") + '\n'-only writes -> no CRLF
	assert(!sawFilteredLevel);   // AC-R3-1: TRACE/DEBUG never reach the queue, let alone the file
}

// AC-R4-1/AC-R4-2: small maxFileSize forces many rotations; doRotate's shift window
// (.1->.2, .2->.3, base->.1) never creates a .4 no matter how many rotations occur, so the file
// set stabilizes deterministically at demo.log+.1+.2+.3 as long as at least 3 rotations happen.
// Unbounded queue again, so this scenario's own produced/written/dropped tally stays clean too.
void runRotationScenario()
{
	LogConfig cfg;
	cfg.level          = INFO;
	cfg.filePath       = "demo.log";
	cfg.maxFileSize    = 4096;   // small on purpose -> forces size-based rotation quickly (R4)
	cfg.maxBackupCount = 3;
	cfg.maxBackupDays  = 30;
	cfg.maxQueueSize   = 0;      // unbounded -> rotation existence checks aren't muddied by drops
	Log::getInstance().init(cfg);

	LogStats before = Log::getInstance().stats();
	std::atomic<long long> produced{ 0 };
	runThreads(&writeAllLevels, &produced);
	Log::getInstance().flush();   // waits on the drain barrier: everything queued is now on disk (C1)
	LogStats after = Log::getInstance().stats();

	printAndVerify("rotation", produced.load(), before, after);

	assert(fileExists("demo.log"));
	assert(fileExists("demo.log.1"));
	assert(fileExists("demo.log.2"));
	assert(fileExists("demo.log.3"));    // AC-R4-1
	assert(!fileExists("demo.log.4"));   // AC-R4-2
}

// AC-R4-3 (FIX-MED): depends on demo.log.3 from runRotationScenario() above. Ages it by 40 days
// then asks the worker to clean up. Writer threads from the rotation scenario have already joined
// and flush() has returned, so no concurrent writer touches the rotated files here; only the
// worker reads them, via triggerMaintenance's mutex/notify, which establishes the happens-before
// this main-thread mtime write needs.
void runRetentionScenario()
{
	const std::string oldBackup = "demo.log.3";
	assert(fileExists(oldBackup));

#if LOG_USE_FILESYSTEM
	{
		std::error_code ec;
		std::filesystem::last_write_time(
			oldBackup,
			std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 40),
			ec);
	}
	Log::getInstance().triggerMaintenance();
	// FIX-MED: flush() also waits out a pending maintenance request, so cleanupOldBackups() is
	// guaranteed to have already run by the time this call returns (deterministic, not flaky).
	Log::getInstance().flush();
	bool stillExists = fileExists(oldBackup);
	std::printf("[retention] demo.log.3 aged 40d, exists-after-cleanup=%d\n", stillExists ? 1 : 0);
	assert(!stillExists);   // AC-R4-3
#else
	std::printf("[retention] skipped: LOG_NO_FILESYSTEM build has no last_write_time() setter\n");
#endif
}

// AC-R6-1: a queue capped at 1 under 4-thread concurrent burst load guarantees drop-newest fires,
// while the library's own accounting (enqueuedOk vs droppedTotal) must still balance exactly
// against every accepted writeLog call, however many of them get dropped.
void runDropScenario()
{
	LogConfig cfg;
	cfg.level          = INFO;
	cfg.filePath       = "drop.log";
	cfg.maxFileSize    = 0;
	cfg.maxBackupCount = 5;
	cfg.maxBackupDays  = 30;
	cfg.maxQueueSize   = 1;   // tiny on purpose -> guarantees drop-newest under concurrent burst (R6)
	Log::getInstance().init(cfg);

	LogStats before = Log::getInstance().stats();
	std::atomic<long long> produced{ 0 };
	runThreads(&writeBurstInfo, &produced);
	Log::getInstance().flush();
	LogStats after = Log::getInstance().stats();

	printAndVerify("drop", produced.load(), before, after);
	assert(after.dropped > before.dropped);   // AC-R6-1: cap=1 under concurrent burst -> guaranteed drop
}

// AC-R2-2/AC-R3-2: the very first LOG_INFO in main() (before any init()) auto-starts the worker on
// the platform-introspected default filename, independent of the LogConfig any scenario built.
void checkAutoStartDefaultName()
{
	std::string def = LogConfig::defaultLogPath();
	bool exists = fileExists(def);
	std::printf("[auto-start] default-log=%s exists=%d\n", def.c_str(), exists ? 1 : 0);
	assert(exists);
}

} // namespace

int main()
{
	// [B8/H4 self-proof] Log before init(): auto-starts on the default file name (own exe name),
	// completely independent of the LogConfig any scenario below builds.
	LOG_INFO("before init");

	runAccountingScenario();      // AC-R2-1, AC-R3-1, AC-R5-1
	runRotationScenario();        // AC-R4-1, AC-R4-2
	runRetentionScenario();       // AC-R4-3 (FIX-MED)
	runDropScenario();            // AC-R6-1
	checkAutoStartDefaultName();  // AC-R2-2, AC-R3-2

	Log::getInstance().shutdown();   // graceful shutdown: drain + fflush + join (atexit also covers this)
	return 0;
}
