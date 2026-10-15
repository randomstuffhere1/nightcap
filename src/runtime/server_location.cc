#include "runtime/server_location.h"

#define JSON_NOEXCEPTION 1
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "runtime/host_toast_overlay.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

// A join must never wait on a lookup, so this is a fraction of the join it
// rides along with and a hard ceiling on what it can spend.
constexpr long kLookupTimeoutMs = 2500L;
constexpr long kLookupConnectTimeoutMs = 1500L;
constexpr std::size_t kMaximumResponseBytes = 64U * 1024U;
// How often the kernel's socket table is sampled while waiting for the game
// connection. The client connects whenever the player joins, so this runs until
// it sees the connection rather than over a fixed window.
constexpr int kPeerPollIntervalMs = 500;
// One slice of that wait. Small enough that closing the client does not wait on
// it, large enough that the worker is not spinning.
constexpr int kPeerPollSlicedSleepMs = 25;
// How long to wait before asking the address provider again after it failed.
constexpr std::int64_t kLookupRetryDelayMs = 30000;
constexpr std::size_t kMaximumResolvers = 8;
constexpr std::size_t kMaximumResolverAddressBytes = 64;

// Roblox's own server-info service, which is what the client itself asks for a
// server's data center. It is unauthenticated and best effort: when it answers,
// the result carries a region and no third party is contacted at all. When it
// does not, the address lookup below answers instead.
constexpr const char* kRobloxServerUrlPrefix =
    "https://apis.roblox.com/game-servers/v1/servers/";
// The public provider Bloxstrap's server-location hint uses.
constexpr const char* kIpInfoUrlPrefix = "https://ipinfo.io/";

struct BoundedResponse {
  std::string body;
  bool exceeded = false;
};

std::size_t WriteResponse(char* data, std::size_t size, std::size_t count,
                          void* context) {
  auto* response = static_cast<BoundedResponse*>(context);
  const std::size_t bytes = size * count;
  if (response->body.size() + bytes > kMaximumResponseBytes) {
    response->exceeded = true;
    return 0;
  }
  response->body.append(data, bytes);
  return bytes;
}

// A quiet GET that never throws and never blocks past its ceiling. Provider
// and proxy details are not propagated into error text.
bool GetJson(const std::string& url, std::string* body) {
  static std::string proxy_url;
  static bool proxy_loaded = false;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return false;
  }
  BoundedResponse response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kLookupConnectTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kLookupTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mocktail/0.1");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  if (!proxy_loaded) {
    proxy_loaded = true;
    const char* host = std::getenv("MOCKTAIL_HTTP_PROXY_HOST");
    const char* port = std::getenv("MOCKTAIL_HTTP_PROXY_PORT");
    if (host != nullptr && port != nullptr) {
      proxy_url = std::string("http://") + host + ":" + port;
    }
  }
  if (!proxy_url.empty()) {
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
  }
  if (const char* ca_bundle = std::getenv("MOCKTAIL_CA_BUNDLE")) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
  }
  const CURLcode status = curl_easy_perform(curl);
  long http_status = 0;
  if (status == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  }
  curl_easy_cleanup(curl);
  if (status != CURLE_OK || http_status != 200 || response.exceeded) {
    return false;
  }
  *body = std::move(response.body);
  return true;
}

// Roblox prints each address as hex in the host's byte order, so a 32-bit word
// has to be read back the way the kernel wrote it.
std::vector<UdpPeer> CurrentPeers() {
  return ReadUdpPeers("/proc/net/udp", "/proc/net/udp6");
}

std::int64_t MonotonicMicroseconds() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void ReportFailure(const std::string& reason) {
  // The indicator is best effort, so a failure is recorded and nothing is shown
  // on screen: a toast that reports its own failure is noise, but the session
  // log has to say why the indicator stayed silent.
  std::fprintf(stderr, "  [server-location] not determined: %s\n",
               reason.c_str());
}

void Report(const ServerLocation& location, const std::string& summary,
            const char* source) {
  std::fprintf(stderr,
               "  [server-location] server=%s provider=%s location=%s\n",
               location.endpoint.c_str(), location.source.c_str(),
               summary.c_str());
  // The notice is a single line, and the line is the message. Where the answer
  // came from is kept to a short tag: it is worth knowing that the host asked
  // Roblox rather than a third party, but a sentence about it would crowd out the
  // place name the player came for.
  char toast[160];
  const int written =
      std::snprintf(toast, sizeof(toast), "Server location: %s (%s)",
                    summary.c_str(), source);
  // A provider string long enough to truncate could also be cut mid-character.
  // The line above still has it.
  if (written > 0 && static_cast<std::size_t>(written) < sizeof(toast)) {
    // Drawn into the game's own window, so it cannot take a click meant for the
    // game. A session with no window yet, or one that never reaches a frame, has
    // nowhere on screen that is not the game's, so the line above is all there is.
    (void)ShowHostToast(toast);
  }
}

bool ParseHexByte(std::string_view hex, std::size_t index, char* byte) {
  if (index + 2 > hex.size()) {
    return false;
  }
  unsigned int value = 0;
  for (std::size_t offset = 0; offset < 2; ++offset) {
    const unsigned char character =
        static_cast<unsigned char>(hex[index + offset]);
    if (std::isxdigit(character) == 0) {
      return false;
    }
    value = value * 16U +
            static_cast<unsigned int>(std::isdigit(character)
                                          ? character - '0'
                                          : std::tolower(character) - 'a' + 10);
  }
  *byte = static_cast<char>(value & 0xFFU);
  return true;
}

std::string FormatHexAddress(std::string_view hex, std::size_t word_size) {
  if (hex.empty() || hex.size() % 2 != 0 || hex.size() / 2 < word_size) {
    return {};
  }
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    char byte = 0;
    if (!ParseHexByte(hex, index, &byte)) {
      return {};
    }
    bytes.push_back(byte);
  }
  // Reverse each 32-bit word, keeping the words themselves in order.
  std::string formatted;
  formatted.reserve(bytes.size() + 2);
  for (std::size_t word = 0; word + word_size <= bytes.size();
       word += word_size) {
    for (std::size_t offset = word_size; offset > 0; --offset) {
      formatted.push_back(bytes[word + offset - 1]);
    }
  }
  if (word_size == 4) {
    in_addr address{};
    std::memcpy(&address.s_addr, formatted.data(), 4);
    char text[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr) {
      return {};
    }
    return text;
  }
  in6_addr address{};
  std::memcpy(address.s6_addr, formatted.data(), 16);
  char text[INET6_ADDRSTRLEN] = {};
  if (inet_ntop(AF_INET6, &address, text, sizeof(text)) == nullptr) {
    return {};
  }
  return text;
}

bool ParseHexPort(std::string_view hex, std::uint16_t* port) {
  if (hex.empty() || hex.size() > 5) {
    return false;
  }
  unsigned int value = 0;
  for (const char character : hex) {
    if (std::isxdigit(static_cast<unsigned char>(character)) == 0) {
      return false;
    }
    value = value * 16U +
            static_cast<unsigned int>(std::isdigit(static_cast<unsigned char>(character))
                                          ? character - '0'
                                          : std::tolower(character) - 'a' + 10);
  }
  *port = static_cast<std::uint16_t>(value);
  return true;
}

// /proc/net/udp and udp6 both print "local rem st ..." after a "sl:" header.
bool ReadProcNetTable(std::string_view path, std::size_t word_size,
                      std::vector<UdpPeer>* peers) {
  std::ifstream input{std::string(path)};
  if (!input) {
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t first = line.find(':');
    if (first == std::string::npos) {
      continue;
    }
    std::istringstream fields(line.substr(first + 1));
    std::string local;
    std::string remote;
    if (!(fields >> local >> remote)) {
      continue;
    }
    const std::size_t local_separator = local.find(':');
    const std::size_t remote_separator = remote.find(':');
    if (local_separator == std::string::npos ||
        remote_separator == std::string::npos) {
      continue;
    }
    UdpPeer peer;
    std::uint16_t remote_port = 0;
    if (!ParseHexPort(remote.substr(remote_separator + 1), &remote_port) ||
        remote_port == 0) {
      // An unconnected socket has no peer; a game session's socket is
      // connected, so this row is not the one we are after.
      continue;
    }
    peer.address =
        FormatHexAddress(remote.substr(0, remote_separator), word_size);
    if (peer.address.empty()) {
      continue;
    }
    peer.port = remote_port;
    peers->push_back(std::move(peer));
  }
  return true;
}

}  // namespace

std::string BuildRobloxServerUrl(std::string_view server_id) {
  return std::string(kRobloxServerUrlPrefix) + std::string(server_id);
}

std::optional<ServerLocation> ParseRobloxServerLocation(
    std::string_view body, std::string_view server_id) {
  if (body.empty() || server_id.empty()) {
    return std::nullopt;
  }
  const Json document = Json::parse(body, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }
  std::string region;
  for (const char* key : {"region", "regionCode", "dataCenterRegion"}) {
    const auto value = document.find(key);
    if (value != document.end() && value->is_string() &&
        !value->get_ref<const std::string&>().empty()) {
      region = value->get<std::string>();
      break;
    }
  }
  if (region.empty()) {
    return std::nullopt;
  }
  ServerLocation location;
  location.source = "roblox";
  location.region = std::move(region);
  location.country = location.region;
  location.endpoint = std::string(server_id);
  return location;
}

std::string BuildIpInfoUrl(std::string_view address) {
  return std::string(kIpInfoUrlPrefix) + std::string(address) + "/json";
}

std::optional<ServerLocation> ParseIpInfoLocation(std::string_view body,
                                                  std::string_view address) {
  if (body.empty() || address.empty()) {
    return std::nullopt;
  }
  const Json document = Json::parse(body, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }
  const auto field = [&document](const char* key) {
    const auto value = document.find(key);
    if (value == document.end() || !value->is_string()) {
      return std::string();
    }
    return value->get_ref<const std::string&>();
  };
  // What the player recognises, in the order it narrows down: the city first,
  // then its region, then the country. A bare country code on its own says
  // almost nothing about where a server is, and two German cities share one.
  // The country's name and its code are the same fact, so only one of them is
  // ever used.
  const auto first_of = [&field](std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
      const std::string part = field(key);
      if (!part.empty()) {
        return part;
      }
    }
    return std::string();
  };
  std::string summary;
  for (const std::string& part :
       {first_of({"city"}), first_of({"region"}),
        first_of({"country_name", "country"})}) {
    if (part.empty()) {
      continue;
    }
    if (!summary.empty()) {
      summary += ", ";
    }
    summary += part;
  }
  if (summary.empty()) {
    return std::nullopt;
  }
  ServerLocation location;
  location.source = "ipinfo";
  location.country = std::move(summary);
  location.endpoint = std::string(address);
  return location;
}

std::vector<UdpPeer> ReadUdpPeers(std::string_view proc_net_udp_path,
                                  std::string_view proc_net_udp6_path) {
  std::vector<UdpPeer> peers;
  ReadProcNetTable(proc_net_udp_path, 4, &peers);
  ReadProcNetTable(proc_net_udp6_path, 16, &peers);
  return peers;
}

std::vector<std::string> ReadResolverAddresses(
    std::string_view resolv_conf_path) {
  std::vector<std::string> addresses;
  std::ifstream input{std::string(resolv_conf_path)};
  if (!input) {
    return addresses;
  }
  std::string line;
  while (std::getline(input, line) && addresses.size() < kMaximumResolvers) {
    std::size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos || line[begin] == '#' ||
        line[begin] == ';') {
      continue;
    }
    const std::size_t end = line.find_first_of(" \t\r;", begin);
    const std::string_view keyword(line.data() + begin,
                                   (end == std::string::npos ? line.size()
                                                              : end) -
                                       begin);
    if (keyword != "nameserver") {
      continue;
    }
    if (end == std::string::npos) {
      continue;
    }
    const std::size_t address_begin = line.find_first_not_of(" \t\r", end);
    if (address_begin == std::string::npos) {
      continue;
    }
    const std::size_t address_end =
        line.find_first_of(" \t\r;#;", address_begin);
    const std::string_view address(
        line.data() + address_begin,
        (address_end == std::string::npos ? line.size() : address_end) -
            address_begin);
    if (!address.empty() && address.size() <= kMaximumResolverAddressBytes) {
      addresses.emplace_back(address);
    }
  }
  return addresses;
}

bool IsLoopbackAddress(std::string_view address) {
  if (address == "::1") {
    return true;
  }
  // 127.0.0.0/8, which also covers the 127.0.0.53 stub resolvers.
  return address.rfind("127.", 0) == 0;
}

namespace {

bool ParseIpv4Octets(std::string_view address, std::uint8_t octets[4]) {
  std::size_t start = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const std::size_t dot = address.find('.', start);
    const std::string_view part =
        dot == std::string_view::npos ? address.substr(start)
                                      : address.substr(start, dot - start);
    if (part.empty() || part.size() > 3) {
      return false;
    }
    unsigned int value = 0;
    for (const char character : part) {
      if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
        return false;
      }
      value = value * 10U + static_cast<unsigned int>(character - '0');
    }
    if (value > 255U) {
      return false;
    }
    octets[index] = static_cast<std::uint8_t>(value);
    if (dot == std::string_view::npos) {
      return index == 3;
    }
    start = dot + 1;
  }
  return false;
}

}  // namespace

bool IsPubliclyRoutableAddress(std::string_view address) {
  if (address.empty() || address == "0.0.0.0" || address == "::") {
    return false;
  }
  // An IPv4 address inside an IPv6 one is the IPv4 address it carries, and is
  // judged as one.
  constexpr std::string_view kMappedPrefix = "::ffff:";
  if (address.rfind(kMappedPrefix, 0) == 0) {
    address.remove_prefix(kMappedPrefix.size());
  }
  if (IsLoopbackAddress(address)) {
    return false;
  }
  // fe80::/10 link-local, fc00::/7 unique-local, ff00::/8 multicast. The
  // kernel writes IPv6 addresses in lowercase hex.
  const bool link_local = address.size() > 3 && address[0] == 'f' &&
                          address[1] == 'e' && address[2] >= '8' &&
                          address[2] <= 'b';
  if (link_local) {
    return false;
  }
  if (address.rfind("fc", 0) == 0 || address.rfind("fd", 0) == 0 ||
      address.rfind("ff", 0) == 0) {
    return false;
  }

  std::uint8_t octets[4] = {0, 0, 0, 0};
  if (ParseIpv4Octets(address, octets)) {
    if (octets[0] == 0 || octets[0] >= 224) {
      // "This network", multicast, and reserved space.
      return false;
    }
    if (octets[0] == 10) {
      return false;  // 10.0.0.0/8
    }
    if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) {
      return false;  // 172.16.0.0/12
    }
    if (octets[0] == 192 && octets[1] == 168) {
      return false;  // 192.168.0.0/16
    }
    if (octets[0] == 169 && octets[1] == 254) {
      return false;  // 169.254.0.0/16
    }
    if (octets[0] == 100 && octets[1] >= 64 && octets[1] <= 127) {
      return false;  // 100.64.0.0/10
    }
    return true;
  }
  // Not an IPv4 address, so an IPv6 one: every IPv6 range that is not publicly
  // routable was rejected above.
  return address.find(':') != std::string_view::npos;
}

std::optional<UdpPeer> SelectGameServerPeer(
    const std::vector<UdpPeer>& peers,
    const std::vector<std::string>& excluded_addresses,
    const std::vector<UdpPeer>& baseline) {
  const auto eligible = [&](const UdpPeer& peer) {
    if (!IsPubliclyRoutableAddress(peer.address)) {
      return false;
    }
    return std::none_of(excluded_addresses.begin(), excluded_addresses.end(),
                        [&](const std::string& excluded) {
                          return excluded == peer.address;
                        });
  };
  // Only a connection this session opened *after* the watch began is evidence.
  // There is deliberately no fallback to whatever else is connected: the client
  // talks to Roblox's own API and CDN over UDP long before the player joins
  // anything, and geolocating one of those reports a data centre as if it were
  // the server they joined. Better to say nothing than to say that.
  for (const UdpPeer& peer : peers) {
    const bool known =
        std::any_of(baseline.begin(), baseline.end(),
                    [&](const UdpPeer& earlier) {
                      return earlier.address == peer.address &&
                             earlier.port == peer.port;
                    });
    if (!known && eligible(peer)) {
      return peer;
    }
  }
  return std::nullopt;
}

ServerLocationResolver::ServerLocationResolver() = default;

ServerLocationResolver::~ServerLocationResolver() { Stop(); }

void ServerLocationResolver::ObserveJoinedServer(std::string server_id) {
  if (stopped_.load() || reported_ || worker_.joinable()) {
    return;
  }
  server_id_ = std::move(server_id);
  worker_.Start(&ServerLocationResolver::ThreadEntry, this, 256U * 1024U);
}

bool ServerLocationResolver::Watching() {
  // A session that has been located stays located: re-arming a finished watch
  // would look for another server, and the client's own API connections would
  // answer instead, over and over.
  if (reported_) {
    return true;
  }
  if (!worker_.joinable()) {
    return false;
  }
  // Reaping a finished watch is what lets another join start a new one.
  const OwnedPthreadWaitResult wait = worker_.WaitFor(0, 1);
  if (wait.status == OwnedPthreadWaitStatus::kTimedOut) {
    return true;
  }
  return !wait.joined();
}

void ServerLocationResolver::Stop() {
  // Ask first, then wait. The watch runs until it is told to stop, so joining it
  // without saying so would hold the session's shutdown for the whole wait,
  // which is what a client closing its window waits on.
  stopped_.store(true);
  if (!worker_.joinable()) {
    return;
  }
  (void)worker_.WaitFor(10000, 10);
}

void* ServerLocationResolver::ThreadEntry(void* context) {
  auto* self = static_cast<ServerLocationResolver*>(context);
  self->Resolve(self->server_id_);
  return nullptr;
}

void ServerLocationResolver::Resolve(const std::string& server_id) {
  // Roblox first: it is the only source that knows its own data centers, and
  // answering here keeps the session off any third-party service.
  if (!server_id.empty()) {
    std::string body;
    if (GetJson(BuildRobloxServerUrl(server_id), &body)) {
      const std::optional<ServerLocation> location =
          ParseRobloxServerLocation(body, server_id);
      if (location.has_value()) {
        Report(*location, location->region, "Roblox");
        return;
      }
    }
  }

  // Fallback: the game connection is this process's own socket, so the kernel
  // already knows the address without the guest telling us. It is not connected
  // when the session starts, and the player joins whenever they please, so this
  // watches for the connection until it appears or the session ends. Giving up
  // after a fixed number of attempts would report nothing for every session
  // that takes longer than that to join.
  const std::vector<std::string> resolvers =
      ReadResolverAddresses("/etc/resolv.conf");
  const std::vector<UdpPeer> baseline = CurrentPeers();
  bool saw_a_peer = false;
  // A provider that cannot answer is retried, but not on every sample: this
  // watches for as long as the session lasts, and hammering a third party every
  // half second is both rude and pointless.
  std::int64_t retry_after_us = 0;
  while (!stopped_.load()) {
    const std::int64_t now = MonotonicMicroseconds();
    if (now >= retry_after_us) {
      const std::optional<UdpPeer> peer =
          SelectGameServerPeer(CurrentPeers(), resolvers, baseline);
      if (peer.has_value()) {
        saw_a_peer = true;
        std::string body;
        std::optional<ServerLocation> location;
        bool answered = GetJson(BuildIpInfoUrl(peer->address), &body);
        if (answered) {
          location = ParseIpInfoLocation(body, peer->address);
        }
        if (location.has_value()) {
          std::fprintf(stderr,
                       "  [server-location] server=%s:%u provider=%s "
                       "country=%s\n",
                       peer->address.c_str(), peer->port,
                       location->source.c_str(), location->country.c_str());
          reported_.store(true);
          Report(*location, location->country, "ipinfo.io");
          return;
        }
        // Name the peer, so a provider that cannot place an address can be told
        // apart from one that was never reached.
        ReportFailure(
            "server " + peer->address + ":" + std::to_string(peer->port) +
            (answered ? " was not placed by the address provider"
                      : " could not be asked about: the address provider did "
                        "not answer"));
        retry_after_us = now + kLookupRetryDelayMs * 1000;
      }
    }
    // Slept in short slices so that stopping the session does not have to wait
    // out a whole poll interval.
    for (int slept = 0; slept < kPeerPollIntervalMs && !stopped_.load();
         slept += kPeerPollSlicedSleepMs) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kPeerPollSlicedSleepMs));
    }
  }
  if (!saw_a_peer) {
    // The session ended without the client ever connecting to a game server.
    // A provider that answered without a location has already been recorded.
    ReportFailure("no connected game server socket appeared");
  }
}

}  // namespace runtime
}  // namespace mocktail