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
}
