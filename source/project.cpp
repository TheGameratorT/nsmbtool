#include "project.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "except.hpp"
#include "unicode.hpp"
#include "lockfile.hpp"
#include "log.hpp"
#include "process.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

constexpr std::string_view ENV_NAME = ".ncpatcher.env";

// The one variable this tool manages, and the markers around the lines it owns.
// Everything outside them belongs to whoever wrote it.
constexpr std::string_view ENV_VAR = "NSMBREF_ROOT";
constexpr std::string_view BLOCK_BEGIN = "# >>> nsmbtool";
constexpr std::string_view BLOCK_END = "# <<< nsmbtool";

constexpr std::size_t NOT_FOUND = std::size_t(-1);

const char* const PROJECT_FILES[] = { "ncpatcher.yaml", "ncpatcher.yml", "ncpatcher.json" };

// The value as NCPatcher's own .env reader will see it. That reader trims and
// then strips one layer of matching quotes, so a value with edge whitespace has
// to be quoted to survive -- and a value that is already quote-wrapped would be
// unwrapped, which no real path is, but which is worth not producing.
std::string envValue(const std::string& value)
{
	const bool padded = !value.empty()
		&& (std::isspace(static_cast<unsigned char>(value.front()))
			|| std::isspace(static_cast<unsigned char>(value.back())));
	const bool quoted = value.size() >= 2 && value.front() == value.back()
		&& (value.front() == '"' || value.front() == '\'');

	if (!padded && !quoted)
		return value;
	if (value.find('"') == std::string::npos)
		return "\"" + value + "\"";
	if (value.find('\'') == std::string::npos)
		return "'" + value + "'";

	throw nsmb::exception("The reference path contains both kinds of quote and cannot be written "
	                    "to " + std::string(ENV_NAME) + ": " + value);
}

std::string readFile(const fs::path& file)
{
	std::ifstream stream(file, std::ios::binary);
	if (!stream.is_open())
		return {};

	std::ostringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

std::vector<std::string> splitLines(const std::string& text)
{
	std::vector<std::string> lines;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line))
		lines.push_back(line);
	return lines;
}

std::string_view trimmed(std::string_view text)
{
	const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
	while (!text.empty() && isSpace(text.front()))
		text.remove_prefix(1);
	while (!text.empty() && isSpace(text.back()))
		text.remove_suffix(1);
	return text;
}

// Whether the line assigns the managed variable, read the way NCPatcher reads
// it: trimmed, and split at the first '='.
bool assignsEnvVar(std::string_view line)
{
	const std::string_view text = trimmed(line);
	if (text.empty() || text.front() == '#')
		return false;

	const std::size_t separator = text.find('=');
	return separator != std::string_view::npos && trimmed(text.substr(0, separator)) == ENV_VAR;
}

bool isMarker(std::string_view line, std::string_view marker)
{
	return trimmed(line).starts_with(marker);
}

// The first and last line of the managed block, both inclusive, or NOT_FOUND
// for a file that has none. An opening marker with no closing one is refused
// rather than guessed at: the block would have to swallow everything below it,
// which is the rest of someone's file.
std::pair<std::size_t, std::size_t> findBlock(const fs::path& file,
	const std::vector<std::string>& lines)
{
	for (std::size_t i = 0; i < lines.size(); i++)
	{
		if (!isMarker(lines[i], BLOCK_BEGIN))
			continue;

		for (std::size_t j = i + 1; j < lines.size(); j++)
		{
			if (isMarker(lines[j], BLOCK_END))
				return { i, j };
		}

		throw nsmb::exception(pathToUtf8Generic(file) + " has an opening \"" + std::string(BLOCK_BEGIN)
			+ "\" marker with no closing \"" + std::string(BLOCK_END)
			+ "\" one. Restore it, or delete the marker and let sync write the block again.");
	}

	return { NOT_FOUND, NOT_FOUND };
}

} // namespace

fs::path Project::lockPath() const
{
	return root / Lockfile::NAME;
}

fs::path Project::envPath() const
{
	return root / ENV_NAME;
}

Project findProject(const fs::path& start)
{
	std::error_code error;
	fs::path directory = fs::absolute(start, error);
	if (error)
		directory = start;
	directory = directory.lexically_normal();

	for (fs::path at = directory; ; at = at.parent_path())
	{
		if (fs::is_regular_file(at / Lockfile::NAME, error))
			return Project{ at, true };
		if (!at.has_relative_path())
			break;
	}

	for (fs::path at = directory; ; at = at.parent_path())
	{
		for (const char* name : PROJECT_FILES)
		{
			if (fs::is_regular_file(at / name, error))
				return Project{ at, false };
		}
		if (!at.has_relative_path())
			break;
	}

	return Project{ directory, false };
}

bool writeEnvFile(const fs::path& file, const fs::path& referenceRoot,
	const std::string& repo, const std::string& rev)
{
	const std::vector<std::string> lines = splitLines(readFile(file));

	const std::vector<std::string> block = {
		std::string(BLOCK_BEGIN) + " (managed block, rewritten by `nsmbtool reference sync`)",
		"# The lines between these markers come from " + std::string(Lockfile::NAME) + "; run",
		"#     nsmbtool reference sync",
		"# after changing it. The rest of this file is yours and is left alone.",
		"#",
		"# NCPatcher reads this file before ${env.*} resolves, and what is here overrides",
		"# the ambient environment, which is the point: a global " + std::string(ENV_VAR) + " in a",
		"# shell profile must not decide what this project builds against.",
		"#",
		"#   reference " + repo,
		"#   revision  " + rev,
		std::string(ENV_VAR) + "=" + envValue(pathToUtf8Generic(referenceRoot)),
		std::string(BLOCK_END)
	};

	const auto [blockBegin, blockEnd] = findBlock(file, lines);

	// Everything is kept except the previous block and any assignment of the
	// managed variable outside it. Those go because the reader takes the last
	// assignment of a name: one left below the block would quietly win over it.
	// The first of them says where the block belongs, on the reasoning that
	// whoever wrote the line by hand put it where they wanted it.
	std::vector<std::string> out;
	std::size_t insertAt = NOT_FOUND;

	for (std::size_t i = 0; i < lines.size(); i++)
	{
		if (i == blockBegin)
		{
			insertAt = out.size();
			i = blockEnd;
			continue;
		}

		if (assignsEnvVar(lines[i]))
		{
			if (insertAt == NOT_FOUND)
				insertAt = out.size();
			continue;
		}

		out.push_back(lines[i]);
	}

	if (insertAt == NOT_FOUND)
	{
		if (!out.empty() && !trimmed(out.back()).empty())
			out.emplace_back();
		insertAt = out.size();
	}

	out.insert(out.begin() + std::ptrdiff_t(insertAt), block.begin(), block.end());

	std::string text;
	for (const std::string& line : out)
	{
		text += line;
		text += '\n';
	}

	if (readFile(file) == text)
		return false;

	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
		throw nsmb::exception("Could not write " + pathToUtf8Generic(file) + ".");
	stream.write(text.data(), std::streamsize(text.size()));
	if (!stream)
		throw nsmb::exception("Could not write " + pathToUtf8Generic(file) + ".");

	return true;
}

EnvFileStanding envFileStanding(const fs::path& projectRoot)
{
	// Asked of git rather than parsed out of .gitignore: the rule may live in a
	// parent directory, in .git/info/exclude, or in the user's global ignore
	// file, and only git knows about all three. A directory that is not a
	// repository has nothing to commit the file to, so it counts as covered.
	const ProcessResult inside = run({ "git", "rev-parse", "--is-inside-work-tree" }, projectRoot);
	if (!inside.ok())
		return EnvFileStanding::Ignored;

	// Tracked first, and it is the case worth separating: check-ignore says
	// nothing is ignored about a file already in the index, which reads as "not
	// ignored" when the real answer is "ignoring it will not help any more".
	if (run({ "git", "ls-files", "--error-unmatch", "--", std::string(ENV_NAME) }, projectRoot).ok())
		return EnvFileStanding::Tracked;

	if (run({ "git", "check-ignore", "--quiet", std::string(ENV_NAME) }, projectRoot).ok())
		return EnvFileStanding::Ignored;

	return EnvFileStanding::Uncovered;
}

} // namespace nsmb
