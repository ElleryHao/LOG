#ifndef _LOG_H
#define _LOG_H

#include <cstdio>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>

// R1/L4: filesystem fallback gate — pinned per design §7.
// Detected via __has_include rather than the __cpp_lib_filesystem feature-test macro: the latter
// is only *defined* once <filesystem> (or <version>) has actually been included, so checking it
// here — before any such include — always evaluates false and would silently force every C++17
// build onto the stat/rename fallback. __has_include has no such ordering dependency.
#if defined(LOG_NO_FILESYSTEM)
  #define LOG_USE_FILESYSTEM 0
#elif defined(__has_include)
  #if __has_include(<filesystem>)
    #define LOG_USE_FILESYSTEM 1
  #else
    #define LOG_USE_FILESYSTEM 0
  #endif
#else
  #define LOG_USE_FILESYSTEM 0
#endif

#if LOG_USE_FILESYSTEM
  #include <filesystem>
#endif

#define LOG_STRING_LENGTH 1024
#define EE_SUCCESS 1
#define EE_FAILURE 0
#define EE_SUCSESS EE_SUCCESS   // R9-6: keep legacy misspelling as alias for compatibility

enum LogLevel
{
	TRACE,
	DEBUG,
	INFO,
	WARN,
	ERROR,
	FATAL
};

// R5: safe accessor replacing the old per-TU static array in the header.
const char* levelName(LogLevel level);

// R3/R4: sole configuration entry point — the library never parses files/argv (user decision 2).
struct LogConfig
{
	bool         enabled         = true;                  // total on/off switch
	LogLevel     level           = INFO;                  // default threshold
	std::string  filePath;                                // empty -> defaultLogPath() (H5, platform introspection)
	std::size_t  maxFileSize     = 10 * 1024 * 1024;       // per-file cap in bytes; 0 = no size-based rotation
	int          maxBackupCount  = 5;                      // backup cap; 0 = keep no backups
	int          maxBackupDays   = 30;                     // delete backups older than this; 0 = no time-based cleanup
	std::size_t  maxQueueSize    = 100000;                 // queue high-water mark; 0 = unbounded
	LogLevel     flushLevel      = ERROR;                  // >= this level forces an immediate fflush of its batch
	unsigned     flushIntervalMs = 1000;                   // periodic wake/flush interval (ms); init clamps to >=1

	static std::string defaultLogPath();                   // platform introspection: exe basename + ".log" (H5)
};

// Reliable in-memory counters, independent of rotation/backup deletion — a validation harness
// that reads these instead of rescanning log files on disk gets an accurate tally even when
// rows have since been rotated away or a backup was pruned by count/age (see log.cpp).
struct LogStats
{
	unsigned long long enqueuedOk;   // writeLog calls that were accepted and pushed onto the queue
	unsigned long long written;      // lines actually fwrite()-succeeded to disk (business lines only)
	unsigned long long dropped;      // writeLog calls dropped because the queue was at maxQueueSize
	unsigned long long writeErrors;  // current consecutive short-write streak (see writeEntry)
};

class Log
{
public:
	~Log();

	static Log& getInstance();

	void init(const LogConfig& cfg);
	void writeLog(LogLevel logLevel, const char* fmt, ...);
	void flush();
	void shutdown();
	void triggerMaintenance();
	LogStats stats() const;               // snapshot of reliable in-memory counters (see LogStats)

	int openFile(const char* fileName);   // legacy-signature compat shim; cannot rename a running sink (H3)

private:
	Log();
	Log(const Log&) = delete;
	Log& operator=(const Log&) = delete;

	struct Entry { LogLevel level; std::string text; };

	void applyConfig(const LogConfig& cfg);
	void ensureAutoStarted();
	void startWorker();
	void stopWorkerLocked(bool terminal);
	void workerLoop();
	void writeEntry(const std::string& line, bool countWritten = true);   // countWritten=false for internal notices
	void rotateIfNeeded(std::size_t);
	void doRotate();
	void cleanupOldBackups();
	void maybeCleanup();
	bool openStream();
	void registerExitHookOnce();

private:
	// Hot-path atomics (read from foreground threads)
	std::atomic<bool>        m_enabled;
	std::atomic<int>         m_level;
	std::atomic<int>         m_flushLevel;
	std::atomic<std::size_t> m_maxQueueSize;   // writeLog reads this, never m_cfg (removes a race)

	// Lifecycle atomics
	std::atomic<bool>        m_stop;           // stop requested (terminal true after shutdown)
	std::atomic<bool>        m_running;        // worker alive (H1)
	std::atomic<bool>        m_started;        // worker started under the *current* config (H4)
	std::atomic<bool>        m_everStarted;    // FIX-H4: published once, first startWorker; never cleared on reconfig
	std::atomic<bool>        m_initialized;
	std::atomic<bool>        m_maintenanceReq; // request worker to run cleanup on next wake (L5)

	LogConfig                m_cfg;            // published before worker start; read-only to worker thereafter

	// Queue + synchronization
	std::mutex               m_mutex;          // guards m_queue + m_draining
	std::condition_variable  m_cvWork;
	std::condition_variable  m_cvDrained;
	std::queue<Entry>        m_queue;
	bool                     m_draining;       // drain barrier (C1); read/written only under m_mutex
	std::atomic<std::size_t> m_dropped;        // periodically exchanged(0) by workerLoop to emit the
	                                            // "dropped N" notice — NOT a running total, see m_droppedTotal
	std::atomic<std::uint64_t> m_written;      // writeEntry successes (business lines only, stats())
	std::atomic<std::uint64_t> m_enqueuedOk;   // writeLog pushes accepted onto m_queue (stats())
	std::atomic<std::uint64_t> m_droppedTotal; // cumulative drop count for stats(); never reset

	std::mutex               m_initMutex;      // serializes init/auto-start/stop transitions (H4)
	std::thread              m_worker;

	// Worker-owned file state (published before worker thread starts, then worker-exclusive)
	std::FILE*   m_fileStream;
	std::string  m_filePath;
	std::size_t  m_curSize;
	std::size_t  m_writeErrors;
	std::chrono::steady_clock::time_point m_lastCleanup;
};

#define LOG_TRACE(...) Log::getInstance().writeLog(TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) Log::getInstance().writeLog(DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  Log::getInstance().writeLog(INFO,  __VA_ARGS__)
#define LOG_WARN(...)  Log::getInstance().writeLog(WARN,  __VA_ARGS__)
#define LOG_ERROR(...) Log::getInstance().writeLog(ERROR, __VA_ARGS__)
#define LOG_FATAL(...) Log::getInstance().writeLog(FATAL, __VA_ARGS__)

#endif
