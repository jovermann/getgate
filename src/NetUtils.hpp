// Shared HTTP utilities.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ut1
{
/// HTTP response body and status code.
struct HttpResponse
{
    /// Numeric HTTP status, or zero if no response was received.
    long status{};
    /// Response body, limited by the caller's maximum byte count.
    std::string body;
};

/// Download a URL with TLS verification, redirects, timeouts, and a response-size limit.
/// Throws std::runtime_error on transport failure, an HTTP error, or a body over maxBytes.
HttpResponse httpGet(const std::string& url, size_t maxBytes = 8 * 1024 * 1024);

/// Resolve an absolute or root-relative URL against a page URL.
/// Throws std::runtime_error if the result leaves the page's origin.
std::string resolveSameOriginUrl(const std::string& pageUrl, const std::string& candidate);

/// Extract same-origin script URLs and plausible flight data URLs from HTML or JS.
/// Returned URLs are unique and preserve discovery order.
std::vector<std::string> discoverFlightUrls(const std::string& pageUrl, const std::string& source, bool scripts);
}
