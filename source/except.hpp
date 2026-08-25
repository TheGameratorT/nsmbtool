#pragma once

// One exception type. Everything this program can fail at is a message for the
// person who ran it -- there is no failure it recovers from by inspecting the
// kind, so a hierarchy would only be ceremony.

#include <stdexcept>
#include <string>

namespace nsmb {

class exception : public std::runtime_error
{
public:
	explicit exception(const std::string& reason) : std::runtime_error(reason) {}
};

} // namespace nsmb
