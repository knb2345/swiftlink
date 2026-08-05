// Filename sanitisation for received files.
//
// THREAT
// ------
// The filename in a START packet is attacker-controlled. A server that joins it
// to an output directory without checking will happily write wherever it is
// told:
//
//     "../../../../home/user/.ssh/authorized_keys"
//     "/etc/cron.d/backdoor"
//     "....//....//etc/passwd"          (defeats naive ".." substring removal)
//
// This is CWE-22, path traversal, and it is the single most likely way a file
// transfer server gets compromised.
//
// APPROACH
// --------
// Allowlist, not denylist. Rather than trying to enumerate dangerous inputs --
// a game the attacker wins, because encoding tricks are endless -- the name is
// reduced to its final path component and then required to match a narrow set
// of permitted characters.
//
// Two different treatments, deliberately:
//
//   * Directory components are STRIPPED. "../../etc/passwd" becomes "passwd"
//     and is accepted. This matches what scp and `unzip -j` do, and it is what
//     makes the result structurally incapable of escaping the output
//     directory. The stripping happens exactly once, and the result is then
//     required to contain no separator at all -- repeatedly stripping is what
//     lets "....//" style inputs through.
//
//   * Disallowed characters are REJECTED, not substituted. Silently rewriting
//     a name means the file that lands is not the file that was asked for, and
//     a caller cannot tell the difference.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swiftlink::transfer {

enum class FilenameRejection : std::uint8_t {
  kNone = 0,
  kEmpty,
  kTooLong,
  kPathSeparator,     // contained '/' after taking the basename (or was all separators)
  kDotDot,            // "." or ".."
  kControlCharacter,  // embedded NUL or other control byte
  kDisallowedCharacter,
  kLeadingDash,       // would be parsed as an option by downstream tooling
};

[[nodiscard]] std::string_view to_string(FilenameRejection rejection) noexcept;

// Longest accepted name. Well under NAME_MAX (255 on ext4) so that any suffix
// the server adds still fits.
inline constexpr std::size_t kMaxFilenameLength = 200;

// Reduces `raw` to a safe bare filename, or reports why it cannot.
// On success `out` never contains a path separator, so joining it to an output
// directory cannot escape that directory.
[[nodiscard]] FilenameRejection sanitize_filename(std::string_view raw,
                                                  std::string& out);

// Joins a sanitised filename to a directory.
[[nodiscard]] std::string join_path(std::string_view directory,
                                    std::string_view sanitized_name);

}  // namespace swiftlink::transfer
