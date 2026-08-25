#pragma once

// One text encoding, everywhere inside this program: UTF-8.
//
// That is free on Linux and macOS, where the filesystem is bytes and the bytes
// are UTF-8 already. On Windows it takes deliberate work, because the platform
// has two of every API and the narrow half of each pair speaks the machine's
// ANSI code page rather than Unicode. A user called Jos\u00e9 has a store under
// C:\Users\Jos\u00e9\AppData\Local, and a program that lets that path through the
// narrow APIs either mangles it or refuses it -- on the one machine where the
// user cannot simply rename themselves.
//
// So paths become strings through pathToUtf8 rather than path::string(), which
// on MSVC would encode to the ANSI code page and lose the character before this
// code ever sees it. Conversion to UTF-16 happens once, at the boundary where a
// string is handed to Windows.

#include <filesystem>
#include <string>
#include <string_view>

namespace nsmb {

// The path as UTF-8, whatever the platform stores natively.
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);

// The same, with '/' separators, for text this program writes for others to
// read: .ncpatcher.env, projects.txt, log lines.
[[nodiscard]] std::string pathToUtf8Generic(const std::filesystem::path& path);

// Back the other way, for text this program reads.
[[nodiscard]] std::filesystem::path utf8ToPath(std::string_view text);

#ifdef _WIN32
[[nodiscard]] std::wstring toWide(std::string_view utf8);
[[nodiscard]] std::string toUtf8(std::wstring_view wide);
#endif

} // namespace nsmb
