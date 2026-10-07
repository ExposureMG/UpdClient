#pragma once

#include <core/error.hpp>
#include <core/export.hpp>

#include <string>
#include <string_view>

// Console paths (section 4.1 of docs/XBDM_PROTOCOL.md) are "DRIVE:\folder\file":
// a drive name of letters and digits, a colon, then names separated by single
// backslashes. A drive root is "DRIVE:\". The '/'-separated form used by hosts and
// file managers is "/DRIVE/folder/file", with the drives as top-level folders.
//
// The console understands no escape inside a quoted value, so a name is refused,
// before anything is sent, when it holds a character that could end the value or
// the command line, or that the console's file systems do not store:
//   - empty, ".", "..";
//   - '"', '\', '/', ':', '*', '?', '<', '>', '|';
//   - a control character (below 0x20) or a byte above 0x7E.
// Spaces, leading and trailing dots and spaces, and every other printable ASCII
// character are kept as they are. Names are not case-folded: the console's own
// comparison rules (FATX ignores case) decide what clashes.

namespace updclient::xbdm {

// One file or folder name.
UPDCLIENT_API Result<void> validateName(std::string_view name);
// A drive name: 1..35 ASCII letters and digits, without colon or backslash.
UPDCLIENT_API Result<void> validateDriveName(std::string_view drive);

// Checks a console path and returns it in canonical form: "DRIVE:" alone gains its
// backslash, one trailing backslash after a name is dropped. Everything else that
// differs from the canonical form (empty names, '/' separators, no drive) is an
// InvalidArgument error.
UPDCLIENT_API Result<std::string> canonicalPath(std::string_view path);

// "/HDD/dir/file" -> "HDD:\dir\file"; "/HDD" and "/HDD/" -> "HDD:\". The leading
// '/' is required, empty names ("//") are refused, one trailing '/' is accepted.
UPDCLIENT_API Result<std::string> toConsolePath(std::string_view slashPath);
// "HDD:\dir\file" -> "/HDD/dir/file"; "HDD:\" -> "/HDD". Takes what canonicalPath
// accepts.
UPDCLIENT_API Result<std::string> fromConsolePath(std::string_view consolePath);

UPDCLIENT_API bool isDriveRoot(std::string_view consolePath) noexcept;
// "HDD" for "HDD:\a\b".
UPDCLIENT_API Result<std::string> driveOf(std::string_view consolePath);
// "HDD:\a" for "HDD:\a\b", "HDD:\" for "HDD:\a". A drive root has no parent.
UPDCLIENT_API Result<std::string> parentOf(std::string_view consolePath);
// "b" for "HDD:\a\b". A drive root has no name.
UPDCLIENT_API Result<std::string> nameOf(std::string_view consolePath);
// "HDD:\a" + "b" -> "HDD:\a\b"; "HDD:\" + "b" -> "HDD:\b".
UPDCLIENT_API Result<std::string> joinPath(std::string_view directory, std::string_view name);

// A value as it goes on a command line: the text between double quotes. Refuses
// '"', CR, LF, NUL, every other control character and bytes above 0x7E.
UPDCLIENT_API Result<std::string> quoteValue(std::string_view value);

} // namespace updclient::xbdm
