#include "log.hpp"

#include <cctype>
#include <cstddef>
#include <cstdlib>

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

} // namespace

void setColorEnabled(bool enabled)
{
	g_color = enabled;
}

bool colorEnabled()
{
	return g_color;
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
	std::cout << paint(ANSI_bBLUE "[" ANSI_bWHITE "Info" ANSI_bBLUE "]" ANSI_RESET " " + message) << std::endl;
}

void warn(const std::string& message)
{
	std::cerr << paint(ANSI_bYELLOW "[" ANSI_bWHITE "Warn" ANSI_bYELLOW "]" ANSI_RESET " " + message) << std::endl;
}

void error(const std::string& message)
{
	std::cerr << paint(ANSI_bRED "[" ANSI_bWHITE "Error" ANSI_bRED "]" ANSI_RESET " " + message) << std::endl;
}

void out(const std::string& text)
{
	std::cout << paint(text) << std::endl;
}

} // namespace nsmb::log
