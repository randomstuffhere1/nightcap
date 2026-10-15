#include "runtime/server_location.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace mocktail::runtime {
namespace {

// The kernel prints each address as hex in the host's byte order, so this is
// the shape a connected UDP socket has in /proc/net/udp.
constexpr const char* kProcNetUdpFixture =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
    "retrnsmt   uid  timeout inode\n"
    "   0: 0100007F:0035 00000000:0000 07 00000000:00000000 00:00000000 "
    "00000000  1000        0 4242 1\n"
    "   1: 0100007F:14E9 68460A78:14E9 01 00000000:00000000 00:00000000 "
    "00000000  1000        0 4243 1\n"
    "   2: 00000000:0035 35B3B3B3:0035 0A 00000000:00000000 00:00000000 "
    "00000000  1000        0 4244 1\n";

class ServerLocationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail-server-location-XXXXXX";
    const char* directory = mkdtemp(pattern);
    ASSERT_NE(directory, nullptr);
    directory_ = directory;
    udp_path_ = std::filesystem::path(directory_) / "udp";
    WriteFixture(udp_path_, kProcNetUdpFixture);
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  static void WriteFixture(const std::filesystem::path& path,
                           const std::string& contents) {
    std::ofstream output(path);
    output << contents;
  }

  std::filesystem::path directory_;
  std::filesystem::path udp_path_;
};

TEST_F(ServerLocationTest, ReadsOnlyConnectedPeersFromTheKernelTables) {
  const std::vector<UdpPeer> peers = ReadUdpPeers(udp_path_.string(), "/nonexistent");

  // The unbound DNS responder (0.0.0.0:0) is not a peer, so the two rows with
  // a real remote address are all that is left.
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0].address, "120.10.70.104");
  EXPECT_EQ(peers[0].port, 5353u);
  EXPECT_EQ(peers[1].address, "179.179.179.53");
  EXPECT_EQ(peers[1].port, 53u);
}

TEST_F(ServerLocationTest, NeverFailsOnAMissingOrGarbledTable) {
  const std::filesystem::path garbage =
      directory_ / "garbage";
  WriteFixture(garbage, "not a table\n:::::\n");
  EXPECT_TRUE(ReadUdpPeers(garbage.string(), garbage.string()).empty());
  EXPECT_TRUE(ReadUdpPeers("/nonexistent", "/nonexistent").empty());
}

TEST(ServerLocationResolverTest, ExcludesLoopbackAndNameServers) {
  const std::vector<std::string> resolvers = {"127.0.0.53", "10.0.0.1"};
  // systemd-resolved and any other name server must never be geolocated as if
  // it were the game server, and neither must anything on loopback.
  std::vector<UdpPeer> peers = {{"127.0.0.53", 53},
                                {"127.0.0.1", 53},
                                {"::1", 53},
                                {"10.0.0.1", 53},
                                {"128.116.17.33", 2200}};
  const std::optional<UdpPeer> chosen =
      SelectGameServerPeer(peers, resolvers);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(chosen->address, "128.116.17.33");

  // Only the host's own traffic is connected: there is no server to report.
  peers = {{"127.0.0.53", 53}, {"10.0.0.1", 53}};
  EXPECT_FALSE(SelectGameServerPeer(peers, resolvers).has_value());
  EXPECT_FALSE(SelectGameServerPeer({}, resolvers).has_value());
}

TEST(ServerLocationResolverTest, PrefersTheConnectionTheSessionJustOpened) {
  const std::vector<std::string> resolvers = {"127.0.0.53"};
  const std::vector<UdpPeer> baseline = {{"198.51.100.7", 9000}};
  const std::vector<UdpPeer> current = {{"198.51.100.7", 9000},
                                        {"128.116.17.33", 2200}};
  const std::optional<UdpPeer> chosen =
      SelectGameServerPeer(current, resolvers, baseline);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(chosen->address, "128.116.17.33");

  // Only connections that already existed are left. A client that talked to
  // Roblox's own API before joining anything is not evidence of a server, and
  // reporting one of those is how the notice ended up naming a data centre at
  // launch.
  EXPECT_FALSE(
      SelectGameServerPeer(baseline, resolvers, baseline).has_value());
}

TEST(ServerLocationResolverTest, OnlyLooksAtAddressesThatCouldBeAGameServer) {
  // Everything the kernel's table holds that is not the game's connection:
  // the local network, the local link, carrier-grade NAT, and the host's own
  // traffic. Geolocating any of these would report a place Roblox never said.
  for (const char* address :
       {"10.0.0.1", "172.16.4.4", "172.31.255.255", "192.168.100.1",
        "169.254.10.10", "100.64.0.1", "0.0.0.0", "127.0.0.1", "::1", "::",
        "fe80::1", "fc00::1", "fd12:3456::1", "ff02::1",
        "::ffff:192.168.1.1"}) {
    EXPECT_FALSE(IsPubliclyRoutableAddress(address)) << address;
  }
  for (const char* address : {"128.116.17.33", "52.85.110.118", "172.32.0.1",
                              "172.15.255.255", "100.63.255.255", "2606:4700::1",
                              "::ffff:52.85.110.118"}) {
    EXPECT_TRUE(IsPubliclyRoutableAddress(address)) << address;
  }
  EXPECT_FALSE(IsPubliclyRoutableAddress(""));

  // A DHCP conversation on the local network must never be chosen, even though
  // it is a connected socket the session opened while the game was joining.
  const std::vector<std::string> resolvers = {"127.0.0.53"};
  const std::vector<UdpPeer> peers = {{"192.168.100.1", 67},
                                      {"128.116.17.33", 2200}};
  const std::optional<UdpPeer> chosen =
      SelectGameServerPeer(peers, resolvers);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(chosen->address, "128.116.17.33");
}

TEST(ServerLocationResolverTest, ReadsNameServersAndNeverBlocks) {
  char pattern[] = "/tmp/mocktail-resolv-conf-XXXXXX";
  const char* directory = mkdtemp(pattern);
  ASSERT_NE(directory, nullptr);
  const std::filesystem::path path =
      std::filesystem::path(directory) / "resolv.conf";
  {
    std::ofstream output(path);
    output << "# generated\n"
           << "nameserver 127.0.0.53\n"
           << "  nameserver   fd00::1  # lan\n"
           << "search example.com\n"
           << "\n"
           << "nameserver 10.0.0.1\n";
  }
  const std::vector<std::string> resolvers = ReadResolverAddresses(path.string());
  ASSERT_EQ(resolvers.size(), 3u);
  EXPECT_EQ(resolvers[0], "127.0.0.53");
  EXPECT_EQ(resolvers[1], "fd00::1");
  EXPECT_EQ(resolvers[2], "10.0.0.1");

  // A missing or unreadable file simply excludes nothing, which cannot
  // misidentify a server.
  EXPECT_TRUE(ReadResolverAddresses("/nonexistent/resolv.conf").empty());

  EXPECT_TRUE(IsLoopbackAddress("127.0.0.53"));
  EXPECT_TRUE(IsLoopbackAddress("127.1.2.3"));
  EXPECT_TRUE(IsLoopbackAddress("::1"));
  EXPECT_FALSE(IsLoopbackAddress("128.116.17.33"));
  EXPECT_FALSE(IsLoopbackAddress("10.0.0.1"));
  EXPECT_FALSE(IsLoopbackAddress("fd00::1"));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST(ServerLocationParserTest, UsesRobloxRegionWhenItAnswers) {
  EXPECT_EQ(
      ParseRobloxServerLocation(R"({"region":"IAD"})", "abc-123")->region,
      "IAD");
  EXPECT_EQ(ParseRobloxServerLocation(R"({"region":"IAD"})", "abc-123")->source,
            "roblox");
  // A response with no region says nothing about where the server is.
  EXPECT_FALSE(
      ParseRobloxServerLocation(R"({"ip_address":"1.2.3.4"})", "abc-123")
          .has_value());
  EXPECT_FALSE(ParseRobloxServerLocation("not json", "abc-123").has_value());
  EXPECT_FALSE(ParseRobloxServerLocation(R"({"region":"IAD"})", "").has_value());
}

TEST(ServerLocationParserTest, FallsBackToTheAddressProvider) {
  // What the player is shown is the place, not a bare country code: the city,
  // its region, and the country, in that order.
  const std::optional<ServerLocation> location = ParseIpInfoLocation(
      R"({"ip":"128.116.17.33","city":"Ashburn","region":"Virginia",)"
      R"("country":"US"})",
      "128.116.17.33");
  ASSERT_TRUE(location.has_value());
  EXPECT_EQ(location->source, "ipinfo");
  EXPECT_EQ(location->country, "Ashburn, Virginia, US");
  EXPECT_EQ(location->endpoint, "128.116.17.33");
  EXPECT_TRUE(location->region.empty());

  // Whatever the provider does supply is used, in the same order, and each part
  // appears once.
  EXPECT_EQ(ParseIpInfoLocation(R"({"city":"Frankfurt am Main","region":"Hesse",)"
                                R"("country":"DE"})",
                                "1.2.3.4")->country,
            "Frankfurt am Main, Hesse, DE");
  EXPECT_EQ(ParseIpInfoLocation(R"({"region":"Hesse","country":"DE"})", "1.2.3.4")
                ->country,
            "Hesse, DE");
  EXPECT_EQ(ParseIpInfoLocation(R"({"country":"SG"})", "1.2.3.4")->country, "SG");
  // A country name is preferred over the code when the provider sends both.
  EXPECT_EQ(
      ParseIpInfoLocation(R"({"country":"SG","country_name":"Singapore"})",
                          "1.2.3.4")
          ->country,
      "Singapore");

  EXPECT_FALSE(ParseIpInfoLocation(R"({"ip":"1.2.3.4"})", "1.2.3.4").has_value());
  EXPECT_FALSE(ParseIpInfoLocation("", "1.2.3.4").has_value());
}

TEST(ServerLocationResolverTest, ASessionIsOnlyEverLocatedOnce) {
  // Nothing has been located yet, so the host is free to start a watch and is
  // told one is needed.
  ServerLocationResolver resolver;
  EXPECT_FALSE(resolver.Watching());
  resolver.ObserveJoinedServer("");
  EXPECT_TRUE(resolver.Watching());
  (void)resolver.Stop();
  // Stopping ends the watch for the session; the host does not start another.
  EXPECT_FALSE(resolver.Watching());
}

TEST(ServerLocationResolverTest, StoppingARunningWatchReturnsPromptly) {
  // Closing the client waits on this, so a watch that only notices it has been
  // asked to stop at the end of its poll interval would hold the whole shutdown
  // down.
  ServerLocationResolver resolver;
  resolver.ObserveJoinedServer("");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto started = std::chrono::steady_clock::now();
  resolver.Stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  EXPECT_LT(elapsed, 2000) << "stopping a running watch took " << elapsed << "ms";
}

TEST(ServerLocationParserTest, BuildsHttpsOnlyProviderUrls) {
  EXPECT_EQ(BuildRobloxServerUrl("97ea-49e4"),
            "https://apis.roblox.com/game-servers/v1/servers/97ea-49e4");
  EXPECT_EQ(BuildIpInfoUrl("128.116.17.33"),
            "https://ipinfo.io/128.116.17.33/json");
}

TEST(ServerLocationResolverTest, StoppingWithoutASessionIsHarmless) {
  ServerLocationResolver resolver;
  resolver.Stop();
  // A resolver that already stopped ignores a late observation rather than
  // starting a worker nothing will join.
  resolver.ObserveJoinedServer("97ea-49e4");
  resolver.Stop();
}

}  // namespace
}  // namespace mocktail::runtime