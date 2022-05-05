#include "log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#include <windows.h>

namespace dechamps_cpplog {

	namespace {

		template <auto functionPointer> struct FunctionPointerFunctor {
			template <typename... Args> auto operator()(Args&&... args) {
				return functionPointer(std::forward<Args>(args)...);
			}
		};

		using Library = std::unique_ptr<std::decay_t<decltype(*HMODULE())>, FunctionPointerFunctor<FreeLibrary>>;

		// On systems that support GetSystemTimePreciseAsFileTime() (i.e. Windows 8 and greater), use that.
		// Otherwise, fall back to GetSystemTimeAsFileTime().
		// See https://github.com/dechamps/FlexASIO/issues/15
		decltype(GetSystemTimeAsFileTime)* GetSystemTimeAsFileTimeFunction() {
			static const auto function = [] {
				static const Library library(LoadLibraryA("kernel32.dll"));
				if (library != nullptr) {
					const auto function = GetProcAddress(library.get(), "GetSystemTimePreciseAsFileTime");
					if (function != nullptr) return reinterpret_cast<decltype(GetSystemTimePreciseAsFileTime)*>(function);
				}
				return GetSystemTimeAsFileTime;
			}();
			return function;
		}

		int64_t FileTimeToTenthsOfUs(const FILETIME filetime) {
			ULARGE_INTEGER integer;
			integer.LowPart = filetime.dwLowDateTime;
			integer.HighPart = filetime.dwHighDateTime;
			return integer.QuadPart;
		}

		std::string FormatSystemTimeISO8601(const SYSTEMTIME& systemtime) {
			std::stringstream stream;
			stream.fill('0');
			stream << std::setw(2) << systemtime.wYear << "-" << std::setw(2) << systemtime.wMonth << "-" << std::setw(2) << systemtime.wDay;
			stream << "T";
			stream << std::setw(2) << systemtime.wHour << ":" << std::setw(2) << systemtime.wMinute << ":" << std::setw(2) << systemtime.wSecond;
			return stream.str();
		}

		std::pair<TIME_ZONE_INFORMATION, LONG> GetTimezoneAndBias() {
			TIME_ZONE_INFORMATION localTimezone;
			// Note: contrary to what MSDN seems to indicate, TIME_ZONE_INFORMATION::Bias does *not* include the StandardBias/DaylightBias.
			switch (GetTimeZoneInformation(&localTimezone)) {
			case TIME_ZONE_ID_UNKNOWN:
				return { localTimezone, localTimezone.Bias };
			case TIME_ZONE_ID_STANDARD:
				return { localTimezone, localTimezone.Bias + localTimezone.StandardBias };
			case TIME_ZONE_ID_DAYLIGHT:
				return { localTimezone, localTimezone.Bias + localTimezone.DaylightBias };
			default:
				abort();
			}
		}

		std::string FormatFiletimeISO8601(const FILETIME filetime) {
			SYSTEMTIME systemtimeUTC;
			if (!FileTimeToSystemTime(&filetime, &systemtimeUTC)) abort();
			systemtimeUTC.wMilliseconds = 0;

			const auto localTimezoneAndBias = GetTimezoneAndBias();

			SYSTEMTIME systemtimeLocal;
			if (!SystemTimeToTzSpecificLocalTime(&localTimezoneAndBias.first, &systemtimeUTC, &systemtimeLocal)) abort();

			std::stringstream stream;
			stream.fill('0');

			stream << FormatSystemTimeISO8601(systemtimeLocal);

			FILETIME truncatedFiletime;
			if (!SystemTimeToFileTime(&systemtimeUTC, &truncatedFiletime)) abort();
			stream << "." << std::setw(7) << FileTimeToTenthsOfUs(filetime) - FileTimeToTenthsOfUs(truncatedFiletime);

			stream << ((localTimezoneAndBias.second >= 0) ? "-" : "+");
			const auto absoluteBias = std::abs(localTimezoneAndBias.second);
			stream << std::setw(2) << absoluteBias / 60 << ":" << std::setw(2) << absoluteBias % 60;

			return stream.str();
		}


		std::string GetModuleName() {
			std::string moduleName(MAX_PATH, 0);
			moduleName.resize(GetModuleFileNameA(NULL, moduleName.data(), DWORD(moduleName.size())));
			return moduleName;
		}

	}

	PreambleLogSink::PreambleLogSink(LogSink& backend) : backend(backend) {
		Logger(this) << "Log time source: " << ((GetSystemTimeAsFileTimeFunction() == GetSystemTimeAsFileTime) ? "GetSystemTimeAsFileTime" : "GetSystemTimePreciseAsFileTime");
		Logger(this) << "Host process: " << GetModuleName();
	}

	FileLogSink::FileLogSink(std::filesystem::path path, Options options) : path(std::move(path)), options(std::move(options)), stream(this->path, std::ios::app | std::ios::out) {
		Logger(this) << "Logfile opened: " << this->path;
		CheckSize();
	}

	FileLogSink::~FileLogSink() {
		Logger(this) << "Closing logfile";
	}

	void FileLogSink::Write(const std::string_view str) {
		if (!stream.is_open()) return;

		stream_sink.Write(str);

		const auto size = str.size();
		if (size >= bytesRemainingUntilSizeCheck) CheckSize();
		else bytesRemainingUntilSizeCheck -= size;
	}

	void FileLogSink::CheckSize() {
		bytesRemainingUntilSizeCheck = (std::numeric_limits<uintmax_t>::max)();

		const auto sizeBytes = std::filesystem::file_size(path);
		const auto maxSizeBytes = options.maxSizeBytes;
		Logger(this) << "Current log file size is " << sizeBytes << " bytes (maximum allowed: " << maxSizeBytes << " bytes)";

		if (sizeBytes < maxSizeBytes) {
			bytesRemainingUntilSizeCheck = options.sizeCheckPeriodBytes;
			return;
		}
		Logger(this) << "Closing logfile as the maximum size is exceeded";
		stream.close();
	}

	void ThreadSafeLogSink::Write(const std::string_view str) {
		std::scoped_lock lock(mutex);
		backend.Write(str);
	}

	void AsyncLogSink::Write(const std::string_view str) {
		std::string strCopy(str);
		bool notify;
		{
			std::scoped_lock lock(mutex);
			if (shutdown) abort();
			notify = queue.empty();
			queue.emplace_back(std::move(strCopy));
		}
		if (notify) stateChanged.notify_all();
	}

	void AsyncLogSink::RunThread() {
		for (;;) {
			std::vector<std::string> localQueue;
			localQueue.reserve(32);
			bool localShutdown;

			{
				std::unique_lock lock(mutex);
				stateChanged.wait(lock, [&] { return !queue.empty() || shutdown; });
				localQueue.swap(queue);
				localShutdown = shutdown;
			}
			
			for (const auto& str : localQueue) backend.Write(str);

			if (localShutdown) break;
		}
	}

	AsyncLogSink::~AsyncLogSink() {
		{
			std::scoped_lock lock(mutex);
			shutdown = true;
		}
		stateChanged.notify_all();
		thread.join();
	}

	Logger::Logger(LogSink* sink, const Options& options) {
		if (sink == nullptr) return;

		enabledState.emplace(*sink);
		auto& stream = enabledState->stream;

		if (options.prependTime) {
			FILETIME now = { 0 };
			GetSystemTimeAsFileTimeFunction()(&now);
			stream << FormatFiletimeISO8601(now) << " ";
		}
		if (options.prependProcessId) stream << GetCurrentProcessId() << " ";
		if (options.prependThreadId) stream << GetCurrentThreadId() << " ";
	}

	Logger::~Logger() {
		if (!enabledState.has_value()) return;
		enabledState->sink.Write(enabledState->stream.str());
	}

}