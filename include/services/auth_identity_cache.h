#ifndef MOCKTAIL_SERVICES_AUTH_IDENTITY_CACHE_H_
#define MOCKTAIL_SERVICES_AUTH_IDENTITY_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace mocktail::services {

// Resolving the signed-in account is a synchronous HTTPS round-trip to
// users.roblox.com on every launch, and the answer is the same account every
// time. This caches it.
//
// The key is a SHA-256 of the .ROBLOSECURITY value, never the value itself: a
// cache hit has to be able to tell "this is the cookie that produced this
// identity" without the file on disk holding a usable credential. A different
// cookie, a rotated session, or a changed account misses the cache and goes to
// the network.
//
// Like the payload caches, this only makes a check cheaper. A missing, stale,
// malformed, or mismatched record means the identity is resolved over HTTP as
// before, and a rejected session (401/403) is never served from here -- only a
// successful resolution is ever recorded.
struct CachedAuthIdentity {
  std::string cookie_digest;
  std::string username;
  std::string display_name;
  std::int64_t user_id = -1;
  std::int64_t checked_at_epoch = 0;
};

// `interval_minutes` of 0 or less means never reuse a record.
bool CachedAuthIdentityFresh(const CachedAuthIdentity& cached,
                             std::int64_t interval_minutes,
                             std::int64_t now_epoch);

std::filesystem::path AuthIdentityCachePath();

bool ReadCachedAuthIdentity(CachedAuthIdentity* cached);

bool WriteCachedAuthIdentity(const CachedAuthIdentity& cached,
                             std::string* error);

// SHA-256 of the cookie value, lowercase hex. Returns empty when it cannot be
// computed, which callers treat as "do not cache".
std::string AuthCookieDigest(std::string_view cookie_value);

// Wall-clock seconds since the epoch, used to stamp the record.
std::int64_t AuthCacheNowEpoch();

}  // namespace mocktail::services

#endif  // MOCKTAIL_SERVICES_AUTH_IDENTITY_CACHE_H_