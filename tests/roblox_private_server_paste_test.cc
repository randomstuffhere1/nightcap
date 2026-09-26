#include "runtime/roblox_private_server_paste.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {
namespace {

TEST(RobloxPrivateServerPasteTest, ParsesPrivateServerGameUrl) {
  const auto link = ParseRobloxPrivateServerGameLink(
      "https://www.roblox.com/games/1234567890/Game-Name?"
      "privateServerLinkCode=12360805305284755354292583827456");

  ASSERT_TRUE(link.has_value());
  EXPECT_EQ(link->place_id, 1234567890);
  EXPECT_EQ(link->link_code,
            "12360805305284755354292583827456");
  EXPECT_EQ(link->fallback_game_name, "Game Name");
}

TEST(RobloxPrivateServerPasteTest, ParsesEncodedSlugAndAdditionalParameters) {
  const auto link = ParseRobloxPrivateServerGameLink(
      "https://roblox.com/games/42/A%20Game?privateServerLinkCode=123&foo=bar");

  ASSERT_TRUE(link.has_value());
  EXPECT_EQ(link->place_id, 42);
  EXPECT_EQ(link->link_code, "123");
  EXPECT_EQ(link->fallback_game_name, "A Game");

  const auto trailing_slash = ParseRobloxPrivateServerGameLink(
      "https://roblox.com/games/42/A%20Game/?privateServerLinkCode=123");
  ASSERT_TRUE(trailing_slash.has_value());
  EXPECT_EQ(trailing_slash->fallback_game_name, "A Game");
}

TEST(RobloxPrivateServerPasteTest, RejectsUntrustedOrIncompleteGameUrls) {
  for (const char* url : {
           "http://www.roblox.com/games/42/Game?privateServerLinkCode=123",
           "https://evil.example/games/42/Game?privateServerLinkCode=123",
           "https://www.roblox.com.evil.example/games/42/Game?privateServerLinkCode=123",
           "https://www.roblox.com/games/0/Game?privateServerLinkCode=123",
           "https://www.roblox.com/games/42/Game",
           "https://www.roblox.com/games/42/Game?privateServerLinkCode=abc",
           "https://www.roblox.com/games/42/Game?privateServerLinkCode=1&privateServerLinkCode=2",
           "https://www.roblox.com/games/42/Game?privateServerLinkCode=123#fragment",
           "https://www.roblox.com/games/42/Game//?privateServerLinkCode=123",
       }) {
    EXPECT_FALSE(ParseRobloxPrivateServerGameLink(url).has_value()) << url;
  }
}

struct ActionProbe {
  bool main_text = true;
  bool resolve_success = true;
  bool confirm_success = true;
  bool launch_success = true;
  bool approve = true;
  int resolve_calls = 0;
  int confirm_calls = 0;
  int confirm_link_calls = 0;
  int launch_calls = 0;
  int64_t place_id = 0;
  std::string game_name;
  std::string link_code;
};

bool IsMainText(void* context) {
  return static_cast<ActionProbe*>(context)->main_text;
}

Status ResolveGame(void* context, int64_t place_id,
                   RobloxPrivateServerGameDetails* details) {
  auto* probe = static_cast<ActionProbe*>(context);
  ++probe->resolve_calls;
  if (!probe->resolve_success) {
    return Status::Error(StatusCode::kUnavailable, "injected lookup failure");
  }
  // Metadata may report a root/canonical place. The URL's place ID remains
  // authoritative for the actual launch.
  details->place_id = 999;
  details->name = "Actual Game";
  return Status::Ok();
}

Status ConfirmGame(void* context, std::string_view game_name, bool* approved) {
  auto* probe = static_cast<ActionProbe*>(context);
  ++probe->confirm_calls;
  probe->game_name.assign(game_name);
  if (!probe->confirm_success) {
    return Status::Error(StatusCode::kUnavailable,
                         "injected confirmation failure");
  }
  *approved = probe->approve;
  return Status::Ok();
}

Status ConfirmLinkGame(void* context,
                      const RobloxPrivateServerGameLink& link,
                      bool* approved) {
  auto* probe = static_cast<ActionProbe*>(context);
  ++probe->confirm_link_calls;
  if (!probe->confirm_success) {
    return Status::Error(StatusCode::kUnavailable,
                         "injected confirmation failure");
  }
  probe->game_name = "Immediate Game";
  probe->place_id = link.place_id;
  probe->link_code = link.link_code;
  *approved = probe->approve;
  return Status::Ok();
}

Status LaunchGame(void* context, int64_t place_id, std::string_view link_code) {
  auto* probe = static_cast<ActionProbe*>(context);
  ++probe->launch_calls;
  probe->place_id = place_id;
  probe->link_code.assign(link_code);
  return probe->launch_success
             ? Status::Ok()
             : Status::Error(StatusCode::kUnavailable,
                             "injected launch failure");
}

RobloxPrivateServerPasteActions Actions(ActionProbe* probe) {
  return {probe, &IsMainText, &ResolveGame, &ConfirmGame, &LaunchGame};
}

RobloxPrivateServerPasteActions ImmediateActions(ActionProbe* probe) {
  return {probe, &IsMainText, nullptr, nullptr, &LaunchGame,
          &ConfirmLinkGame};
}

constexpr char kPrivateUrl[] =
    "https://www.roblox.com/games/1234567890/Game-Name?"
    "privateServerLinkCode=12360805305284755354292583827456";

TEST(RobloxPrivateServerPasteTest, ConfirmedMainTextPasteLaunchesPrivateServer) {
  ActionProbe probe;
  const auto result = HandleRobloxPrivateServerPaste(kPrivateUrl, Actions(&probe));

  EXPECT_TRUE(result.handled);
  EXPECT_TRUE(result.approved);
  EXPECT_TRUE(result.status.ok()) << result.status.message();
  EXPECT_EQ(probe.resolve_calls, 1);
  EXPECT_EQ(probe.confirm_calls, 1);
  EXPECT_EQ(probe.launch_calls, 1);
  EXPECT_EQ(probe.place_id, 1234567890);
  EXPECT_EQ(probe.game_name, "Actual Game");
  EXPECT_EQ(probe.link_code, "12360805305284755354292583827456");
}

TEST(RobloxPrivateServerPasteTest, ImmediateConfirmationSkipsPrePromptResolve) {
  ActionProbe probe;
  const auto result = HandleRobloxPrivateServerPaste(kPrivateUrl,
                                                     ImmediateActions(&probe));

  EXPECT_TRUE(result.handled);
  EXPECT_TRUE(result.approved);
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(probe.resolve_calls, 0);
  EXPECT_EQ(probe.confirm_link_calls, 1);
  EXPECT_EQ(probe.launch_calls, 1);
  EXPECT_EQ(probe.place_id, 1234567890);
  EXPECT_EQ(probe.game_name, "Immediate Game");
}

TEST(RobloxPrivateServerPasteTest, DeclinedMainTextPasteDoesNotLaunch) {
  ActionProbe probe;
  probe.approve = false;
  const auto result = HandleRobloxPrivateServerPaste(kPrivateUrl, Actions(&probe));

  EXPECT_TRUE(result.handled);
  EXPECT_FALSE(result.approved);
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(probe.confirm_calls, 1);
  EXPECT_EQ(probe.launch_calls, 0);
}

TEST(RobloxPrivateServerPasteTest, ExperienceTextIsNotIntercepted) {
  ActionProbe probe;
  probe.main_text = false;
  const auto result = HandleRobloxPrivateServerPaste(kPrivateUrl, Actions(&probe));

  EXPECT_FALSE(result.handled);
  EXPECT_FALSE(result.approved);
  EXPECT_EQ(probe.resolve_calls, 0);
  EXPECT_EQ(probe.confirm_calls, 0);
  EXPECT_EQ(probe.launch_calls, 0);
}

TEST(RobloxPrivateServerPasteTest, NonPrivateTextPassesThrough) {
  ActionProbe probe;
  const auto result = HandleRobloxPrivateServerPaste(
      "https://www.roblox.com/games/1234567890/Game-Name", Actions(&probe));

  EXPECT_FALSE(result.handled);
  EXPECT_EQ(probe.resolve_calls, 0);
}

TEST(RobloxPrivateServerPasteTest, ResolutionFailureConsumesPrivateUrl) {
  ActionProbe probe;
  probe.resolve_success = false;
  const auto result = HandleRobloxPrivateServerPaste(kPrivateUrl, Actions(&probe));

  EXPECT_TRUE(result.handled);
  EXPECT_FALSE(result.approved);
  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(probe.confirm_calls, 0);
  EXPECT_EQ(probe.launch_calls, 0);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
