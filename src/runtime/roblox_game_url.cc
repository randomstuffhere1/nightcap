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

}  // namespace

std::optional<RobloxGameUrlLink> ParseRobloxGameUrl(std::string_view url) {
  if (url.empty() || url.size() > kMaximumGameUrlBytes ||
      ContainsControlBytes(url) || !IsValidUtf8(url)) {
    return std::nullopt;
  }

  const ScopedCurlUrl parsed;
  const std::string owned_url(url);
  if (parsed.get() == nullptr ||
      curl_url_set(parsed.get(), CURLUPART_URL, owned_url.c_str(),
                   CURLU_NON_SUPPORT_SCHEME | CURLU_PATH_AS_IS) != CURLUE_OK) {
    return std::nullopt;
  }

  std::string scheme;
  std::string host;
  std::string path;
  if (!GetUrlPart(parsed.get(), CURLUPART_SCHEME, 0, &scheme) ||
      !GetUrlPart(parsed.get(), CURLUPART_HOST, 0, &host) ||
      !GetUrlPart(parsed.get(), CURLUPART_PATH, CURLU_URLDECODE, &path) ||
      AsciiLower(scheme) != "https") {
    return std::nullopt;
  }
  const std::string normalized_host = AsciiLower(host);
  if ((normalized_host != "www.roblox.com" && normalized_host != "roblox.com") ||
      HasUrlPart(parsed.get(), CURLUPART_USER) ||
      HasUrlPart(parsed.get(), CURLUPART_PASSWORD) ||
      HasUrlPart(parsed.get(), CURLUPART_FRAGMENT)) {
    return std::nullopt;
  }
  ScopedCurlString port;
  const CURLUcode port_status =
      curl_url_get(parsed.get(), CURLUPART_PORT, port.output(), 0);
  if (port_status == CURLUE_OK && port.get() != nullptr &&
      std::string_view(port.get()) != "443") {
    return std::nullopt;
  }

  const std::string_view path_view(path);
  if (path_view.size() >= 2 && path_view.back() == '/' &&
      path_view[path_view.size() - 2] == '/') {
    return std::nullopt;
  }
  std::vector<std::string_view> segments;
  std::size_t begin = 1;
  while (begin < path_view.size()) {
    const std::size_t end = path_view.find('/', begin);
    const std::string_view segment = path_view.substr(
        begin, end == std::string_view::npos ? path_view.size() - begin
                                             : end - begin);
    if (segment.empty()) {
      // A single trailing slash is harmless and is common in copied Roblox
      // links; double slashes and empty interior segments remain rejected.
      if (end == path_view.size() - 1) break;
      return std::nullopt;
    }
    segments.push_back(segment);
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  if (segments.size() < 2 || segments.size() > 3 ||
      segments[0] != "games") {
    return std::nullopt;
  }

  int64_t place_id = 0;
  if (!ParsePositiveId(segments[1], &place_id)) return std::nullopt;

  // A public game page has no query at all, so an absent part is not an error.
  ScopedCurlString raw_query;
  (void)curl_url_get(parsed.get(), CURLUPART_QUERY, raw_query.output(), 0);
  std::string link_code;
  std::string access_code;
  const std::string_view query = raw_query.get() != nullptr
                                     ? std::string_view(raw_query.get())
                                     : std::string_view();
  if (!ReadJoinQuery(query, &link_code, &access_code)) return std::nullopt;

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

}  // namespace runtime
}  // namespace mocktail
