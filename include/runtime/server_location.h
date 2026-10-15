#ifndef MOCKTAIL_RUNTIME_SERVER_LOCATION_H_
#define MOCKTAIL_RUNTIME_SERVER_LOCATION_H_

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/owned_pthread.h"

namespace mocktail::runtime {

// Where the game server a session landed in is. Only the server's own address
// or the server ID ever leaves the machine, never the player's, so the fallback
// provider below cannot disclose who is playing.
struct ServerLocation {
  // Which provider answered: "roblox" for Roblox's own server info, "ipinfo"
  // for the address lookup.
  std::string source;
  // Roblox's own data-center code, e.g. "IAD". Only the Roblox provider sets
  // one; it is passed through rather than mapped, so no invented table of codes
  // can claim a country Roblox never said.
  std::string region;
  // Whatever the provider calls the location: an ISO country code, a country
  // name, or a region name.
  std::string country;
  // What was looked up, so the session log says what the answer is about.
  std::string endpoint;
};

struct UdpPeer {
  std::string address;
  std::uint16_t port = 0;
};

// Roblox's own server info for one server ID. Best effort and unauthenticated:
// when Roblox answers, the result carries its data-center region and needs no
// third party at all.
std::string BuildRobloxServerUrl(std::string_view server_id);
std::optional<ServerLocation> ParseRobloxServerLocation(
    std::string_view body, std::string_view server_id);

// The public ipinfo.io lookup Bloxstrap's server-location hint uses, applied to
// the game server's address.
std::string BuildIpInfoUrl(std::string_view address);
std::optional<ServerLocation> ParseIpInfoLocation(std::string_view body,
                                                  std::string_view address);

// Reads the UDP peers this process has, from the kernel's own socket tables.
// The guest runs in-process, so its game connection is one of ours: no shim
// hook, no interception, no packet inspection.
std::vector<UdpPeer> ReadUdpPeers(std::string_view proc_net_udp_path,
                                  std::string_view proc_net_udp6_path);

// The host's own name servers, which show up in the same table as the game
// connection and would otherwise be geolocated as if they were a server.
std::vector<std::string> ReadResolverAddresses(
    std::string_view resolv_conf_path);

bool IsLoopbackAddress(std::string_view address);

// Whether an address can be a game server. Only a publicly routable address
// can: the client connects to Roblox over the internet, so anything on the
// machine's own network, the local link, or a carrier-grade NAT is some other
// socket entirely. The kernel's table holds every UDP socket in the process,
// which includes things like a DHCP conversation that have nothing to do with
// the game, and geolocating one of those would report a made-up place.
bool IsPubliclyRoutableAddress(std::string_view address);

// Picks the peer a game session most likely landed on: the first connected peer
// that is neither unspecified, loopback, nor a name server. When `baseline`
// holds the peers that already existed before the join, a peer that is new to
// the process wins, because that is the connection the session just made.
std::optional<UdpPeer> SelectGameServerPeer(
    const std::vector<UdpPeer>& peers,
    const std::vector<std::string>& excluded_addresses,
    const std::vector<UdpPeer>& baseline = {});

// Watches for the game server a session lands in, and reports where it is, on
// a worker so a slow lookup can never hold up the join. Every step is best
// effort: a session that cannot be located simply prints nothing extra.
//
// The worker keeps watching until it finds the connection rather than giving up
// after a fixed number of attempts, because the client joins whenever the
// player asks it to. A session that has not joined yet is the normal case, not
// a failed lookup.
class ServerLocationResolver final {
 public:
  ServerLocationResolver();
  ~ServerLocationResolver();

  ServerLocationResolver(const ServerLocationResolver&) = delete;
  ServerLocationResolver& operator=(const ServerLocationResolver&) = delete;

  // Starts watching for a joined server. `server_id` is only known when the
  // host launched the experience itself, and lets the lookup ask Roblox rather
  // than a third party. A watch already running is left alone, so a repeated
  // join cannot pile up workers.
  void ObserveJoinedServer(std::string server_id);
  // True while there is nothing more for the host to do: either a watch is
  // still running, or this session has already been located. A session that has
  // been located stays located: a second watch would find the client's own
  // connections to Roblox rather than the server the player joined.
  bool Watching();
  // Joins a running lookup. Call before the session log is stopped.
  void Stop();

 private:
  static void* ThreadEntry(void* context);
  void Resolve(const std::string& server_id);

  OwnedPthread worker_;
  std::string server_id_;
  std::atomic<bool> stopped_{false};
  // Set once this session has been located.
  std::atomic<bool> reported_{false};
};

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_SERVER_LOCATION_H_