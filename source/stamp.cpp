#include "stamp.hpp"

#include <charconv>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "except.hpp"
#include "log.hpp"
#include "process.hpp"
#include "project.hpp"
#include "unicode.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

std::string trimmed(std::string text)
{
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
		text.pop_back();
	return text;
}

} // namespace

std::string formatStamp(const std::string& shortHash, long long seconds)
{
	const std::time_t when = static_cast<std::time_t>(seconds);

	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &when);
#else
	gmtime_r(&when, &utc);
#endif

	char date[32];
	const std::size_t length = std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &utc);
	if (length == 0)
		throw nsmb::exception("The commit date does not fit a timestamp.");

	return shortHash + " " + std::string(date, length);
}

int stampWrite(const StampOptions& options)
{
	const fs::path start = options.directory.empty()
		? fs::current_path() : utf8ToPath(options.directory);

	if (!fs::is_directory(start))
		throw nsmb::exception(pathToUtf8Generic(start) + " is not a directory.");

	if (options.out.empty())
	{
		throw nsmb::exception(ANSI_bCYAN "stamp" ANSI_RESET " needs " ANSI_bCYAN "--out"
			ANSI_RESET ": there is no default place for a build identifier to go.");
	}

	// Anchored to the project rather than to the current directory, because the
	// revision being stamped is the project's. A hook already runs at the root,
	// but `nsmbtool stamp` typed in a module directory should not quietly stamp
	// whatever repository happens to enclose it.
	const Project project = findProject(start);

	const fs::path outPath = utf8ToPath(options.out).is_absolute()
		? utf8ToPath(options.out) : project.root / utf8ToPath(options.out);

	std::string stamp;

	const ProcessResult hash = gitAvailable()
		? run({ "git", "rev-parse", "--short", "HEAD" }, project.root)
		: ProcessResult{};
	const ProcessResult date = hash.ok()
		? run({ "git", "log", "-1", "--format=%ct" }, project.root)
		: ProcessResult{};

	if (hash.ok() && date.ok())
	{
		const std::string seconds = trimmed(date.out);

		long long epoch = 0;
		const char* first = seconds.data();
		const char* last = first + seconds.size();
		if (std::from_chars(first, last, epoch).ec != std::errc{} )
			throw nsmb::exception("git reported a commit date this cannot read: " + seconds + ".");

		stamp = formatStamp(trimmed(hash.out), epoch);
	}
	else
	{
		// The script this replaces stamped `unknown` silently. Building outside
		// a repository is a real thing to do -- a source archive has no git
		// history -- so it is not an error, but the crash screen is the only
		// place this string is ever read, and a build that cannot be identified
		// from it should say so while there is still someone watching.
		log::warn("No git revision here, so the build stamp will not identify this build.");
		stamp = formatStamp("unknown", static_cast<long long>(std::time(nullptr)));
	}

	// No trailing newline. The game reads this as a C string from a fixed
	// address and prints it on one line of the crash screen; a newline would be
	// part of what it printed.
	std::error_code error;
	fs::create_directories(outPath.parent_path(), error);

	std::ifstream existing(outPath, std::ios::binary);
	if (existing.is_open())
	{
		const std::string previous(
			(std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
		if (previous == stamp)
		{
			log::info("Build stamp " ANSI_bWHITE + stamp + ANSI_RESET " was already written.");
			return 0;
		}
	}
	existing.close();

	std::ofstream stream(outPath, std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
		throw nsmb::exception("Could not write " + pathToUtf8Generic(outPath) + ".");

	stream << stamp;
	if (!stream)
		throw nsmb::exception("Could not write " + pathToUtf8Generic(outPath) + ".");

	log::info("Build stamp " ANSI_bWHITE + stamp + ANSI_RESET ".");
	return 0;
}

} // namespace nsmb
