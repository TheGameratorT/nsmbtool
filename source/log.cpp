#include "log.hpp"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <exception>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace nsmb::log {

namespace {

bool g_color = true;
bool g_ncpStyle = false;
int g_ncpIndent = 14;

// Same width std::stoi would refuse silently on: an unset or malformed
// NCPATCHER_LOG_INDENT falls back to NCPatcher's own default rather than
// producing a negative or huge indent.
int parseIndent(const char* value)
{
	if (value == nullptr || *value == '\0')
		return 14;
	try
	{
		const int parsed = std::stoi(value);
		return parsed > 0 ? parsed : 14;
	}
	catch (const std::exception&)
	{
		return 14;
	}
}

} // namespace

void setColorEnabled(bool enabled)
{
	g_color = enabled;
}

bool colorEnabled()
{
	return g_color;
}

void adoptNcpatcherStyleFromEnvironment()
{
	const char* style = std::getenv("NCPATCHER_LOG_STYLE");
	g_ncpStyle = style != nullptr && std::string(style) == "ncp";
	if (g_ncpStyle)
		g_ncpIndent = parseIndent(std::getenv("NCPATCHER_LOG_INDENT"));
}

std::string paint(std::string text)
{
	if (g_color)
		return text;

	// Only CSI sequences are ever emitted here, and every one of them ends in a
	// letter, so the terminator does not need a table.
	std::string out;
	out.reserve(text.size());
	for (std::size_t i = 0; i < text.size(); i++)
	{
		if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[')
		{
			i += 2;
			while (i < text.size() && !std::isalpha(static_cast<unsigned char>(text[i])))
				i++;
			continue;
		}
		out.push_back(text[i]);
	}
	return out;
}

void info(const std::string& message)
{
	if (g_ncpStyle)
	{
		std::cout << std::string(std::size_t(g_ncpIndent), ' ') << paint(message) << std::endl;
		return;
	}
	std::cout << paint(ANSI_bBLUE "[" ANSI_bWHITE "Info" ANSI_bBLUE "]" ANSI_RESET " " + message) << std::endl;
}

void warn(const std::string& message)
{
	if (g_ncpStyle)
	{
		std::cerr << paint(ANSI_bYELLOW "warning: " ANSI_RESET + message) << std::endl;
		return;
	}
	std::cerr << paint(ANSI_bYELLOW "[" ANSI_bWHITE "Warn" ANSI_bYELLOW "]" ANSI_RESET " " + message) << std::endl;
}

void error(const std::string& message)
{
	if (g_ncpStyle)
	{
		std::cerr << paint(ANSI_bRED "error: " ANSI_RESET + message) << std::endl;
		return;
	}
	std::cerr << paint(ANSI_bRED "[" ANSI_bWHITE "Error" ANSI_bRED "]" ANSI_RESET " " + message) << std::endl;
}

void out(const std::string& text)
{
	std::cout << paint(text) << std::endl;
}

} // namespace nsmb::log
