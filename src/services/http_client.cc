#include "services/http_client.h"

#include <curl/curl.h>

#include <array>
#include <limits>
#include <mutex>

#include "net/curl_support.h"
#include "runtime/runtime_config.h"
#include "runtime/system_proxy.h"

namespace mocktail {
namespace services {
namespace {

size_t WriteHeader(char* data, size_t size, size_t count, void* user_data) {
  auto* headers = static_cast<std::vector<std::string>*>(user_data);
  if (headers == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<size_t>::max() / count)) {
    return 0;
  }
  const size_t bytes = size * count;
  std::string header(data, bytes);
  while (!header.empty() && (header.back() == '\r' || header.back() == '\n')) {
    header.pop_back();
  }
  if (!header.empty()) headers->push_back(std::move(header));
  return bytes;
}

}  // namespace

HttpResponse PerformRequest(const HttpRequest& request,
                            const std::string* post_body) {
  HttpResponse response;
  if (request.url.empty()) {
    response.error = "HTTP request URL is empty";
    return response;
  }

  if (net::InitialiseCurl() != CURLE_OK) {
    response.error = curl_easy_strerror(CURLE_FAILED_INIT);
    return response;
  }

  const net::EasyHandle handle(true);
  if (!handle) {
    response.error = "curl_easy_init failed";
    return response;
  }

  // Both owners release on every path below, including the early returns, so
  // the manual cleanup this used to do in two places cannot be missed again.
  const net::HeaderList headers(request.headers);
  if (!headers.allocated()) {
    response.error = "could not allocate HTTP headers";
    return response;
  }

  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  net::BoundedStringSink writer{&response.body, request.maximum_body_bytes,
                                false};
  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  if (post_body == nullptr) {
    curl_easy_setopt(handle.get(), CURLOPT_HTTPGET, 1L);
  } else {
    curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, post_body->data());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(post_body->size()));
  }
  net::TransportOptions options;
  options.user_agent = "Mocktail/0.1";
  options.timeout_ms = request.timeout_ms > 0 ? request.timeout_ms : 15000L;
  options.follow_redirects = request.follow_redirects;
  options.maximum_redirects = 5;
  net::ApplyTransportOptions(handle.get(), options);
  curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, net::WriteBoundedString);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &writer);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, WriteHeader);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &response.headers);
  runtime::ApplyConfiguredProxy(handle.get());
  if (headers.get() != nullptr) {
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  }

  const CURLcode result = curl_easy_perform(handle.get());
  if (result == CURLE_OK) {
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE,
                      &response.status_code);
    response.transport_ok = true;
  } else if (writer.exceeded) {
    response.error = "HTTP response exceeded configured size limit";
  } else if (error_buffer[0] != '\0') {
    response.error = error_buffer.data();
  } else {
    response.error = curl_easy_strerror(result);
  }

  return response;
}

HttpResponse CurlHttpClient::Get(const HttpRequest& request) {
  return PerformRequest(request, nullptr);
}

HttpResponse CurlHttpClient::Post(const HttpRequest& request,
                                  const std::string& body) {
  return PerformRequest(request, &body);
}

}  // namespace services
}  // namespace mocktail
