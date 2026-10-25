#include "xdelta.hpp"

#include <filesystem>

#include "except.hpp"
#include "log.hpp"
#include "process.hpp"
#include "project.hpp"
#include "unicode.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

// The last non-empty line of the encoder's complaint. xdelta3 opens with a
// banner-like preamble on some failures, and the line that says what went
// wrong is the final one.
std::string lastLine(std::string text)
{
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
		text.pop_back();

	const std::size_t newline = text.find_last_of('\n');
	return newline == std::string::npos ? text : text.substr(newline + 1);
}

} // namespace

int xdeltaWrite(const XdeltaOptions& options)
{
	const fs::path start = options.directory.empty()
		? fs::current_path() : utf8ToPath(options.directory);

	if (!fs::is_directory(start))
		throw nsmb::exception(pathToUtf8Generic(start) + " is not a directory.");

	if (options.source.empty() || options.target.empty() || options.out.empty())
	{
		throw nsmb::exception(ANSI_bCYAN "xdelta" ANSI_RESET " needs " ANSI_bCYAN "--source"
			ANSI_RESET ", " ANSI_bCYAN "--target" ANSI_RESET " and " ANSI_bCYAN "--out" ANSI_RESET ".");
	}

	// Relative paths are the project's, as for stamp, so the answer does not
	// depend on which directory the hook happened to run from.
	const Project project = findProject(start);

	const auto resolve = [&](const std::string& text) {
		const fs::path path = utf8ToPath(text);
		return path.is_absolute() ? path : project.root / path;
	};

	const fs::path source = resolve(options.source);
	const fs::path target = resolve(options.target);
	const fs::path out = resolve(options.out);

	if (!fs::is_regular_file(source))
		throw nsmb::exception("The source ROM " + pathToUtf8Generic(source) + " does not exist.");
	if (!fs::is_regular_file(target))
		throw nsmb::exception("The target ROM " + pathToUtf8Generic(target) + " does not exist.");

	std::error_code error;
	fs::create_directories(out.parent_path(), error);
	if (error)
		throw nsmb::exception("Could not create " + pathToUtf8Generic(out.parent_path()) + ".");

	// -f because the file from the previous build is expected to be there.
	ProcessResult result;
	try
	{
		result = run({ "xdelta3", "-e", "-f", "-s", pathToUtf8(source), pathToUtf8(target),
			pathToUtf8(out) }, project.root);
	}
	catch (const nsmb::exception&)
	{
		throw nsmb::exception("Could not run " ANSI_bCYAN "xdelta3" ANSI_RESET
			". It has to be installed and on PATH.");
	}

	if (!result.ok())
	{
		// A failed encode can leave a truncated patch behind, which would look
		// like a good one to anyone who did not read the build log.
		fs::remove(out, error);

		const std::string reason = lastLine(result.err);
		throw nsmb::exception("xdelta3 failed" + (reason.empty() ? std::string() : ": " + reason)
			+ " (exit code " + std::to_string(result.exitCode) + ").");
	}

	log::info("Wrote " ANSI_bWHITE + pathToUtf8Generic(fs::relative(out, project.root, error))
		+ ANSI_RESET ".");
	return 0;
}

} // namespace nsmb
