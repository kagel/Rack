#include <system.hpp>
#include <string.hpp>

#include <thread>
#include <dirent.h>
#include <sys/stat.h>

#if defined ARCH_LIN || defined ARCH_MAC
	#include <pthread.h>
	#include <sched.h>
	#include <execinfo.h> // for backtrace and backtrace_symbols
	#include <unistd.h> // for execl
	#include <sys/utsname.h>
#endif

#if defined ARCH_WIN
	#include <windows.h>
	#include <shellapi.h>
	#include <processthreadsapi.h>
	#include <dbghelp.h>
#endif


namespace rack {
namespace system {


std::list<std::string> getEntries(const std::string &path) {
	std::list<std::string> filenames;
	DIR *dir = opendir(path.c_str());
	if (dir) {
		struct dirent *d;
		while ((d = readdir(dir))) {
			std::string filename = d->d_name;
			if (filename == "." || filename == "..")
				continue;
			filenames.push_back(path + "/" + filename);
		}
		closedir(dir);
	}
	filenames.sort();
	return filenames;
}

bool isFile(const std::string &path) {
	struct stat statbuf;
	if (stat(path.c_str(), &statbuf))
		return false;
	return S_ISREG(statbuf.st_mode);
}

bool isDirectory(const std::string &path) {
	struct stat statbuf;
	if (stat(path.c_str(), &statbuf))
		return false;
	return S_ISDIR(statbuf.st_mode);
}

void moveFile(const std::string &srcPath, const std::string &destPath) {
	std::remove(destPath.c_str());
	// Whether this overwrites existing files is implementation-defined.
	// i.e. Mingw64 fails to overwrite.
	// This is why we remove the file above.
	std::rename(srcPath.c_str(), destPath.c_str());
}

void copyFile(const std::string &srcPath, const std::string &destPath) {
	// Open source
	FILE *source = fopen(srcPath.c_str(), "rb");
	if (!source)
		return;
	DEFER({
		fclose(source);
	});
	// Open destination
	FILE *dest = fopen(destPath.c_str(), "wb");
	if (!dest)
		return;
	DEFER({
		fclose(dest);
	});
	// Copy buffer
	const int bufferSize = (1<<15);
	char buffer[bufferSize];
	while (1) {
		size_t size = fread(buffer, 1, bufferSize, source);
		if (size == 0)
			break;
		size = fwrite(buffer, 1, size, dest);
		if (size == 0)
			break;
	}
}

void createDirectory(const std::string &path) {
#if defined ARCH_WIN
	std::wstring pathW = string::toWstring(path);
	CreateDirectoryW(pathW.c_str(), NULL);
#else
	mkdir(path.c_str(), 0755);
#endif
}

int getLogicalCoreCount() {
	return std::thread::hardware_concurrency();
}

void setThreadName(const std::string &name) {
#if defined ARCH_LIN
	pthread_setname_np(pthread_self(), name.c_str());
#elif defined ARCH_WIN
	// Unsupported on Windows
#endif
}

void setThreadRealTime(bool realTime) {
#if defined ARCH_LIN
	int err;
	int policy;
	struct sched_param param;
	if (realTime) {
		// Round-robin scheduler policy
		policy = SCHED_RR;
		param.sched_priority = sched_get_priority_max(policy);
	}
	else {
		// Default scheduler policy
		policy = 0;
		param.sched_priority = 0;
	}
	err = pthread_setschedparam(pthread_self(), policy, &param);
	assert(!err);

	// pthread_getschedparam(pthread_self(), &policy, &param);
	// DEBUG("policy %d priority %d", policy, param.sched_priority);
#elif defined ARCH_MAC
	// Not yet implemented
#elif defined ARCH_WIN
	// Set process class first
	if (realTime) {
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	}
	else {
		SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
	}
#endif
}

std::string getStackTrace() {
	int stackLen = 128;
	void *stack[stackLen];
	std::string s;

#if defined ARCH_LIN || defined ARCH_MAC
	stackLen = backtrace(stack, stackLen);
	char **strings = backtrace_symbols(stack, stackLen);

	for (int i = 1; i < stackLen; i++) {
		s += string::f("%d: %s\n", stackLen - i - 1, strings[i]);
	}
	free(strings);
#elif defined ARCH_WIN
	HANDLE process = GetCurrentProcess();
	SymInitialize(process, NULL, true);
	stackLen = CaptureStackBackTrace(0, stackLen, stack, NULL);

	SYMBOL_INFO *symbol = (SYMBOL_INFO*) calloc(sizeof(SYMBOL_INFO) + 256, 1);
	symbol->MaxNameLen = 255;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

	for (int i = 1; i < stackLen; i++) {
		SymFromAddr(process, (DWORD64) stack[i], 0, symbol);
		s += string::f("%d: %s 0x%0x\n", stackLen - i - 1, symbol->Name, symbol->Address);
	}
	free(symbol);
#endif

	return s;
}

void openBrowser(const std::string &url) {
#if defined ARCH_LIN
	std::string command = "xdg-open \"" + url + "\"";
	(void) std::system(command.c_str());
#endif
#if defined ARCH_MAC
	std::string command = "open \"" + url + "\"";
	std::system(command.c_str());
#endif
#if defined ARCH_WIN
	std::wstring urlW = string::toWstring(url);
	ShellExecuteW(NULL, L"open", urlW.c_str(), NULL, NULL, SW_SHOWDEFAULT);
#endif
}

void openFolder(const std::string &path) {
#if defined ARCH_LIN
	std::string command = "xdg-open \"" + path + "\"";
	(void) std::system(command.c_str());
#endif
#if defined ARCH_MAC
	std::string command = "open \"" + path + "\"";
	std::system(command.c_str());
#endif
#if defined ARCH_WIN
	std::wstring pathW = string::toWstring(path);
	ShellExecuteW(NULL, L"explore", pathW.c_str(), NULL, NULL, SW_SHOWDEFAULT);
#endif
}


void runProcessDetached(const std::string &path) {
#if defined ARCH_WIN
	STARTUPINFOW startupInfo;
	PROCESS_INFORMATION processInfo;

	std::memset(&startupInfo, 0, sizeof(startupInfo));
	startupInfo.cb = sizeof(startupInfo);
	std::memset(&processInfo, 0, sizeof(processInfo));

	std::wstring pathW = string::toWstring(path);
	CreateProcessW(pathW.c_str(), NULL,
		NULL, NULL, false, 0, NULL, NULL,
		&startupInfo, &processInfo);
#else
	// Not implemented on Linux or Mac
	assert(0);
#endif
}


std::string getOperatingSystemInfo() {
#if defined ARCH_LIN || defined ARCH_MAC
	struct utsname u;
	uname(&u);
	return string::f("%s %s %s %s", u.sysname, u.release, u.version, u.machine);
#elif defined ARCH_WIN
	OSVERSIONINFOW info;
	ZeroMemory(&info, sizeof(info));
	info.dwOSVersionInfoSize = sizeof(info);
	GetVersionExW(&info);
	// See https://docs.microsoft.com/en-us/windows/desktop/api/winnt/ns-winnt-_osversioninfoa for a list of Windows version numbers.
	return string::f("Windows %u.%u", info.dwMajorVersion, info.dwMinorVersion);
#endif
}


} // namespace system
} // namespace rack
