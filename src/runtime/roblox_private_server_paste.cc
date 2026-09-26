#include "runtime/roblox_private_server_paste.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/environment.h"
#include "runtime/failure_dialog.h"
#include "runtime/system_proxy.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumGameUrlBytes = 64 * 1024;
constexpr std::size_t kMaximumQueryBytes = 16 * 1024;
constexpr std::size_t kMaximumQueryParameters = 64;
constexpr std::size_t kMaximumDecodedQueryBytes = 16 * 1024;
constexpr std::size_t kMaximumGameNameBytes = 256;
constexpr std::size_t kMaximumLinkCodeBytes = 128;
constexpr std::size_t kMaximumGameResponseBytes = 512 * 1024;
constexpr const char* kGamesEndpoint =
    "https://games.roblox.com/v1/games?universeIds=";

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

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

std::string DisplayNameFromSlug(std::string_view slug) {
  std::string name(slug);
  std::replace(name.begin(), name.end(), '-', ' ');
  return name;
}

bool ParsePrivateServerQuery(std::string_view query, std::string* link_code) {
  if (query.empty() || query.size() > kMaximumQueryBytes ||
      link_code == nullptr) {
    return false;
  }
  std::size_t count = 0;
  std::size_t begin = 0;
  bool found = false;
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
    if (AsciiLower(key) == "privateserverlinkcode") {
      if (found || value.empty() || value.size() > kMaximumLinkCodeBytes ||
          !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= '0' && character <= '9';
          })) {
        return false;
      }
      *link_code = std::move(value);
      found = true;
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return found;
}

struct BoundedResponse {
  std::string body;
  bool exceeded = false;
};

std::size_t WriteHttpResponse(char* data, std::size_t size,
                              std::size_t count, void* user_data) {
  auto* response = static_cast<BoundedResponse*>(user_data);
  if (response == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<std::size_t>::max() / count)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > kMaximumGameResponseBytes ||
      response->body.size() > kMaximumGameResponseBytes - bytes) {
    response->exceeded = true;
    return 0;
  }
  response->body.append(data, bytes);
  return bytes;
}

bool ReadPositiveJsonId(const nlohmann::json& object, const char* key,
                        int64_t* output) {
  if (!object.is_object() || output == nullptr) return false;
  const auto found = object.find(key);
  if (found == object.end()) return false;
  if (found->is_number_unsigned()) {
    const uint64_t value = found->get<uint64_t>();
    if (value == 0 ||
        value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return false;
    }
    *output = static_cast<int64_t>(value);
    return true;
  }
  if (found->is_number_integer()) {
    const int64_t value = found->get<int64_t>();
    if (value <= 0) return false;
    *output = value;
    return true;
  }
  return false;
}

bool SafeGameName(const nlohmann::json& object, std::string* name) {
  if (object == nullptr || name == nullptr) return false;
  const auto found = object.find("name");
  if (found == object.end() || !found->is_string()) return false;
  const std::string& value = found->get_ref<const std::string&>();
  if (value.empty() || value.size() > kMaximumGameNameBytes ||
      ContainsControlBytes(value) || !IsValidUtf8(value)) {
    return false;
  }
  *name = value;
  return true;
}

std::string PrivateServerConfirmationMessage(std::string_view game_name) {
  std::string message = "You're trying to join ";
  message.append(game_name);
  message += "'s private server. Do you want to join?";
  return message;
}

constexpr std::string_view kPrivateServerVerificationFailureMessage =
    "Roblox could not verify this game. The private server was not joined.";

}  // namespace

std::optional<RobloxPrivateServerGameLink> ParseRobloxPrivateServerGameLink(
    std::string_view text) {
  if (text.empty() || text.size() > kMaximumGameUrlBytes ||
      ContainsControlBytes(text) || !IsValidUtf8(text)) {
    return std::nullopt;
  }

  const ScopedCurlUrl parsed;
  const std::string url(text);
  if (parsed.get() == nullptr ||
      curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(),
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
  if ((normalized_host != "www.roblox.com" &&
       normalized_host != "roblox.com") ||
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

  ScopedCurlString raw_query;
  if (curl_url_get(parsed.get(), CURLUPART_QUERY, raw_query.output(), 0) !=
          CURLUE_OK ||
      raw_query.get() == nullptr) {
    return std::nullopt;
  }
  std::string link_code;
  if (!ParsePrivateServerQuery(raw_query.get(), &link_code)) {
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
  return RobloxPrivateServerGameLink{place_id, std::move(link_code),
                                     std::move(fallback_game_name)};
}

RobloxPrivateServerPasteResult HandleRobloxPrivateServerPaste(
    std::string_view text,
    const RobloxPrivateServerPasteActions& actions) {
  const std::optional<RobloxPrivateServerGameLink> link =
      ParseRobloxPrivateServerGameLink(text);
  if (!link.has_value()) return {};
  if (!actions.complete()) {
    return {true, false,
            Unavailable("private-server paste handler is incomplete")};
  }
  if (!actions.is_main_text_entry(actions.context)) return {};

  bool approved = false;
  Status status = Status::Ok();
  if (actions.confirm_link != nullptr) {
    status = actions.confirm_link(actions.context, *link, &approved);
  } else {
    RobloxPrivateServerGameDetails details;
    status = actions.resolve_game(actions.context, link->place_id, &details);
    if (!status.ok()) return {true, false, std::move(status)};
    if (details.place_id <= 0) {
      return {true, false,
              Unavailable("private-server game has no launchable place")};
    }
    const std::string game_name = details.name.empty()
                                      ? link->fallback_game_name
                                      : details.name;
    if (game_name.empty() || game_name.size() > kMaximumGameNameBytes ||
        ContainsControlBytes(game_name) || !IsValidUtf8(game_name)) {
      return {true, false,
              Unavailable("private-server game has no valid display name")};
    }
    status = actions.confirm(actions.context, game_name, &approved);
  }
  if (!status.ok()) return {true, false, std::move(status)};
  if (!approved) return {true, false, Status::Ok()};

  // The URL's place ID is authoritative. Do not replace a sub-place with the
  // universe's root place while resolving the human-readable game name.
  status = actions.launch(actions.context, link->place_id, link->link_code);
  return {true, status.ok(), std::move(status)};
}

std::optional<nlohmann::json> GetRobloxJson(const std::string& url) {
  static std::once_flag curl_initialization;
  static CURLcode curl_initialization_status = CURLE_FAILED_INIT;
  std::call_once(curl_initialization, [] {
    curl_initialization_status = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
  if (curl_initialization_status != CURLE_OK) return std::nullopt;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return std::nullopt;
  BoundedResponse response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3500L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nightcap/1.0");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteHttpResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  std::string proxy_url;
  const char* proxy_host = std::getenv("MOCKTAIL_HTTP_PROXY_HOST");
  const char* proxy_port = std::getenv("MOCKTAIL_HTTP_PROXY_PORT");
  const char* proxy_scheme = std::getenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  if (proxy_host != nullptr && proxy_port != nullptr) {
    const std::optional<NetworkProxyConfig> proxy =
        ParseNetworkProxyConfig(proxy_host, proxy_port,
                                proxy_scheme != nullptr ? proxy_scheme : "http");
    if (proxy.has_value()) {
      proxy_url = BuildNetworkProxyUrl(*proxy);
      curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
      curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
    }
  }
  if (const char* ca_bundle = std::getenv("MOCKTAIL_CA_BUNDLE")) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
  }

  const CURLcode perform_status = curl_easy_perform(curl);
  long http_status = 0;
  if (perform_status == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  }
  curl_easy_cleanup(curl);
  if (perform_status != CURLE_OK || http_status != 200 || response.exceeded) {
    return std::nullopt;
  }
  nlohmann::json document = nlohmann::json::parse(
      response.body.begin(), response.body.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }
  return document;
}

Status ResolveRobloxPrivateServerGame(
    void* /*context*/, int64_t place_id,
    RobloxPrivateServerGameDetails* details) {
  if (details == nullptr || place_id <= 0) {
    return Invalid("private-server game lookup is invalid");
  }
  *details = {};

  const std::optional<nlohmann::json> universe_document = GetRobloxJson(
      "https://apis.roblox.com/universes/v1/places/" +
      std::to_string(place_id) + "/universe");
  int64_t universe_id = 0;
  if (!universe_document.has_value() ||
      !ReadPositiveJsonId(*universe_document, "universeId", &universe_id)) {
    return Unavailable("Roblox did not return private-server universe details");
  }

  const std::optional<nlohmann::json> game_document = GetRobloxJson(
      std::string(kGamesEndpoint) + std::to_string(universe_id));
  if (!game_document.has_value()) {
    return Unavailable("Roblox did not return private-server game details");
  }
  const auto data = game_document->find("data");
  if (data == game_document->end() || !data->is_array() || data->empty() ||
      !(*data)[0].is_object()) {
    return Unavailable("Roblox game no longer exists or is unavailable");
  }
  const nlohmann::json& game = (*data)[0];
  int64_t returned_universe_id = 0;
  // The current games.roblox.com response calls the universe identifier
  // "id". Some older/proxied responses use "universeId" instead, so accept
  // either spelling while still checking that it is the universe we requested.
  if (!ReadPositiveJsonId(game, "universeId", &returned_universe_id) &&
      !ReadPositiveJsonId(game, "id", &returned_universe_id)) {
    return Unavailable("Roblox returned incomplete private-server game details");
  }
  if (returned_universe_id != universe_id) {
    return Unavailable("Roblox returned incomplete private-server game details");
  }
  details->place_id = place_id;
  if (!SafeGameName(game, &details->name)) details->name.clear();
  return Status::Ok();
}

Status ConfirmRobloxPrivateServerPaste(void* /*context*/,
                                      std::string_view game_name,
                                      bool* approved) {
  if (approved == nullptr || game_name.empty() ||
      game_name.size() > kMaximumGameNameBytes ||
      ContainsControlBytes(game_name) || !IsValidUtf8(game_name)) {
    return Invalid("private-server confirmation is invalid");
  }
  *approved = false;

  const ProcessEnvironment environment;
  switch (ShowConfirmationDialog(
      environment, PrivateServerConfirmationMessage(game_name))) {
    case ConfirmationDialogResult::kConfirmed:
      *approved = true;
      return Status::Ok();
    case ConfirmationDialogResult::kCancelled:
      return Status::Ok();
    case ConfirmationDialogResult::kUnavailable:
      return Unavailable("private-server confirmation is unavailable");
  }
  return Unavailable("private-server confirmation returned no response");
}

Status ResolvePrivateServerConfirmationMessage(void* context,
                                                std::string* ready_message) {
  if (ready_message == nullptr) {
    return Invalid("private-server confirmation output is invalid");
  }
  const auto* link =
      static_cast<const RobloxPrivateServerGameLink*>(context);
  if (link == nullptr || link->place_id <= 0 || link->link_code.empty()) {
    *ready_message = kPrivateServerVerificationFailureMessage;
    return Invalid("private-server confirmation link is invalid");
  }

  RobloxPrivateServerGameDetails details;
  const Status status =
      ResolveRobloxPrivateServerGame(nullptr, link->place_id, &details);
  if (!status.ok()) {
    *ready_message = kPrivateServerVerificationFailureMessage;
    return status;
  }
  const std::string game_name = details.name.empty()
                                    ? link->fallback_game_name
                                    : details.name;
  if (details.place_id <= 0 || game_name.empty() ||
      game_name.size() > kMaximumGameNameBytes ||
      ContainsControlBytes(game_name) || !IsValidUtf8(game_name)) {
    *ready_message = kPrivateServerVerificationFailureMessage;
    return Unavailable("private-server game has no valid display name");
  }
  *ready_message = PrivateServerConfirmationMessage(game_name);
  return Status::Ok();
}

Status ConfirmRobloxPrivateServerPasteLink(
    void* /*context*/, const RobloxPrivateServerGameLink& link,
    bool* approved) {
  if (approved == nullptr || link.place_id <= 0 || link.link_code.empty()) {
    return Invalid("private-server confirmation link is invalid");
  }
  *approved = false;
  const ProcessEnvironment environment;
  switch (ShowConfirmationDialog(
      environment, "Checking Roblox game details...",
      const_cast<RobloxPrivateServerGameLink*>(&link),
      &ResolvePrivateServerConfirmationMessage)) {
    case ConfirmationDialogResult::kConfirmed:
      *approved = true;
      return Status::Ok();
    case ConfirmationDialogResult::kCancelled:
      return Status::Ok();
    case ConfirmationDialogResult::kUnavailable:
      return Unavailable("private-server confirmation is unavailable");
  }
  return Unavailable("private-server confirmation returned no response");
}

}  // namespace runtime
}  // namespace mocktail
