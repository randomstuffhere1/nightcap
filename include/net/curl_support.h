#ifndef MOCKTAIL_NET_CURL_SUPPORT_H_
#define MOCKTAIL_NET_CURL_SUPPORT_H_

#include <curl/curl.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace net {

// libcurl is initialised exactly once per process.
//
// Every caller used to carry its own std::once_flag around
// curl_global_init(CURL_GLOBAL_DEFAULT): five translation units had five
// singletons between them. That made the initialisation cost depend on which
// code path ran first, meant no call site could observe another one's failure,
// and left the guarantee that a later setenv-free environment still reaches one
// initialised libcurl up to whichever translation unit happened to link.
//
// Returns CURLE_OK once libcurl is ready, and the same failure to every later
// caller. Idempotent and safe to call from any thread.
CURLcode InitialiseCurl();

// RAII owner for a CURL*. A default-constructed handle is empty and owns
// nothing, so the null case needs no special handling at the call site.
class EasyHandle final {
 public:
  EasyHandle() = default;
  ~EasyHandle() { reset(); }

  EasyHandle(const EasyHandle&) = delete;
  EasyHandle& operator=(const EasyHandle&) = delete;

  EasyHandle(EasyHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  EasyHandle& operator=(EasyHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  // Initialises libcurl first, so a failed initialisation surfaces as an empty
  // handle rather than as a libcurl call that returns garbage.
  explicit EasyHandle(bool initialise_curl);

  CURL* get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

  void reset();

 private:
  CURL* handle_ = nullptr;
};

// RAII owner for a curl_slist built from a list of header lines.
class HeaderList final {
 public:
  // `allocated` reports whether every header was accepted. A failed allocation
  // leaves the list empty, so callers can surface the failure and skip the
  // transfer instead of sending a header-less request.
  explicit HeaderList(const std::vector<std::string>& headers);
  HeaderList(const HeaderList&) = delete;
  HeaderList& operator=(const HeaderList&) = delete;
  ~HeaderList();

  curl_slist* get() const { return headers_; }
  bool allocated() const { return allocated_; }

 private:
  curl_slist* headers_ = nullptr;
  bool allocated_ = true;
};

// RAII owner for a CURLU*, the URL parser used to validate redirect targets.
class UrlHandle final {
 public:
  UrlHandle();
  UrlHandle(const UrlHandle&) = delete;
  UrlHandle& operator=(const UrlHandle&) = delete;
  ~UrlHandle();

  CURLU* get() const { return url_; }
  explicit operator bool() const { return url_ != nullptr; }

 private:
  CURLU* url_ = nullptr;
};

// RAII owner for a char* that libcurl allocated and that must go back through
// curl_free.
class OwnedCString final {
 public:
  OwnedCString() = default;
  explicit OwnedCString(char* value) : value_(value) {}
  OwnedCString(const OwnedCString&) = delete;
  OwnedCString& operator=(const OwnedCString&) = delete;
  ~OwnedCString();

  OwnedCString(OwnedCString&& other) noexcept : value_(other.value_) {
    other.value_ = nullptr;
  }
  OwnedCString& operator=(OwnedCString&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }

  const char* get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

  void reset();

 private:
  char* value_ = nullptr;
};

// The transport settings that legitimately differ per call site. Everything
// else these transfers have in common is applied by ApplyTransportOptions.
struct TransportOptions {
  const char* user_agent = "Mocktail/1";
  long connect_timeout_ms = 5000;
  long timeout_ms = 15000;
  // The updater follows redirects itself so it can re-validate every hop's
  // host, so redirect following is opt-in rather than always on.
  bool follow_redirects = true;
  long maximum_redirects = 5;
};

// Applies the transport baseline that every caller in this tree already set by
// hand, so the safety properties live in one place:
//
//   * HTTPS only, for the request and for any redirect, so a redirect cannot
//     downgrade the scheme.
//   * CURLOPT_NOSIGNAL, because these transfers run on threads that must not
//     take signals.
//   * Transparent gzip.
//   * MOCKTAIL_CA_BUNDLE when configured, which is how a user points the client
//     at a custom trust store.
//
// Returns CURLE_OK when every option was accepted.
CURLcode ApplyTransportOptions(CURL* handle, const TransportOptions& options);

// One copy of the bounded append that every response body writer here had
// duplicated.
//
// The bound is enforced with `size > maximum - bytes` rather than with
// `size + bytes > maximum`, so an oversized or malicious `size * count` cannot
// wrap the addition and slip past the limit.
struct BoundedStringSink {
  std::string* output = nullptr;
  std::size_t maximum = 0;
  bool exceeded = false;
};

// Curl-compatible write callback for BoundedStringSink. Returning short tells
// curl to abort the transfer, which is how the limit is enforced.
std::size_t WriteBoundedString(char* data, std::size_t size, std::size_t count,
                               void* opaque);

}  // namespace net
}  // namespace mocktail

#endif  // MOCKTAIL_NET_CURL_SUPPORT_H_