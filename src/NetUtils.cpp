// Shared HTTP utilities.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "NetUtils.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <memory>

namespace ut1
{
namespace
{
/// Receive response bytes; returning zero tells libcurl to abort when the limit is exceeded.
size_t receive(char* bytes, size_t size, size_t count, void* context)
{
    auto* state = static_cast<std::pair<std::string*, size_t>*>(context);
    size_t length = size * count;
    if (state->first->size() > state->second || length > state->second - state->first->size()) return 0;
    state->first->append(bytes, length);
    return length;
}

/// Convert ASCII text to lowercase for URL classification.
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
}

HttpResponse httpGet(const std::string& url, size_t maxBytes)
{
    CURL* handle = curl_easy_init();
    if (!handle) throw std::runtime_error("Unable to initialize libcurl");
    HttpResponse response;
    std::pair<std::string*, size_t> state{&response.body, maxBytes};
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "getgate/0.1 (+https://www.heathrow.com/departures)");
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &state);
    CURLcode code = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    curl_easy_cleanup(handle);
    if (code != CURLE_OK) throw std::runtime_error("HTTP GET " + url + ": " + curl_easy_strerror(code));
    if ((response.status < 200 || response.status >= 300) && !(url.starts_with("file://") && response.status == 0))
        throw std::runtime_error("HTTP GET " + url + ": status " + std::to_string(response.status));
    return response;
}

std::string resolveSameOriginUrl(const std::string& pageUrl, const std::string& candidate)
{
    auto schemeEnd = pageUrl.find("://");
    if (schemeEnd == std::string::npos) throw std::runtime_error("Invalid page URL");
    auto slash = pageUrl.find('/', schemeEnd + 3);
    std::string origin = pageUrl.substr(0, slash);
    std::string url;
    if (candidate.starts_with("https://")) url = candidate;
    else if (candidate.starts_with("//")) url = pageUrl.substr(0, schemeEnd) + ":" + candidate;
    else if (candidate.starts_with('/')) url = origin + candidate;
    else if (!candidate.empty() && candidate[0] != '#' && candidate[0] != '?') url = pageUrl.substr(0, pageUrl.rfind('/') + 1) + candidate;
    if (!url.starts_with(origin + "/") && url != origin) throw std::runtime_error("URL is outside page origin");
    return url;
}

std::vector<std::string> discoverFlightUrls(const std::string& pageUrl, const std::string& source, bool scripts)
{
    std::vector<std::string> urls;
    // Both HTML attributes and quoted JS string literals are included. Keep URLs on the page's origin.
    const std::regex quoted(R"re(["']([^"'<>\s]+)["'])re");
    for (std::sregex_iterator it(source.begin(), source.end(), quoted), end; it != end; ++it)
    {
        std::string candidate = (*it)[1];
        std::string key = lower(candidate);
        bool wanted = scripts ? (key.ends_with(".js") || key.find(".js?") != std::string::npos)
                              : ((key.find("flight") != std::string::npos || key.find("depart") != std::string::npos)
                                  && (candidate.find('/') != std::string::npos || key.ends_with(".json")));
        if (!wanted) continue;
        try
        {
            auto url = resolveSameOriginUrl(pageUrl, candidate);
            if (std::find(urls.begin(), urls.end(), url) == urls.end()) urls.push_back(url);
        }
        catch (const std::runtime_error&) { }
    }
    return urls;
}

namespace
{
/// Read a URL component and release libcurl's allocated result.
std::string urlPart(CURLU* url, CURLUPart part)
{
    char* value = nullptr;
    if (curl_url_get(url, part, &value, 0) != CURLUE_OK) return {};
    std::string result(value);
    curl_free(value);
    return result;
}

/// Build a validated HTTP URL handle.
std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsedHttpUrl(const std::string& value)
{
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("Invalid URL: " + value);
    std::string scheme = lower(urlPart(url.get(), CURLUPART_SCHEME));
    if ((scheme != "http" && scheme != "https") || urlPart(url.get(), CURLUPART_HOST).empty()
        || !urlPart(url.get(), CURLUPART_USER).empty() || !urlPart(url.get(), CURLUPART_PASSWORD).empty())
        throw std::runtime_error("Expected an HTTP(S) URL without embedded credentials: " + value);
    return url;
}

/// Track response body size and overflow.
struct BodyState
{
    /// Response bytes received so far.
    std::string* body{};
    /// Maximum accepted response bytes.
    size_t limit{};
    /// Whether the size limit was reached.
    bool overflow{};
};

/// Receive a bounded response body.
size_t receiveBounded(char* bytes, size_t size, size_t count, void* context)
{
    auto& state = *static_cast<BodyState*>(context);
    size_t length = size * count;
    if (length > state.limit - state.body->size()) { state.overflow = true; return 0; }
    state.body->append(bytes, length);
    return length;
}

/// Track redirect targets from HTTP response headers.
struct HeaderState
{
    /// Original request URL, or the latest redirect target.
    std::string currentUrl;
    /// URLs found in Location headers.
    std::vector<std::string>* redirects{};
};

/// Read Location headers from each redirect response.
size_t receiveHeader(char* bytes, size_t size, size_t count, void* context)
{
    auto& state = *static_cast<HeaderState*>(context);
    size_t length = size * count;
    std::string line(bytes, length);
    if (lower(line).starts_with("location:"))
    {
        std::string target = line.substr(9);
        auto first = target.find_first_not_of(" \t");
        auto last = target.find_last_not_of(" \t\r\n");
        if (first != std::string::npos)
        {
            try
            {
                state.currentUrl = resolveHttpUrl(state.currentUrl, target.substr(first, last - first + 1));
                state.redirects->push_back(state.currentUrl);
            }
            catch (const std::runtime_error&) { }
        }
    }
    return length;
}
}

std::string resolveHttpUrl(const std::string& baseUrl, const std::string& reference)
{
    auto url = parsedHttpUrl(baseUrl);
    if (curl_url_set(url.get(), CURLUPART_URL, reference.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("Invalid URL reference: " + reference);
    curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0);
    parsedHttpUrl(urlPart(url.get(), CURLUPART_URL));
    return urlPart(url.get(), CURLUPART_URL);
}

std::string httpUrlHostname(const std::string& value)
{
    auto url = parsedHttpUrl(value);
    return lower(urlPart(url.get(), CURLUPART_HOST));
}

std::string httpUrlOrigin(const std::string& value)
{
    auto url = parsedHttpUrl(value);
    std::string scheme = lower(urlPart(url.get(), CURLUPART_SCHEME));
    std::string port = urlPart(url.get(), CURLUPART_PORT);
    if (port.empty()) port = scheme == "https" ? "443" : "80";
    return scheme + "://" + lower(urlPart(url.get(), CURLUPART_HOST)) + ":" + port;
}

DetailedHttpResponse httpGetDetailed(const std::string& value, const HttpRequestOptions& options)
{
    parsedHttpUrl(value);
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Unable to initialize libcurl");
    DetailedHttpResponse response;
    BodyState body{&response.body, options.maxBytes, false};
    HeaderState headers{value, &response.redirectUrls};
    curl_easy_setopt(handle.get(), CURLOPT_URL, value.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "getpageurls/1.0");
    curl_easy_setopt(handle.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, receiveBounded);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, receiveHeader);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    if (!options.user.empty())
    {
        curl_easy_setopt(handle.get(), CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(handle.get(), CURLOPT_USERNAME, options.user.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_PASSWORD, options.password.c_str());
    }
    CURLcode code = curl_easy_perform(handle.get());
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response.status);
    char* effective = nullptr;
    curl_easy_getinfo(handle.get(), CURLINFO_EFFECTIVE_URL, &effective);
    response.effectiveUrl = effective ? effective : value;
    char* contentType = nullptr;
    curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_TYPE, &contentType);
    if (contentType) response.contentType = lower(std::string(contentType).substr(0, std::string(contentType).find(';')));
    if (body.overflow) throw std::runtime_error("Response exceeds size limit: " + value);
    if (code != CURLE_OK) throw std::runtime_error("HTTP GET " + value + ": " + curl_easy_strerror(code));
    return response;
}
}
