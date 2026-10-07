#include "runtime/roblox_private_server_paste.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
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
#include "runtime/roblox_game_url.h"
#include "runtime/roblox_presence_resolver.h"
#include "runtime/system_proxy.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumGameNameBytes = 256;
constexpr std::size_t kMaximumLinkCodeBytes = 128;
constexpr std::size_t kMaximumGameResponseBytes = 512 * 1024;
constexpr std::size_t kMaximumShareRequestBytes = 256;
constexpr std::size_t kMaximumHeaderBytes = 16 * 1024;
constexpr std::size_t kMaximumDebugBodyBytes = 1024;
constexpr const char* kGamesEndpoint =
    "https://games.roblox.com/v1/games?universeIds=";
// Roblox's LinkingService resolves the deferred /share?code=... token. It is
// the same endpoint the shipped Roblox client calls.
constexpr const char* kShareResolveEndpoint =
    "https://apis.roblox.com/sharelinks/v1/resolve-link";
// Roblox hands out the CSRF token as an x-csrf-token response header. This
// endpoint always rejects the call when no CSRF token accompanies it, so
// posting the cookie on its own is how the token is read without any effect
// on the session. Never send a token here: that is the only way this call
// could ever succeed, and succeeding would end the user's session.
constexpr const char* kCsrfTokenEndpoint = "https://auth.roblox.com/v1/logout";
constexpr char kCsrfTokenHeader[] = "x-csrf-token";

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

// A link code is the opaque-but-numeric join token Roblox expects. Rejecting
// anything else keeps a hostile resolved value from ever reaching the launch.
bool IsValidLinkCode(std::string_view value) {
  return !value.empty() && value.size() <= kMaximumLinkCodeBytes &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= '0' && character <= '9';
         });
}

bool IsShareCodeToken(std::string_view value) {
  return value.size() == 32 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f') ||
                  (character >= 'A' && character <= 'F');
         });
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

struct BoundedResponse {
  std::string body;
  bool exceeded = false;
};

struct RobloxHttpResult {
  long http_status = 0;
  CURLcode curl_status = CURLE_OK;
  std::string body;
  bool body_exceeded = false;
  std::string headers;
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

std::size_t WriteHttpHeader(char* data, std::size_t size,
                            std::size_t count, void* user_data) {
  auto* result = static_cast<RobloxHttpResult*>(user_data);
  if (result == nullptr || data == nullptr) return 0;
  const std::size_t bytes = size * count;
  // Returning short from a header callback aborts the transfer, so drop the
  // overflow instead of failing the request over oversized headers.
  if (result->headers.size() + bytes <= kMaximumHeaderBytes) {
    result->headers.append(data, bytes);
  }
  return bytes;
}

bool HeaderNameIs(std::string_view candidate, std::string_view expected) {
  if (candidate.size() != expected.size()) return false;
  for (std::size_t index = 0; index < candidate.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(candidate[index])) !=
        std::tolower(static_cast<unsigned char>(expected[index]))) {
      return false;
    }
  }
  return true;
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
  // Share the origin allow-list with the launch-URI entry point so the two
  // cannot drift on host, port or path validation.
  const std::optional<RobloxGameUrlLink> parsed = ParseRobloxGameUrl(text);
  if (parsed.has_value()) {
    if (parsed->link_code.empty()) {
      // A public game page carries no join code, so it is not a private-server
      // paste and must keep its normal paste behavior.
      return std::nullopt;
    }
    return RobloxPrivateServerGameLink{parsed->place_id, parsed->link_code,
                                       parsed->fallback_game_name, {}};
  }
  const std::optional<RobloxShareLinkUrl> share = ParseRobloxShareUrl(text);
  if (!share.has_value()) return std::nullopt;
  // A deferred share link carries neither a place ID nor a link code; both
  // come from Roblox's LinkingService at confirmation time.
  return RobloxPrivateServerGameLink{0, {}, {}, share->code};
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
  RobloxPrivateServerLaunchTarget target;
  if (actions.confirm_link != nullptr) {
    status = actions.confirm_link(actions.context, *link, &target, &approved);
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
    // The URL's place ID is authoritative. Do not replace a sub-place with the
    // universe's root place while resolving the human-readable game name.
    target.place_id = link->place_id;
    target.link_code = link->link_code;
  }
  if (!status.ok()) return {true, false, std::move(status)};
  if (!approved) return {true, false, Status::Ok()};
  // An approval without a complete target is never allowed to launch.
  if (target.place_id <= 0 || !IsValidLinkCode(target.link_code)) {
    return {true, false,
            Unavailable("private-server link has no valid launch target")};
  }

  status = actions.launch(actions.context, target.place_id, target.link_code);
  return {true, status.ok(), std::move(status)};
}

std::optional<nlohmann::json> RobloxJsonRequest(const std::string& url,
                                                const std::string* post_body,
                                                curl_slist* headers,
                                                 RobloxHttpResult* result_out) {
  static std::once_flag curl_initialization;
  static CURLcode curl_initialization_status = CURLE_FAILED_INIT;
  std::call_once(curl_initialization, [] {
    curl_initialization_status = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
  if (curl_initialization_status != CURLE_OK) return std::nullopt;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return std::nullopt;
  BoundedResponse response;
  RobloxHttpResult* result = result_out;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (headers != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  if (post_body != nullptr) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(post_body->size()));
  } else {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  }
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3500L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nightcap/1.0");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteHttpResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  if (result != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHttpHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, result);
  }

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
  if (result != nullptr) {
    result->curl_status = perform_status;
    result->http_status = http_status;
    result->body = response.body;
    result->body_exceeded = response.exceeded;
  }
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

std::optional<nlohmann::json> GetRobloxJson(const std::string& url) {
  return RobloxJsonRequest(url, nullptr, nullptr, nullptr);
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

// Roblox issues the CSRF token as a response header; it is not derivable from
// the session cookie. The token is stable for the life of the session, so it is
// cached after the first read.
std::string FetchRobloxCsrfToken(const std::string& auth_cookie,
                                 long* http_status_out) {
  static std::mutex token_mutex;
  static std::string cached_token;
  {
    const std::lock_guard<std::mutex> lock(token_mutex);
    if (!cached_token.empty()) return cached_token;
  }

  curl_slist* headers = nullptr;
  const std::string cookie_header = "Cookie: .ROBLOSECURITY=" + auth_cookie;
  headers = curl_slist_append(headers, cookie_header.c_str());
  RobloxHttpResult result;
  // Deliberately sent with no CSRF header and an empty body: this call is
  // expected to be rejected, and that rejection is where the token comes from.
  const std::string empty_body;
  (void)RobloxJsonRequest(kCsrfTokenEndpoint, &empty_body, headers, &result);
  curl_slist_free_all(headers);
  if (http_status_out != nullptr) *http_status_out = result.http_status;

  std::string token;
  std::size_t line_start = 0;
  while (line_start < result.headers.size()) {
    const std::size_t line_end = result.headers.find('\n', line_start);
    const std::string_view line = std::string_view(result.headers).substr(
        line_start, line_end == std::string::npos
                         ? std::string::npos
                         : line_end - line_start);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos &&
        HeaderNameIs(line.substr(0, colon), kCsrfTokenHeader)) {
      std::string_view value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
        value.remove_suffix(1);
      }
      token.assign(value);
      break;
    }
    if (line_end == std::string::npos) break;
    line_start = line_end + 1;
  }

  if (!token.empty()) {
    const std::lock_guard<std::mutex> lock(token_mutex);
    if (cached_token.empty()) cached_token = token;
  }
  return token;
}

// Roblox returns one typed payload per invite flavour, with every flavour that
// does not apply left null. A shared private server arrives under
// privateServerInviteData; the rest are the other link types.
constexpr const char* kShareInviteKeys[] = {
    "privateServerInviteData",
    "experienceInviteData",
    "notificationExperienceInviteData",
    "experienceDetailsInviteData",
    "experienceAffiliateData",
    "experienceEventData",
    "friendInviteData",
    "profileLinkResolutionResponseData",
    "screenshotInviteData",
    "avatarItemDetailsData",
    "contentPostData",
};

// Picks the populated payload out of that map. Reported back by name so a
// future rename shows up in the debug block instead of silently failing.
const nlohmann::json& ReadShareInvite(const nlohmann::json& root,
                                      std::string* key_out) {
  for (const char* key : kShareInviteKeys) {
    const auto found = root.find(key);
    if (found != root.end() && found->is_object() && !found->empty()) {
      if (key_out != nullptr) *key_out = key;
      return *found;
    }
  }
  if (key_out != nullptr) key_out->clear();
  return root;
}

// Safety net for a payload Roblox later renames or nests differently: take the
// first top-level object that names both a place and a link code.
bool ReadShareLinkCode(const nlohmann::json& object, std::string* link_code);

bool FindShareInviteWithTarget(const nlohmann::json& root,
                               const nlohmann::json** invite_out,
                               std::string* key_out) {
  if (!root.is_object()) return false;
  for (auto entry = root.begin(); entry != root.end(); ++entry) {
    if (!entry.value().is_object() || entry.value().empty()) continue;
    int64_t place_id = 0;
    std::string link_code;
    if (ReadPositiveJsonId(entry.value(), "placeId", &place_id) &&
        ReadShareLinkCode(entry.value(), &link_code)) {
      *invite_out = &entry.value();
      if (key_out != nullptr) *key_out = entry.key();
      return true;
    }
  }
  return false;
}

bool ReadShareLinkCode(const nlohmann::json& object, std::string* link_code) {
  if (link_code == nullptr) return false;
  for (const char* key : {"linkCode", "privateServerLinkCode"}) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) continue;
    const std::string& value = found->get_ref<const std::string&>();
    if (IsValidLinkCode(value)) {
      *link_code = value;
      return true;
    }
  }
  return false;
}

std::string ReadShareGameName(const nlohmann::json& object) {
  for (const char* key : {"name", "gameName", "title"}) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) continue;
    const std::string& value = found->get_ref<const std::string&>();
    if (value.empty() || value.size() > kMaximumGameNameBytes ||
        ContainsControlBytes(value) || !IsValidUtf8(value)) {
      continue;
    }
    return value;
  }
  return {};
}

struct ShareInviteReadout {
  bool usable = false;
  std::string invite_key;
  std::string status;
  int64_t place_id = 0;
  std::string link_code;
  std::string game_name;
};

// Turns a LinkingService resolve-link payload into a joinable target. Recorded
// shape: one populated payload per invite flavour, with a shared private
// server arriving under privateServerInviteData.
ShareInviteReadout ReadShareInviteResponse(std::string_view json_body) {
  ShareInviteReadout readout;
  const nlohmann::json document = nlohmann::json::parse(
      json_body.begin(), json_body.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) return readout;

  std::string invite_key;
  const nlohmann::json& invite = ReadShareInvite(document, &invite_key);
  readout.invite_key = invite_key;
  const auto status = invite.find("status");
  if (status != invite.end() && status->is_string()) {
    readout.status = status->get_ref<const std::string&>();
  }
  readout.game_name = ReadShareGameName(invite);

  int64_t place_id = 0;
  std::string link_code;
  if (ReadPositiveJsonId(invite, "placeId", &place_id) &&
      ReadShareLinkCode(invite, &link_code)) {
    readout.usable = true;
    readout.place_id = place_id;
    readout.link_code = std::move(link_code);
    return readout;
  }

  const nlohmann::json* scanned = nullptr;
  std::string scanned_key;
  if (FindShareInviteWithTarget(document, &scanned, &scanned_key)) {
    place_id = 0;
    link_code.clear();
    if (ReadPositiveJsonId(*scanned, "placeId", &place_id) &&
        ReadShareLinkCode(*scanned, &link_code)) {
      readout.usable = true;
      readout.place_id = place_id;
      readout.link_code = std::move(link_code);
      readout.invite_key = scanned_key;
      const std::string name = ReadShareGameName(*scanned);
      if (!name.empty()) readout.game_name = name;
    }
  }
  return readout;
}

bool ParseRobloxPrivateServerShareResponse(
    std::string_view json_body, RobloxPrivateServerLaunchTarget* target,
    std::string* fallback_game_name) {
  if (target == nullptr) return false;
  *target = {};
  if (fallback_game_name != nullptr) fallback_game_name->clear();
  const ShareInviteReadout readout = ReadShareInviteResponse(json_body);
  if (!readout.usable) return false;
  target->place_id = readout.place_id;
  target->link_code = readout.link_code;
  if (fallback_game_name != nullptr) *fallback_game_name = readout.game_name;
  return true;
}

void LogShareLinkDebug(std::string_view share_code, bool cookie_present,
                       std::size_t cookie_len, long csrf_status,
                       std::string_view csrf_token, long resolve_status,
                       CURLcode curl_status, std::string_view body,
                       std::string_view invite_key, std::string_view outcome) {
  // Everything between the markers is meant to be pasted into a bug report, so
  // keep it on one line per field and never print the session cookie itself.
  // string_view::data() may be null when empty, which is not a valid %s.
  const char* code_text = share_code.empty() ? "" : share_code.data();
  const char* body_text = body.empty() ? "" : body.data();
  const char* outcome_text = outcome.empty() ? "" : outcome.data();
  const int body_len =
      static_cast<int>(std::min(body.size(), kMaximumDebugBodyBytes));
  std::fprintf(stderr,
               "  ===== NIGHTCAP ROBLOX SHARE LINK DEBUG BEGIN =====\n"
               "  share_code=%.*s\n"
               "  cookie_present=%d cookie_len=%zu\n"
               "  csrf_probe_http=%ld csrf_token_len=%zu\n"
               "  resolve_http=%ld curl=%d\n"
               "  invite_key=%.*s\n"
               "  resolve_body=%.*s\n"
               "  outcome=%.*s\n"
               "  ===== NIGHTCAP ROBLOX SHARE LINK DEBUG END =====\n",
               static_cast<int>(share_code.size()), code_text,
               cookie_present ? 1 : 0, cookie_len, csrf_status,
               csrf_token.size(), resolve_status, static_cast<int>(curl_status),
               static_cast<int>(invite_key.size()),
               invite_key.empty() ? "" : invite_key.data(), body_len,
               body_text, static_cast<int>(outcome.size()), outcome_text);
  std::fflush(stderr);
}

Status ResolveRobloxPrivateServerShareLink(
    std::string_view share_code, RobloxPrivateServerLaunchTarget* target,
    std::string* fallback_game_name) {
  if (target == nullptr || !IsShareCodeToken(share_code)) {
    return Invalid("private-server share code is invalid");
  }
  *target = {};
  if (fallback_game_name != nullptr) fallback_game_name->clear();

  // The deferred token is only resolvable for a signed-in account.
  const std::string auth_cookie = ReadStoredRoblosecurityCookie();
  if (auth_cookie.empty()) {
    LogShareLinkDebug(share_code, false, 0, 0, {}, 0, CURLE_OK, {}, {},
                      "no stored Roblox session");
    return Unavailable("resolving a Roblox share link requires sign-in");
  }

  long csrf_status = 0;
  const std::string csrf_token = FetchRobloxCsrfToken(auth_cookie, &csrf_status);
  if (csrf_token.empty()) {
    LogShareLinkDebug(share_code, true, auth_cookie.size(), csrf_status, {},
                      0, CURLE_OK, {}, {},
                      "could not obtain a CSRF token");
    return Unavailable("Roblox did not issue a CSRF token");
  }

  // Verified against Roblox's LinkingService: the selector is named linkId and
  // the flavour linkType, and the payload comes back flat.
  const nlohmann::json request_body = {{"linkId", std::string(share_code)},
                                       {"linkType", "Server"}};
  const std::string payload = request_body.dump();
  if (payload.size() > kMaximumShareRequestBytes) {
    return Invalid("private-server share request is too large");
  }

  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string cookie_header = "Cookie: .ROBLOSECURITY=" + auth_cookie;
  headers = curl_slist_append(headers, cookie_header.c_str());
  const std::string csrf_header = std::string("X-CSRF-TOKEN: ") + csrf_token;
  headers = curl_slist_append(headers, csrf_header.c_str());

  RobloxHttpResult result;
  const std::optional<nlohmann::json> document =
      RobloxJsonRequest(kShareResolveEndpoint, &payload, headers, &result);
  curl_slist_free_all(headers);

  if (!document.has_value()) {
    LogShareLinkDebug(share_code, true, auth_cookie.size(), csrf_status,
                      csrf_token, result.http_status, result.curl_status,
                      result.body, {},
                      "LinkingService rejected the request");
    return Unavailable("Roblox did not resolve the shared private server");
  }

  std::string invite_key;
  const nlohmann::json& invite = ReadShareInvite(*document, &invite_key);
  int64_t place_id = 0;
  std::string link_code;
  bool usable = ReadPositiveJsonId(invite, "placeId", &place_id) &&
                ReadShareLinkCode(invite, &link_code);
  if (!usable) {
    // The expected payload was not the one that named the server; look once
    // more across every populated payload before giving up.
    const nlohmann::json* scanned = nullptr;
    std::string scanned_key;
    if (FindShareInviteWithTarget(*document, &scanned, &scanned_key)) {
      place_id = 0;
      link_code.clear();
      if (ReadPositiveJsonId(*scanned, "placeId", &place_id) &&
          ReadShareLinkCode(*scanned, &link_code)) {
        usable = true;
        invite_key = scanned_key;
      }
    }
  }
  if (!usable) {
    std::string status;
    const auto found = invite.find("status");
    if (found != invite.end() && found->is_string()) {
      status = found->get_ref<const std::string&>();
    }
    LogShareLinkDebug(share_code, true, auth_cookie.size(), csrf_status,
                      csrf_token, result.http_status, result.curl_status,
                      result.body, invite_key,
                      "no joinable target; invite_key=" + invite_key +
                          " status=" +
                          (status.empty() ? std::string("unknown") : status));
    return Unavailable(
        status.empty()
            ? std::string("Roblox returned an unusable shared-server link")
            : "shared private server link is " + status);
  }
  target->place_id = place_id;
  target->link_code = std::move(link_code);
  if (fallback_game_name != nullptr) {
    *fallback_game_name = ReadShareGameName(invite);
  }
  LogShareLinkDebug(share_code, true, auth_cookie.size(), csrf_status,
                    csrf_token, result.http_status, result.curl_status,
                    result.body, invite_key, "resolved");
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

// Handed to the confirmation dialog as its "resolve now" callback so the
// prompt appears immediately while this runs behind it.
struct PrivateServerConfirmationContext {
  const RobloxPrivateServerGameLink* link = nullptr;
  RobloxPrivateServerLaunchTarget* target = nullptr;
};

Status ResolvePrivateServerConfirmationMessage(void* opaque_context,
                                                std::string* ready_message) {
  if (ready_message == nullptr) {
    return Invalid("private-server confirmation output is invalid");
  }
  const auto* context =
      static_cast<PrivateServerConfirmationContext*>(opaque_context);
  if (context == nullptr || context->link == nullptr ||
      context->target == nullptr) {
    *ready_message = kPrivateServerVerificationFailureMessage;
    return Invalid("private-server confirmation link is invalid");
  }
  const RobloxPrivateServerGameLink& link = *context->link;
  RobloxPrivateServerLaunchTarget target;
  std::string fallback_game_name = link.fallback_game_name;

  Status status = Status::Ok();
  if (!link.share_code.empty()) {
    // A deferred share link owns neither the place ID nor the link code, so
    // both have to come from Roblox before anything can be shown or launched.
    status = ResolveRobloxPrivateServerShareLink(
        link.share_code, &target, &fallback_game_name);
  } else if (link.place_id > 0 && IsValidLinkCode(link.link_code)) {
    target.place_id = link.place_id;
    target.link_code = link.link_code;
  } else {
    status = Invalid("private-server confirmation link is invalid");
  }
  if (!status.ok()) {
    *ready_message = kPrivateServerVerificationFailureMessage;
    return status;
  }

  RobloxPrivateServerGameDetails details;
  status = ResolveRobloxPrivateServerGame(nullptr, target.place_id, &details);
  if (!status.ok()) {
    *ready_message = kPrivateServerVerificationFailureMessage;
    return status;
  }
  const std::string game_name = details.name.empty() ? fallback_game_name
                                                     : details.name;
  if (details.place_id <= 0 || game_name.empty() ||
      game_name.size() > kMaximumGameNameBytes ||
      ContainsControlBytes(game_name) || !IsValidUtf8(game_name)) {
    *ready_message = kPrivateServerVerificationFailureMessage;
    return Unavailable("private-server game has no valid display name");
  }
  *context->target = std::move(target);
  *ready_message = PrivateServerConfirmationMessage(game_name);
  return Status::Ok();
}

Status ConfirmRobloxPrivateServerPasteLink(
    void* /*context*/, const RobloxPrivateServerGameLink& link,
    RobloxPrivateServerLaunchTarget* target, bool* approved) {
  if (approved == nullptr || target == nullptr) {
    return Invalid("private-server confirmation link is invalid");
  }
  *approved = false;
  *target = {};
  if (link.share_code.empty() &&
      (link.place_id <= 0 || !IsValidLinkCode(link.link_code))) {
    return Invalid("private-server confirmation link is invalid");
  }

  PrivateServerConfirmationContext context{&link, target};
  const ProcessEnvironment environment;
  switch (ShowConfirmationDialog(
      environment, "Checking Roblox game details...", &context,
      &ResolvePrivateServerConfirmationMessage)) {
    case ConfirmationDialogResult::kConfirmed:
      // The dialog only enables Yes once resolution succeeded, so an approval
      // here always carries a complete target.
      if (target->place_id <= 0 || !IsValidLinkCode(target->link_code)) {
        *target = {};
        return Unavailable("private-server confirmation produced no target");
      }
      *approved = true;
      return Status::Ok();
    case ConfirmationDialogResult::kCancelled:
      *target = {};
      return Status::Ok();
    case ConfirmationDialogResult::kUnavailable:
      *target = {};
      return Unavailable("private-server confirmation is unavailable");
  }
  *target = {};
  return Unavailable("private-server confirmation returned no response");
}

}  // namespace runtime
}  // namespace mocktail
