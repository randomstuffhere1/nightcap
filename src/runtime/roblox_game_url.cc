#include "runtime/roblox_game_url.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumGameUrlBytes = 64 * 1024;
constexpr std::size_t kMaximumQueryBytes = 16 * 1024;
constexpr std::size_t kMaximumQueryParameters = 64;
constexpr std::size_t kMaximumDecodedQueryBytes = 16 * 1024;
constexpr std::size_t kMaximumGameNameBytes = 256;
constexpr std::size_t kMaximumLinkCodeBytes = 128;

std::string AsciiLower(std::string_view value) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return normalized;
}

bool ContainsControlBytes(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte < 0x20 || byte == 0x7f;
  });
}

bool IsValidUtf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7f) {
      ++index;
      continue;
    }
    std::size_t continuation_count = 0;
    uint32_t code_point = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation_count = 1;
      code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation_count = 2;
      code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation_count = 3;
      code_point = first & 0x07;
    } else {
      return false;
    }
    if (index + continuation_count >= value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const unsigned char continuation =
          static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

int HexDigitValue(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

bool PercentDecode(std::string_view encoded, bool plus_as_space,
                   std::string* decoded) {
  if (decoded == nullptr || encoded.size() > kMaximumDecodedQueryBytes) {
    return false;
  }
  decoded->clear();
  decoded->reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    unsigned char value = static_cast<unsigned char>(encoded[index]);
    if (encoded[index] == '%') {
      if (index + 2 >= encoded.size()) return false;
      const int high = HexDigitValue(encoded[index + 1]);
      const int low = HexDigitValue(encoded[index + 2]);
      if (high < 0 || low < 0) return false;
      value = static_cast<unsigned char>((high << 4) | low);
      index += 2;
    } else if (encoded[index] == '+' && plus_as_space) {
      value = ' ';
    }
    if (value < 0x20 || value == 0x7f) return false;
    decoded->push_back(static_cast<char>(value));
  }
  return decoded->size() <= kMaximumDecodedQueryBytes &&
         IsValidUtf8(*decoded);
}

class ScopedCurlUrl final {
 public:
  ScopedCurlUrl() : handle_(curl_url()) {}
  ~ScopedCurlUrl() {
    if (handle_ != nullptr) curl_url_cleanup(handle_);
  }

  ScopedCurlUrl(const ScopedCurlUrl&) = delete;
  ScopedCurlUrl& operator=(const ScopedCurlUrl&) = delete;

  CURLU* get() const { return handle_; }

 private:
  CURLU* handle_ = nullptr;
};

class ScopedCurlString final {
 public:
  ScopedCurlString() = default;
  ~ScopedCurlString() { curl_free(value_); }

  ScopedCurlString(const ScopedCurlString&) = delete;
  ScopedCurlString& operator=(const ScopedCurlString&) = delete;

  char** output() { return &value_; }
  const char* get() const { return value_; }

 private:
  char* value_ = nullptr;
};

bool GetUrlPart(CURLU* url, CURLUPart part, unsigned int flags,
                std::string* output) {
  ScopedCurlString value;
  return url != nullptr && output != nullptr &&
         curl_url_get(url, part, value.output(), flags) == CURLUE_OK &&
         value.get() != nullptr &&
         (output->assign(value.get()), true);
}

bool HasUrlPart(CURLU* url, CURLUPart part) {
  ScopedCurlString value;
  return url != nullptr &&
         curl_url_get(url, part, value.output(), 0) == CURLUE_OK;
}

bool ParsePositiveId(std::string_view value, int64_t* output) {
  if (output == nullptr || value.empty() || value.size() > 19 ||
      value.front() == '0') {
    return false;
  }
  int64_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
      parsed <= 0) {
    return false;
  }
  *output = parsed;
  return true;
}

bool IsBoundedToken(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= '0' && character <= '9';
         });
}

bool IsBoundedCode(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum;
}

// Reads the join selectors out of a game link query. Unrelated parameters are
// ignored, but a selector that is present and malformed rejects the link so a
// private join is never silently downgraded to a public one.
bool ReadJoinQuery(std::string_view query, std::string* link_code,
                   std::string* access_code) {
  if (query.empty()) return true;
  if (query.size() > kMaximumQueryBytes || link_code == nullptr ||
      access_code == nullptr) {
    return false;
  }
  std::size_t count = 0;
  std::size_t begin = 0;
  while (begin <= query.size()) {
    if (++count > kMaximumQueryParameters) return false;
    const std::size_t end = query.find('&', begin);
    const std::string_view pair =
        query.substr(begin, end == std::string_view::npos
                                ? query.size() - begin
                                : end - begin);
    const std::size_t equals = pair.find('=');
    if (pair.empty() || equals == std::string_view::npos || equals == 0) {
      return false;
    }
    std::string key;
    std::string value;
    if (!PercentDecode(pair.substr(0, equals), true, &key) ||
        !PercentDecode(pair.substr(equals + 1), true, &value)) {
      return false;
    }
    const std::string normalized = AsciiLower(key);
    if (normalized == "privateserverlinkcode" || normalized == "linkcode") {
      if (!link_code->empty() || !IsBoundedToken(value, kMaximumLinkCodeBytes)) {
        return false;
      }
      *link_code = std::move(value);
    } else if (normalized == "accesscode") {
      if (!access_code->empty() ||
          !IsBoundedCode(value, kMaximumLinkCodeBytes)) {
        return false;
      }
      *access_code = std::move(value);
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return true;
}

std::string DisplayNameFromSlug(std::string_view slug) {
  std::string name(slug);
  std::replace(name.begin(), name.end(), '-', ' ');
  return name;
}

struct RobloxUrlParts {
  std::string path;
  std::string query;
};

// Applies the one origin allow-list shared by every roblox.com link parser:
// https only, host roblox.com or www.roblox.com, no userinfo, no fragment and
// port 443 only. Returns the decoded path and the raw query string.
bool ReadRobloxUrlParts(const ScopedCurlUrl& parsed, RobloxUrlParts* parts) {
  if (parsed.get() == nullptr || parts == nullptr) return false;
  std::string scheme;
  std::string host;
  if (!GetUrlPart(parsed.get(), CURLUPART_SCHEME, 0, &scheme) ||
      !GetUrlPart(parsed.get(), CURLUPART_HOST, 0, &host) ||
      !GetUrlPart(parsed.get(), CURLUPART_PATH, CURLU_URLDECODE, &parts->path) ||
      AsciiLower(scheme) != "https") {
    return false;
  }
  const std::string normalized_host = AsciiLower(host);
  if ((normalized_host != "www.roblox.com" &&
       normalized_host != "roblox.com") ||
      HasUrlPart(parsed.get(), CURLUPART_USER) ||
      HasUrlPart(parsed.get(), CURLUPART_PASSWORD) ||
      HasUrlPart(parsed.get(), CURLUPART_FRAGMENT)) {
    return false;
  }
  ScopedCurlString port;
  const CURLUcode port_status =
      curl_url_get(parsed.get(), CURLUPART_PORT, port.output(), 0);
  if (port_status == CURLUE_OK && port.get() != nullptr &&
      std::string_view(port.get()) != "443") {
    return false;
  }
  ScopedCurlString raw_query;
  (void)curl_url_get(parsed.get(), CURLUPART_QUERY, raw_query.output(), 0);
  if (raw_query.get() != nullptr) parts->query = raw_query.get();
  return true;
}

// Runs the shared size/encoding gates, parses the URL and applies the origin
// allow-list, leaving `parsed` and `parts` ready for path-shape validation.
bool PrepareRobloxUrl(std::string_view url, ScopedCurlUrl* parsed,
                      RobloxUrlParts* parts) {
  if (parsed == nullptr || parts == nullptr || url.empty() ||
      url.size() > kMaximumGameUrlBytes || ContainsControlBytes(url) ||
      !IsValidUtf8(url)) {
    return false;
  }
  const std::string owned_url(url);
  if (parsed->get() == nullptr ||
      curl_url_set(parsed->get(), CURLUPART_URL, owned_url.c_str(),
                   CURLU_NON_SUPPORT_SCHEME | CURLU_PATH_AS_IS) != CURLUE_OK) {
    return false;
  }
  return ReadRobloxUrlParts(*parsed, parts);
}

// Splits a decoded roblox.com path into non-empty segments. A single trailing
// slash is harmless and is common in copied Roblox links; empty interior
// segments and double slashes remain rejected.
bool SplitRobloxPath(std::string_view path,
                     std::vector<std::string_view>* segments) {
  if (segments == nullptr) return false;
  segments->clear();
  if (path.size() >= 2 && path.back() == '/' &&
      path[path.size() - 2] == '/') {
    return false;
  }
  std::size_t begin = 1;
  while (begin < path.size()) {
    const std::size_t end = path.find('/', begin);
    const std::string_view segment = path.substr(
        begin, end == std::string_view::npos ? path.size() - begin
                                             : end - begin);
    if (segment.empty()) {
      if (end == path.size() - 1) break;
      return false;
    }
    segments->push_back(segment);
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return true;
}

bool IsShareCode(std::string_view value) {
  return value.size() == 32 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f') ||
                  (character >= 'A' && character <= 'F');
         });
}

// Reads the deferred-link selectors out of a share URL query. A `code` that is
// present but malformed rejects the link. Only type=Server names a shared
// private server; every other share flavour is a different link form and is
// left to the normal paste behavior.
bool ReadShareQuery(std::string_view query, std::string* code,
                    std::string* type) {
  if (query.empty() || code == nullptr || type == nullptr ||
      query.size() > kMaximumQueryBytes) {
    return false;
  }
  std::size_t count = 0;
  std::size_t begin = 0;
  while (begin <= query.size()) {
    if (++count > kMaximumQueryParameters) return false;
    const std::size_t end = query.find('&', begin);
    const std::string_view pair =
        query.substr(begin, end == std::string_view::npos
                                ? query.size() - begin
                                : end - begin);
    const std::size_t equals = pair.find('=');
    if (pair.empty() || equals == std::string_view::npos || equals == 0) {
      return false;
    }
    std::string key;
    std::string value;
    if (!PercentDecode(pair.substr(0, equals), true, &key) ||
        !PercentDecode(pair.substr(equals + 1), true, &value)) {
      return false;
    }
    const std::string normalized = AsciiLower(key);
    if (normalized == "code") {
      if (!code->empty() || !IsShareCode(value)) return false;
      *code = std::move(value);
    } else if (normalized == "type") {
      if (!type->empty() || value.size() > kMaximumLinkCodeBytes ||
          ContainsControlBytes(value)) {
        return false;
      }
      *type = std::move(value);
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  if (code->empty() || AsciiLower(*type) != "server") return false;
  std::transform(code->begin(), code->end(), code->begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return true;
}

}  // namespace

std::optional<RobloxGameUrlLink> ParseRobloxGameUrl(std::string_view url) {
  ScopedCurlUrl parsed;
  RobloxUrlParts parts;
  if (!PrepareRobloxUrl(url, &parsed, &parts)) return std::nullopt;

  std::vector<std::string_view> segments;
  if (!SplitRobloxPath(parts.path, &segments)) return std::nullopt;
  if (segments.size() < 2 || segments.size() > 3 ||
      segments[0] != "games") {
    return std::nullopt;
  }

  int64_t place_id = 0;
  if (!ParsePositiveId(segments[1], &place_id)) return std::nullopt;

  // A public game page has no query at all, so an absent part is not an error.
  std::string link_code;
  std::string access_code;
  if (!ReadJoinQuery(parts.query, &link_code, &access_code)) {
    return std::nullopt;
  }

  std::string fallback_game_name;
  if (segments.size() == 3) {
    if (segments[2].size() > kMaximumGameNameBytes ||
        ContainsControlBytes(segments[2]) || !IsValidUtf8(segments[2])) {
      return std::nullopt;
    }
    fallback_game_name = DisplayNameFromSlug(segments[2]);
  }
  return RobloxGameUrlLink{place_id, std::move(link_code),
                           std::move(access_code),
                           std::move(fallback_game_name)};
}

std::optional<RobloxShareLinkUrl> ParseRobloxShareUrl(std::string_view url) {
  ScopedCurlUrl parsed;
  RobloxUrlParts parts;
  if (!PrepareRobloxUrl(url, &parsed, &parts)) return std::nullopt;

  std::vector<std::string_view> segments;
  if (!SplitRobloxPath(parts.path, &segments)) return std::nullopt;
  // Roblox serves the share form at /share and redirects browsers to
  // /share-links; both name the same deferred link.
  if (segments.size() != 1 ||
      (segments[0] != "share" && segments[0] != "share-links")) {
    return std::nullopt;
  }

  std::string code;
  std::string type;
  if (!ReadShareQuery(parts.query, &code, &type)) return std::nullopt;
  return RobloxShareLinkUrl{std::move(code), std::move(type)};
}

}  // namespace runtime
}  // namespace mocktail
