#include "process.hpp"

#include <array>
#include <sstream>

#include "except.hpp"
#include "unicode.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace nsmb {

namespace {

std::string describe(const std::vector<std::string>& argv)
{
	std::ostringstream oss;
	for (std::size_t i = 0; i < argv.size(); i++)
		oss << (i == 0 ? "" : " ") << argv[i];
	return oss.str();
}

#ifdef _WIN32

// The quoting rules CommandLineToArgvW undoes on the other side. Backslashes
// only matter when they run up against a quote, which is why the count is
// carried rather than each one escaped where it stands.
std::wstring quoteArgument(const std::wstring& argument)
{
	if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
		return argument;

	std::wstring out = L"\"";
	for (std::size_t i = 0; ; i++)
	{
		std::size_t backslashes = 0;
		while (i < argument.size() && argument[i] == L'\\')
		{
			i++;
			backslashes++;
		}

		if (i == argument.size())
		{
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (argument[i] == L'"')
		{
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
		}
		else
		{
			out.append(backslashes, L'\\');
			out.push_back(argument[i]);
		}
	}
	out.push_back(L'"');
	return out;
}

struct Handle
{
	HANDLE h = nullptr;
	~Handle() { if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
	void reset() { if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = nullptr; }
};

std::string drain(HANDLE pipe)
{
	std::string out;
	std::array<char, 4096> buffer{};
	DWORD read = 0;
	while (ReadFile(pipe, buffer.data(), DWORD(buffer.size()), &read, nullptr) && read > 0)
		out.append(buffer.data(), read);
	return out;
}

#else

// Reads both pipes at once. Reading one to EOF and then the other deadlocks the
// moment the child fills the pipe it is not being read from, and git writes
// enough to stderr during a fetch to do exactly that.
void drainBoth(int outFd, int errFd, std::string& outText, std::string& errText)
{
	std::array<pollfd, 2> fds{};
	fds[0] = { outFd, POLLIN, 0 };
	fds[1] = { errFd, POLLIN, 0 };

	std::array<char, 4096> buffer{};
	int open = 2;
	while (open > 0)
	{
		if (poll(fds.data(), fds.size(), -1) < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}

		for (std::size_t i = 0; i < fds.size(); i++)
		{
			if (fds[i].fd < 0 || fds[i].revents == 0)
				continue;

			const ssize_t count = ::read(fds[i].fd, buffer.data(), buffer.size());
			if (count > 0)
			{
				(i == 0 ? outText : errText).append(buffer.data(), std::size_t(count));
			}
			else if (count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN))
			{
				fds[i].fd = -1;
				open--;
			}
		}
	}
}

#endif

} // namespace

ProcessResult run(const std::vector<std::string>& argv, const fs::path& workDir, Capture capture)
{
	if (argv.empty())
		throw nsmb::exception("Tried to run an empty command.");

	ProcessResult result;

#ifdef _WIN32
	// Wide throughout, and CreateProcessW below. The narrow CreateProcessA takes
	// the machine's ANSI code page, which on a Windows install whose user name is
	// not ASCII cannot spell the store path at all.
	std::wstring commandLine;
	for (std::size_t i = 0; i < argv.size(); i++)
	{
		if (i != 0)
			commandLine.push_back(L' ');
		commandLine += quoteArgument(toWide(argv[i]));
	}

	SECURITY_ATTRIBUTES inherit{};
	inherit.nLength = sizeof(inherit);
	inherit.bInheritHandle = TRUE;

	Handle outRead, outWrite, errRead, errWrite;
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);

	if (capture == Capture::Output)
	{
		if (!CreatePipe(&outRead.h, &outWrite.h, &inherit, 0)
		    || !CreatePipe(&errRead.h, &errWrite.h, &inherit, 0))
		{
			throw nsmb::exception("Could not create a pipe to read git's output.");
		}
		SetHandleInformation(outRead.h, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(errRead.h, HANDLE_FLAG_INHERIT, 0);

		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdOutput = outWrite.h;
		startup.hStdError = errWrite.h;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	}

	// The path is already UTF-16 on Windows, so this one needs no conversion at
	// all -- which is the other half of the reason to be wide here.
	const std::wstring directory = workDir.empty() ? std::wstring() : workDir.wstring();

	PROCESS_INFORMATION process{};
	if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr,
	        capture == Capture::Output ? TRUE : FALSE, 0, nullptr,
	        directory.empty() ? nullptr : directory.c_str(), &startup, &process))
	{
		throw nsmb::exception("Could not run " + describe(argv) + ".");
	}

	if (capture == Capture::Output)
	{
		// The parent's copies have to go, or the reads below never see EOF.
		outWrite.reset();
		errWrite.reset();
		result.out = drain(outRead.h);
		result.err = drain(errRead.h);
	}

	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(process.hProcess, &code);
	result.exitCode = int(code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
#else
	int outPipe[2] = { -1, -1 };
	int errPipe[2] = { -1, -1 };
	if (capture == Capture::Output)
	{
		if (pipe(outPipe) != 0 || pipe(errPipe) != 0)
			throw nsmb::exception("Could not create a pipe to read git's output.");
	}

	// A pipe that carries nothing unless exec fails, and that closes itself when
	// exec succeeds. Without it there is no way to tell a program that could not
	// be started from one that started and exited 127 -- which matters, because
	// the first means git is not installed and the second means git said no.
	int errorPipe[2] = { -1, -1 };
	if (pipe(errorPipe) != 0)
		throw nsmb::exception("Could not create a pipe.");
	fcntl(errorPipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC);

	std::vector<char*> raw;
	raw.reserve(argv.size() + 1);
	for (const std::string& argument : argv)
		raw.push_back(const_cast<char*>(argument.c_str()));
	raw.push_back(nullptr);

	const pid_t pid = fork();
	if (pid < 0)
	{
		close(errorPipe[0]);
		close(errorPipe[1]);
		throw nsmb::exception(std::string("Could not fork: ") + std::strerror(errno) + ".");
	}

	if (pid == 0)
	{
		if (!workDir.empty() && chdir(workDir.c_str()) != 0)
			_exit(127);

		if (capture == Capture::Output)
		{
			dup2(outPipe[1], STDOUT_FILENO);
			dup2(errPipe[1], STDERR_FILENO);
			close(outPipe[0]);
			close(outPipe[1]);
			close(errPipe[0]);
			close(errPipe[1]);
		}

		execvp(raw[0], raw.data());

		const int failure = errno;
		const ssize_t ignored = write(errorPipe[1], &failure, sizeof(failure));
		(void)ignored;
		_exit(127);
	}

	close(errorPipe[1]);

	if (capture == Capture::Output)
	{
		close(outPipe[1]);
		close(errPipe[1]);
		drainBoth(outPipe[0], errPipe[0], result.out, result.err);
		close(outPipe[0]);
		close(errPipe[0]);
	}

	int execError = 0;
	const ssize_t reported = ::read(errorPipe[0], &execError, sizeof(execError));
	close(errorPipe[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
	{
		if (errno != EINTR)
			throw nsmb::exception("Lost track of " + describe(argv) + ".");
	}

	if (WIFEXITED(status))
		result.exitCode = WEXITSTATUS(status);
	else
		result.exitCode = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);

	if (reported == sizeof(execError))
	{
		throw nsmb::exception("Could not run " + describe(argv) + ": "
			+ std::strerror(execError) + ".");
	}
#endif

	return result;
}

bool gitAvailable()
{
	try
	{
		return run({ "git", "--version" }).ok();
	}
	catch (const std::exception&)
	{
		return false;
	}
}

} // namespace nsmb
