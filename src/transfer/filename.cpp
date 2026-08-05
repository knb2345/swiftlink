#include "swiftlink/transfer/filename.hpp"

#include <cctype>

namespace swiftlink::transfer {

std::string_view to_string(FilenameRejection rejection) noexcept {
  switch (rejection) {
    case FilenameRejection::kNone:
      return "ok";
    case FilenameRejection::kEmpty:
      return "filename is empty";
    case FilenameRejection::kTooLong:
      return "filename is too long";
    case FilenameRejection::kPathSeparator:
      return "filename resolves to a path, not a name";
    case FilenameRejection::kDotDot:
      return "filename is '.' or '..'";
    case FilenameRejection::kControlCharacter:
      return "filename contains a control character or NUL";
    case FilenameRejection::kDisallowedCharacter:
      return "filename contains a character outside the permitted set";
    case FilenameRejection::kLeadingDash:
      return "filename begins with '-'";
  }
  return "unknown rejection";
}

namespace {

// Permitted characters: ASCII letters, digits, and a small set of punctuation
// that appears in real filenames. Everything else -- spaces, quotes, shell
// metacharacters, backslashes, non-ASCII bytes -- is refused.
//
// Refusing non-ASCII is stricter than POSIX requires (a filename is any byte
// sequence without '/' or NUL) and would need relaxing for international
// filenames. It is the right default here: the set of bytes that are safe in
// every context a filename later flows into (a shell, a log, a web page) is
// much smaller than the set the filesystem accepts.
[[nodiscard]] bool is_permitted(char c) noexcept {
  const auto byte = static_cast<unsigned char>(c);
  if (byte >= 'a' && byte <= 'z') return true;
  if (byte >= 'A' && byte <= 'Z') return true;
  if (byte >= '0' && byte <= '9') return true;
  return c == '.' || c == '-' || c == '_' || c == '+' || c == '@' || c == '=';
}

}  // namespace

FilenameRejection sanitize_filename(std::string_view raw, std::string& out) {
  out.clear();

  if (raw.empty()) {
    return FilenameRejection::kEmpty;
  }

  // Reject control bytes before anything else. An embedded NUL is the classic
  // way to make a checked string and the string a C API later sees differ.
  for (const char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      return FilenameRejection::kControlCharacter;
    }
  }

  // Take only the final path component. This is what defuses traversal: after
  // this, "../../etc/passwd" is "passwd" and "/etc/shadow" is "shadow".
  //
  // Note it is applied *once* and the result is then required to contain no
  // separator at all. Repeatedly stripping or substituting would reintroduce
  // the "....//" class of bug, where removing the inner "../" leaves a fresh
  // one behind.
  const std::size_t last_separator = raw.find_last_of('/');
  const std::string_view base = (last_separator == std::string_view::npos)
                                    ? raw
                                    : raw.substr(last_separator + 1);

  if (base.empty()) {
    // The name ended in '/', so it names a directory, not a file.
    return FilenameRejection::kPathSeparator;
  }

  if (base == "." || base == "..") {
    return FilenameRejection::kDotDot;
  }

  if (base.size() > kMaxFilenameLength) {
    return FilenameRejection::kTooLong;
  }

  // A leading '-' is not a filesystem hazard, but it turns into an option the
  // moment the name reaches a command line.
  if (base.front() == '-') {
    return FilenameRejection::kLeadingDash;
  }

  for (const char c : base) {
    if (c == '/') {
      return FilenameRejection::kPathSeparator;  // unreachable, but explicit
    }
    if (!is_permitted(c)) {
      return FilenameRejection::kDisallowedCharacter;
    }
  }

  out.assign(base);
  return FilenameRejection::kNone;
}

std::string join_path(std::string_view directory,
                      std::string_view sanitized_name) {
  std::string path(directory);
  if (!path.empty() && path.back() != '/') {
    path.push_back('/');
  }
  path.append(sanitized_name);
  return path;
}

}  // namespace swiftlink::transfer
