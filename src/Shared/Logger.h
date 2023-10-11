#pragma once
#include <source_location>
#include <spdlog/spdlog.h>

class Logger {
public:
	static Logger& Get() noexcept {
		static Logger instance;
		return instance;
	}

	// 在 exe 中调用
	bool Initialize(spdlog::level::level_enum logLevel, const char* logFileName, int logArchiveAboveSize, int logMaxArchiveFiles) noexcept;
	// 在 dll 中调用
	bool Initialize(Logger& logger) noexcept;

	void SetLevel(spdlog::level::level_enum logLevel);

	void Flush() {
		_logger->flush();
	}

	void Info(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::info, msg, location);
	}

	void Win32Info(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::info, _MakeWin32ErrorMsg(msg), location);
	}

	void ComInfo(std::string_view msg, HRESULT hr, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::info, _MakeComErrorMsg(msg, hr), location);
	}

	void Warn(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::warn, msg, location);
	}

	void Win32Warn(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::warn, _MakeWin32ErrorMsg(msg), location);
	}

	void ComWarn(std::string_view msg, HRESULT hr, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::warn, _MakeComErrorMsg(msg, hr), location);
	}

	void Error(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::err, msg, location);
	}

	void Win32Error(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::err, _MakeWin32ErrorMsg(msg), location);
	}

	void ComError(std::string_view msg, HRESULT hr, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::err, _MakeComErrorMsg(msg, hr), location);
	}

	void Critical(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::critical, msg, location);
	}

	void Win32Critical(std::string_view msg, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::critical, _MakeWin32ErrorMsg(msg), location);
	}

	void ComCritical(std::string_view msg, HRESULT hr, const std::source_location& location = std::source_location::current()) {
		_Log(spdlog::level::critical, _MakeComErrorMsg(msg, hr), location);
	}

private:
	static std::string _MakeWin32ErrorMsg(std::string_view msg);

	static std::string _MakeComErrorMsg(std::string_view msg, HRESULT hr);

	void _Log(spdlog::level::level_enum logLevel, std::string_view msg, const std::source_location& location);

	std::shared_ptr<spdlog::logger> _logger;
};
