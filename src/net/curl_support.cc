#include "net/curl_support.h"

#include <cstdlib>
#include <limits>
#include <mutex>

namespace mocktail {
namespace net {
namespace {

std::once_flag g_curl_init_once;
CURLcode g_curl_init_result = CURLE_FAILED_INIT;

void InitialiseCurlOnce() {
  g_curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

}  // namespace

CURLcode InitialiseCurl() {
  std::call_once(g_curl_init_once, InitialiseCurlOnce);
  return g_curl_init_result;
}

EasyHandle::EasyHandle(bool initialise_curl) {
  if (initialise_curl && InitialiseCurl() != CURLE_OK) {
    return;
  }
  handle_ = curl_easy_init();
}

void EasyHandle::reset() {
  if (handle_ != nullptr) {
    curl_easy_cleanup(handle_);
    handle_ = nullptr;
  }
}

HeaderList::HeaderList(const std::vector<std::string>& headers) {
  for (const std::string& header : headers) {
    curl_slist* next = curl_slist_append(headers_, header.c_str());
    if (next == nullptr) {
      if (headers_ != nullptr) {
        curl_slist_free_all(headers_);
        headers_ = nullptr;
      }
      allocated_ = false;
      return;
    }
    headers_ = next;
  }
}

HeaderList::~HeaderList() {
  if (headers_ != nullptr) {
    curl_slist_free_all(headers_);
    headers_ = nullptr;
  }
}

UrlHandle::UrlHandle() : url_(curl_url()) {}

UrlHandle::~UrlHandle() {
  if (url_ != nullptr) {
    curl_url_cleanup(url_);
    url_ = nullptr;
  }
}

OwnedCString::~OwnedCString() { reset(); }

void OwnedCString::reset() {
  if (value_ != nullptr) {
    curl_free(value_);
    value_ = nullptr;
  }
}

CURLcode ApplyTransportOptions(CURL* handle, const TransportOptions& options) {
  if (handle == nullptr) {
    return CURLE_BAD_FUNCTION_ARGUMENT;
  }
  if (curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK ||
      curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https") !=
          CURLE_OK ||
      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "") != CURLE_OK ||
      curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                       options.connect_timeout_ms) != CURLE_OK ||
      curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, options.timeout_ms) !=
          CURLE_OK ||
      curl_easy_setopt(handle, CURLOPT_USERAGENT, options.user_agent) !=
          CURLE_OK) {
    return CURLE_FAILED_INIT;
  }
  if (curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION,
                       options.follow_redirects ? 1L : 0L) != CURLE_OK) {
    return CURLE_FAILED_INIT;
  }
  // MAXREDIRS is only meaningful while libcurl is following; leaving it unset
  // in the manual-redirect case keeps the two configurations distinguishable.
  if (options.follow_redirects &&
      curl_easy_setopt(handle, CURLOPT_MAXREDIRS,
                       options.maximum_redirects) != CURLE_OK) {
    return CURLE_FAILED_INIT;
  }
  if (const char* ca_bundle = std::getenv("MOCKTAIL_CA_BUNDLE")) {
    if (curl_easy_setopt(handle, CURLOPT_CAINFO, ca_bundle) != CURLE_OK) {
      return CURLE_FAILED_INIT;
    }
  }
  return CURLE_OK;
}

std::size_t WriteBoundedString(char* data, std::size_t size, std::size_t count,
                               void* opaque) {
  auto* sink = static_cast<BoundedStringSink*>(opaque);
  if (sink == nullptr || sink->output == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<std::size_t>::max() / count)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > sink->maximum ||
      sink->output->size() > sink->maximum - bytes) {
    sink->exceeded = true;
    return 0;
  }
  sink->output->append(data, bytes);
  return bytes;
}

}  // namespace net
}  // namespace mocktail