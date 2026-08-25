#pragma once

// Output, and the small amount of colour that goes with it.
//
// The colour macros are spelled the same way NCPatcher spells them, and for the
// same reason: the two programs run one after the other in the same terminal,
// often in the same build, and a path highlighted one way by one of them and
// another way by the other reads as two unrelated tools rather than two halves
// of one toolchain.

#include <iostream>
#include <string>

#define ANSI_RESET "\x1b[0m"
#define ANSI_RED "\x1b[31m"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_CYAN "\x1b[36m"
#define ANSI_bRED "\x1b[31;1m"
#define ANSI_bGREEN "\x1b[32;1m"
#define ANSI_bYELLOW "\x1b[33;1m"
#define ANSI_bBLUE "\x1b[34;1m"
#define ANSI_bCYAN "\x1b[36;1m"
#define ANSI_bWHITE "\x1b[37;1m"

namespace nsmb::log {

// Whether escape codes are written at all. Decided once, from whether stdout is
// a terminal and whether NO_COLOR is set, so that a redirected log is plain text
// rather than a file full of bracket sequences.
void setColorEnabled(bool enabled);
[[nodiscard]] bool colorEnabled();

// Strips escape sequences when colour is off. Everything below funnels through
// this, so a message can be written with the macros unconditionally.
[[nodiscard]] std::string paint(std::string text);

void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

// Plain, unprefixed, on stdout: the output of `path` and `list`, which exist to
// be read by another program as much as by a person.
void out(const std::string& text);

} // namespace nsmb::log
