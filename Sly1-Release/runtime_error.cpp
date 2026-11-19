#include "runtime_error.h"
#include "../platform/crash_report.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <ctime>
#endif
#include <cstdarg>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <iostream>
#include <mutex>

namespace
{
	std::mutex s_consoleMutex;
	bool s_consoleInitialized = false;
	std::mutex s_errorMutex;

}

void EnsureRuntimeErrorConsole()
{
	std::lock_guard<std::mutex> consoleLock(s_consoleMutex);
	if (s_consoleInitialized)
		return;
	#ifdef _WIN32
	// (output already going to a pipe or a file, the launcher's: keep it there, no console of its own)
	const DWORD errorType = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
	if (errorType == FILE_TYPE_PIPE || errorType == FILE_TYPE_DISK)
	{
		s_consoleInitialized = true;
		return;
	}
	if (GetConsoleWindow() == nullptr && AllocConsole() == FALSE)
		return;

	FILE* stream = nullptr;
	freopen_s(&stream, "CONOUT$", "w", stdout);
	freopen_s(&stream, "CONOUT$", "w", stderr);
	std::cout.clear();
	std::cerr.clear();
	SetConsoleTitleW(L"SafeHouse - Runtime Diagnostics");

	if (HWND consoleWindow = GetConsoleWindow())
	{
		ShowWindow(consoleWindow, SW_SHOW);
		SetForegroundWindow(consoleWindow);
	}
	#endif
	s_consoleInitialized = true;
}

void ReportRuntimeError(const char* subsystem, const char* format, ...)
{
	EnsureRuntimeErrorConsole();
	std::lock_guard<std::mutex> lock(s_errorMutex);

	#ifdef _WIN32
	SYSTEMTIME time{};
	GetLocalTime(&time);
	std::fprintf(stderr, "[%02u:%02u:%02u.%03u] [ERROR] [%s] ",
		time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
		subsystem != nullptr ? subsystem : "Runtime");
	#else
	const auto now = std::chrono::system_clock::now();
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()) % 1000;
	const std::time_t rawTime = std::chrono::system_clock::to_time_t(now);
	std::tm localTime{};
	localtime_r(&rawTime, &localTime);
	std::fprintf(stderr, "[%02d:%02d:%02d.%03lld] [ERROR] [%s] ",
		localTime.tm_hour, localTime.tm_min, localTime.tm_sec,
		static_cast<long long>(milliseconds.count()),
		subsystem != nullptr ? subsystem : "Runtime");
	#endif

	va_list arguments;
	va_start(arguments, format);
	std::vfprintf(stderr, format, arguments);
	va_end(arguments);
	std::fputc('\n', stderr);
	std::fflush(stderr);
}

void ReportFatalRuntimeError(const char* message)
{
	ReportRuntimeError("Fatal", "%s", message != nullptr ? message : "Unknown fatal error");
	#ifdef _WIN32
	MessageBoxW(nullptr,
		L"SafeHouse could not continue.\n\n"
		L"The error details were printed in the diagnostic console.",
		L"SafeHouse - Fatal Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
	#endif
}

// The crash handlers shared by every program (platform/crash_report.h): "crash:" lines on stderr, which the launcher
// keeps, and a minidump on Windows; no console window or message box (the launcher runs the game with its output piped).
void InstallRuntimeErrorHandlers()
{
	cane::crash::install("Sly1-Release");
}
