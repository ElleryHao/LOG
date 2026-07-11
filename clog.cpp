#include "clog.h"

#include <cstring>
#include <ctime>
#include <string>

#if LOG_USE_FILESYSTEM
  #include <filesystem>
#else
  #include <sys/stat.h>
#endif

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__linux__)
  #include <unistd.h>
#endif

// author hzkai

namespace {

const char* LevelMsg[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

// P1/F9: local time including milliseconds; localtime_s (Win) / localtime_r (POSIX) are thread-safe,
// unlike the gmtime() the old implementation used (F2).
void formatLocalTime(char* buf, std::size_t n)
{
	using namespace std::chrono;
	auto now = system_clock::now();
	std::time_t t = system_clock::to_time_t(now);
	int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);

	std::tm tmv{};
#if defined(_WIN32)
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char base[20];
	std::strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tmv);
	std::snprintf(buf, n, "%s.%03d", base, ms);
}

// P9/H5/F10: platform introspection for the executable's own path — independent of argv,
// so the default log file name works on every call path (not only argv-driven ones).
std::string executablePath()
{
#if defined(_WIN32)
	char buf[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, buf, sizeof(buf));
	if (len == 0 || len >= sizeof(buf)) return std::string();
	return std::string(buf, len);
#elif defined(__linux__)
	char buf[4096];
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len <= 0) return std::string();
	return std::string(buf, static_cast<std::size_t>(len));
#else
	return std::string();
#endif
}

bool fsExists(const std::string& path)
{
#if LOG_USE_FILESYSTEM
	std::error_code ec;
	return std::filesystem::exists(path, ec) && !ec;
#elif defined(_WIN32)
	struct __stat64 st;
	return _stat64(path.c_str(), &st) == 0;
#else
	struct stat st;
	return stat(path.c_str(), &st) == 0;
#endif
}

std::size_t fsSize(const std::string& path)
{
#if LOG_USE_FILESYSTEM
	std::error_code ec;
	auto sz = std::filesystem::file_size(path, ec);
	return ec ? 0 : static_cast<std::size_t>(sz);
#elif defined(_WIN32)
	struct __stat64 st;
	if (_stat64(path.c_str(), &st) != 0) return 0;
	return static_cast<std::size_t>(st.st_size);
#else
	struct stat st;
	if (stat(path.c_str(), &st) != 0) return 0;
	return static_cast<std::size_t>(st.st_size);
#endif
}

bool fsRename(const std::string& from, const std::string& to)
{
#if LOG_USE_FILESYSTEM
	std::error_code ec;
	std::filesystem::rename(from, to, ec);
	return !ec;
#else
	return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool fsRemove(const std::string& path)
{
#if LOG_USE_FILESYSTEM
	std::error_code ec;
	return std::filesystem::remove(path, ec);
#else
	return std::remove(path.c_str()) == 0;
#endif
}

// P5/F5: "same clock" age comparison (last_write_time vs. steady/system now), not wall-clock subtraction.
bool fsAgeExceedsDays(const std::string& path, int days)
{
	if (days <= 0) return false;
#if LOG_USE_FILESYSTEM
	std::error_code ec;
	auto mtime = std::filesystem::last_write_time(path, ec);
	if (ec) return false;
	auto age = std::filesystem::file_time_type::clock::now() - mtime;
	return age > std::chrono::hours(24 * days);
#else
  #if defined(_WIN32)
	struct __stat64 st;
	if (_stat64(path.c_str(), &st) != 0) return false;
	std::time_t mtime = st.st_mtime;
  #else
	struct stat st;
	if (stat(path.c_str(), &st) != 0) return false;
	std::time_t mtime = st.st_mtime;
  #endif
	std::time_t now = std::time(nullptr);
	return (now - mtime) > static_cast<std::time_t>(days) * 86400;
#endif
}

} // namespace

const char* levelName(LogLevel level)
{
	int i = static_cast<int>(level);
	return (i >= 0 && i < 6) ? LevelMsg[i] : "?";
}

std::string LogConfig::defaultLogPath()
{
	std::string p = executablePath();
	if (p.empty()) return "app.log";
	std::size_t sl = p.find_last_of("/\\");
	if (sl != std::string::npos) p = p.substr(sl + 1);
	std::size_t dot = p.find_last_of('.');
	if (dot != std::string::npos) p = p.substr(0, dot);
	return p.empty() ? "app.log" : p + ".log";
}

// R5/M2/M3/R9-2/R9-9: intentionally-leaked singleton — one instance for the process lifetime,
// never destroyed, so there is no static-destruction-order UAF. The OS reclaims it on exit.
Log& Log::getInstance()
{
	static Log* instance = new Log();
	return *instance;
}

Log::Log()
	: m_enabled(true), m_level(INFO), m_flushLevel(ERROR), m_maxQueueSize(100000),
	  m_stop(false), m_running(false), m_started(false), m_everStarted(false),
	  m_initialized(false), m_maintenanceReq(false),
	  m_draining(false), m_dropped(0), m_written(0), m_enqueuedOk(0), m_droppedTotal(0),
	  m_fileStream(nullptr), m_curSize(0), m_writeErrors(0),
	  m_lastCleanup(std::chrono::steady_clock::now())
{
}

Log::~Log()
{
	shutdown();   // defensive only; normally never runs (singleton is intentionally leaked)
}

void Log::applyConfig(const LogConfig& cfg)   // called only while no worker is alive
{
	m_cfg = cfg;
	if (m_cfg.filePath.empty()) m_cfg.filePath = LogConfig::defaultLogPath();          // H5
	if (m_cfg.flushIntervalMs < 1) m_cfg.flushIntervalMs = 1;                          // M5: no busy-wait(0)
	if (m_cfg.maxFileSize != 0 && m_cfg.maxFileSize < 512) m_cfg.maxFileSize = 512;    // M5
	if (m_cfg.maxBackupCount < 0) m_cfg.maxBackupCount = 0;
	if (m_cfg.maxBackupDays  < 0) m_cfg.maxBackupDays  = 0;
	m_filePath = m_cfg.filePath;
	m_writeErrors = 0;
	m_enabled.store(m_cfg.enabled);
	m_level.store(m_cfg.level);
	m_flushLevel.store(m_cfg.flushLevel);
	m_maxQueueSize.store(m_cfg.maxQueueSize);
}

bool Log::openStream()   // [B1] worker-owned file state, set up before the worker starts
{
	if (m_fileStream) { std::fclose(m_fileStream); m_fileStream = nullptr; }   // fixes openFile's reopen leak
	m_fileStream = std::fopen(m_filePath.c_str(), "ab");                       // binary append -> LF only (P2)
	if (!m_fileStream)
	{
		std::fprintf(stderr, "[logger] open %s failed, file sink disabled\n", m_filePath.c_str());
		return false;
	}
	m_curSize = fsSize(m_filePath);
	m_writeErrors = 0;
	return true;
}

void Log::registerExitHookOnce()
{
	static std::once_flag once;
	std::call_once(once, [] { std::atexit([] { Log::getInstance().shutdown(); }); });
}

void Log::startWorker()
{
	m_stop.store(false);
	m_running.store(true);   // set true before the thread is scheduled (H1)
	try
	{
		m_worker = std::thread(&Log::workerLoop, this);
		m_started.store(true);
		// FIX-H4: published once and never cleared by a reconfig — this is the flag writeLog's
		// auto-start check must use, so a writeLog racing init()'s reconfig window never sees
		// "not started" and never contends m_initMutex on the hot path.
		m_everStarted.store(true, std::memory_order_release);
	}
	catch (...)
	{
		m_running.store(false);
		std::fprintf(stderr, "[logger] cannot start worker\n");
	}
}

void Log::stopWorkerLocked(bool terminal)   // caller must hold m_initMutex
{
	if (!m_started.load()) { if (terminal) m_stop.store(true); return; }
	m_stop.store(true);
	m_cvWork.notify_all();
	if (m_worker.joinable()) m_worker.join();
	if (m_fileStream) { std::fflush(m_fileStream); std::fclose(m_fileStream); m_fileStream = nullptr; }
	m_running.store(false);
	m_started.store(false);
	if (!terminal) m_stop.store(false);   // reset so init() can restart; terminal shutdown stays true
}

void Log::init(const LogConfig& cfg)   // H4: authoritative, supports reconfiguration
{
	std::lock_guard<std::mutex> lk(m_initMutex);
	if (m_started.load()) stopWorkerLocked(/*terminal=*/false);   // already auto-started -> stop it first
	applyConfig(cfg);
	if (m_cfg.enabled) { openStream(); startWorker(); }
	m_initialized.store(true, std::memory_order_release);
	registerExitHookOnce();
}

void Log::ensureAutoStarted()   // safety net, mechanism kept separate from explicit init() (H4)
{
	// FIX-H4: gated on "ever started" (published once, never cleared), not "started under the
	// current config" — the latter is briefly false during init()'s reconfig window.
	if (m_everStarted.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lk(m_initMutex);
	if (m_everStarted.load()) return;
	applyConfig(LogConfig{});
	if (m_cfg.enabled) { openStream(); startWorker(); }
	m_initialized.store(true, std::memory_order_release);
	registerExitHookOnce();
}

void Log::writeLog(LogLevel level, const char* fmt, ...)
{
	if (!m_enabled.load(std::memory_order_relaxed)) return;                 // [B7]
	if ((int)level < m_level.load(std::memory_order_relaxed)) return;       // [B7]

	char body[LOG_STRING_LENGTH];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(body, sizeof(body), fmt, ap);   // reuse of the existing vsnprintf approach
	va_end(ap);

	char ts[32];
	formatLocalTime(ts, sizeof(ts));   // local time incl. ms

	// FIX-H4: only take the auto-start path (which may briefly touch m_initMutex) before this
	// singleton has EVER started. Once true, m_everStarted never flips back to false, so a
	// writeLog racing a live reconfig inside init() never falls into this branch — it never
	// blocks the business thread on m_initMutex.
	if (!m_everStarted.load(std::memory_order_acquire) && !m_stop.load(std::memory_order_relaxed))
		ensureAutoStarted();

	if (m_stop.load(std::memory_order_relaxed) || !m_running.load(std::memory_order_acquire))
	{
		// [B6][H1] degrade to stderr — the singleton is intentionally leaked so it is always
		// valid to call into (no UAF), it simply isn't accepting entries right now.
		std::fprintf(stderr, "%s %s | %s\n", ts, levelName(level), body);
		return;
	}

	std::string line;
	line.reserve(std::strlen(ts) + std::strlen(body) + 16);
	line += ts; line += ' '; line += levelName(level); line += " | "; line += body; line += '\n';   // \r\n -> \n (P2)

	{
		std::lock_guard<std::mutex> lk(m_mutex);
		std::size_t cap = m_maxQueueSize.load(std::memory_order_relaxed);
		if (cap && m_queue.size() >= cap)   // [B4]
		{
			m_dropped.fetch_add(1, std::memory_order_relaxed);        // periodic notice counter
			m_droppedTotal.fetch_add(1, std::memory_order_relaxed);   // FIX: running total for stats()
			return;
		}
		m_queue.push(Entry{ level, std::move(line) });
	}
	m_enqueuedOk.fetch_add(1, std::memory_order_relaxed);   // FIX: running total for stats()
	m_cvWork.notify_one();   // notify after unlocking
}

void Log::writeEntry(const std::string& line, bool countWritten)   // worker-only
{
	if (!m_fileStream) return;   // [B2]
	std::size_t n = std::fwrite(line.data(), 1, line.size(), m_fileStream);
	if (n < line.size())   // [B3][M4]
	{
		++m_writeErrors;
		m_curSize += n;
		if (m_writeErrors <= 3)
			std::fprintf(stderr, "[logger] short write on %s (%zu/%zu), disk full?\n",
			             m_filePath.c_str(), n, line.size());
		if (m_writeErrors >= 100)
		{
			std::fprintf(stderr, "[logger] too many write errors, disabling file sink\n");
			std::fclose(m_fileStream);
			m_fileStream = nullptr;
		}
		return;
	}
	m_writeErrors = 0;
	m_curSize += n;
	// FIX: reliable in-memory counter for stats() — independent of rotation/backup deletion.
	// countWritten=false for the internal "dropped N" notice line (see workerLoop) so it never
	// inflates the business-message tally that produced == written + dropped depends on.
	if (countWritten) m_written.fetch_add(1, std::memory_order_relaxed);
	rotateIfNeeded(0);
}

void Log::workerLoop()   // m_running already true (set by startWorker)
{
	for (;;)
	{
		std::queue<Entry> batch;
		bool stopping = false;
		try
		{
			{
				std::unique_lock<std::mutex> lk(m_mutex);
				// FIX-MED: wake for a pending maintenance request too, not just new entries/stop,
				// so triggerMaintenance() + flush() deterministically executes cleanup instead of
				// waiting up to flushIntervalMs (flaky under a tight test timeout).
				m_cvWork.wait_for(lk, std::chrono::milliseconds(m_cfg.flushIntervalMs),
				                  [this] { return !m_queue.empty() || m_stop.load() || m_maintenanceReq.load(); });
				m_draining = true;   // [C1] set the drain barrier under the lock, then swap
				std::swap(batch, m_queue);
				stopping = m_stop.load();
			}
			while (!batch.empty()) { writeEntry(batch.front().text); batch.pop(); }   // lock-free write (worker-owned)

			std::size_t d = m_dropped.exchange(0);
			if (d)
			{
				char ts[32];
				formatLocalTime(ts, sizeof(ts));
				// countWritten=false: this is an internal notice, not a business message counted
				// by any caller's "produced" tally (see writeEntry).
				writeEntry(std::string(ts) + " WARN | [logger] dropped " + std::to_string(d) + " messages\n",
				           /*countWritten=*/false);
			}
			if (m_fileStream) std::fflush(m_fileStream);   // [R9-4] flush per batch, not per line

			if (m_maintenanceReq.exchange(false)) cleanupOldBackups();   // [L5] on-demand cleanup
			maybeCleanup();                                              // 60s throttled fallback

			{ std::lock_guard<std::mutex> lk(m_mutex); m_draining = false; m_cvDrained.notify_all(); }   // [C1]
		}
		catch (...)   // [H1/B9] keep the worker alive across a bad iteration
		{
			std::fprintf(stderr, "[logger] worker iteration error, continuing\n");
			std::lock_guard<std::mutex> lk(m_mutex);
			m_draining = false;
			m_cvDrained.notify_all();
		}

		if (stopping)   // final, thorough drain
		{
			std::queue<Entry> tail;
			{ std::lock_guard<std::mutex> lk(m_mutex); std::swap(tail, m_queue); }
			try
			{
				while (!tail.empty()) { writeEntry(tail.front().text); tail.pop(); }
				if (m_fileStream) std::fflush(m_fileStream);
			}
			catch (...) {}
			// [H2/B10] unconditional notify before break, so a late flush() never deadlocks
			{ std::lock_guard<std::mutex> lk(m_mutex); m_draining = false; m_cvDrained.notify_all(); }
			break;
		}
	}
	m_running.store(false);
}

void Log::flush()
{
	if (!m_running.load()) return;   // [H1] no worker to wait on
	std::unique_lock<std::mutex> lk(m_mutex);
	m_cvWork.notify_one();           // wake a possibly idle worker
	// FIX-MED: also wait out a pending maintenance request, so a caller that does
	// triggerMaintenance() + flush() is guaranteed the cleanup has actually run by the time
	// flush() returns (otherwise time-based retention tests are flaky).
	m_cvDrained.wait(lk, [this] {
		return (m_queue.empty() && !m_draining && !m_maintenanceReq.load()) || m_stop.load();
	});
}

void Log::shutdown()   // idempotent, terminal
{
	std::lock_guard<std::mutex> lk(m_initMutex);
	stopWorkerLocked(/*terminal=*/true);   // m_stop stays true -> writeLog degrades to stderr afterwards
}

void Log::triggerMaintenance()
{
	m_maintenanceReq.store(true);
	m_cvWork.notify_one();
}

// enqueuedOk/written/dropped are plain atomic loads, safe at any time. writeErrors mirrors the
// worker-owned consecutive-short-write counter (§0.4 discipline) — call after flush()/shutdown()
// so the drain barrier's happens-before covers this read too, same rule as other worker-owned state.
LogStats Log::stats() const
{
	LogStats s;
	s.enqueuedOk  = m_enqueuedOk.load(std::memory_order_relaxed);
	s.written     = m_written.load(std::memory_order_relaxed);
	s.dropped     = m_droppedTotal.load(std::memory_order_relaxed);
	s.writeErrors = static_cast<unsigned long long>(m_writeErrors);
	return s;
}

void Log::rotateIfNeeded(std::size_t)
{
	if (m_cfg.maxFileSize == 0) return;
	if (m_curSize < m_cfg.maxFileSize) return;
	doRotate();
}

void Log::doRotate()   // worker-only, fully serialized (R9-5)
{
	if (m_fileStream) { std::fclose(m_fileStream); m_fileStream = nullptr; }
	if (m_cfg.maxBackupCount <= 0) { fsRemove(m_filePath); openStream(); return; }

	for (int k = m_cfg.maxBackupCount - 1; k >= 1; --k)
	{
		std::string src = m_filePath + "." + std::to_string(k);
		std::string dst = m_filePath + "." + std::to_string(k + 1);
		if (fsExists(src)) { fsRemove(dst); fsRename(src, dst); }
	}
	if (fsExists(m_filePath))
	{
		std::string d1 = m_filePath + ".1";
		fsRemove(d1);   // [M1] remove before rename so Windows doesn't fail on an existing target
		fsRename(m_filePath, d1);
	}
	openStream();   // fresh file, m_curSize reset to 0
	cleanupOldBackups();
}

void Log::cleanupOldBackups()   // worker-only
{
	for (int k = m_cfg.maxBackupCount + 1; k <= m_cfg.maxBackupCount + 64; ++k)   // [L2] don't stop at a gap
	{
		std::string p = m_filePath + "." + std::to_string(k);
		if (fsExists(p)) fsRemove(p);
	}
	if (m_cfg.maxBackupDays > 0)
	{
		for (int k = 1; k <= m_cfg.maxBackupCount; ++k)
		{
			std::string p = m_filePath + "." + std::to_string(k);
			if (fsExists(p) && fsAgeExceedsDays(p, m_cfg.maxBackupDays)) fsRemove(p);
		}
	}
}

void Log::maybeCleanup()   // 60s throttle
{
	auto now = std::chrono::steady_clock::now();
	if (now - m_lastCleanup < std::chrono::seconds(60)) return;
	m_lastCleanup = now;
	cleanupOldBackups();
}

int Log::openFile(const char* fileName)   // legacy-signature compat; cannot rename a running sink (H3)
{
	if (m_started.load())
	{
		std::fprintf(stderr, "[logger] openFile ignored: already running; use init()\n");
		return EE_FAILURE;
	}
	LogConfig c;
	c.filePath = fileName ? fileName : "";
	init(c);
	return m_running.load() ? EE_SUCCESS : EE_FAILURE;
}
