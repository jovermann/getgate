// getgate - Print the departure gate for a flight.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "CommandLineParser.hpp"
#include "NetUtils.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
/// JSON value used for flight-board responses.
struct Json
{
    /// JSON object properties.
    std::map<std::string, Json> object;
    /// JSON array elements.
    std::vector<Json> array;
    /// JSON string or number representation.
    std::string scalar;
};

/// Parse the JSON subset needed by the board, including nested objects and arrays.
class JsonParser
{
public:
    /// Construct a parser over an existing response body.
    explicit JsonParser(const std::string& input_) : input(input_) { }
    /// Parse one complete JSON document; throws for malformed data.
    Json parse() { auto value = parseValue(); space(); if (pos != input.size()) throw std::runtime_error("Trailing JSON data"); return value; }
private:
    /// Source text, retained by reference during parsing.
    const std::string& input;
    /// Current source offset.
    size_t pos{};
    /// Skip JSON whitespace.
    void space() { while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos; }
    /// Consume a required delimiter.
    void expect(char c) { space(); if (pos >= input.size() || input[pos++] != c) throw std::runtime_error("Malformed JSON"); }
    /// Parse a quoted JSON string and common escapes.
    std::string parseString()
    {
        expect('"'); std::string result;
        while (pos < input.size())
        {
            char c = input[pos++];
            if (c == '"') return result;
            if (c == '\\')
            {
                if (pos >= input.size()) break;
                c = input[pos++];
                if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
                else if (c == 'u') { if (pos + 4 > input.size()) break; pos += 4; c = '?'; }
            }
            result += c;
        }
        throw std::runtime_error("Unterminated JSON string");
    }
    /// Parse a JSON value recursively, rejecting unreasonable nesting.
    Json parseValue(unsigned depth = 0)
    {
        if (depth > 64) throw std::runtime_error("JSON nesting limit exceeded");
        space(); if (pos >= input.size()) throw std::runtime_error("Unexpected end of JSON");
        Json value; char c = input[pos];
        if (c == '{')
        {
            ++pos; space(); if (pos < input.size() && input[pos] == '}') { ++pos; return value; }
            do { std::string key = parseString(); expect(':'); value.object.emplace(key, parseValue(depth + 1)); space();
                 if (pos < input.size() && input[pos] == '}') { ++pos; return value; } expect(','); } while (true);
        }
        if (c == '[')
        {
            ++pos; space(); if (pos < input.size() && input[pos] == ']') { ++pos; return value; }
            do { value.array.push_back(parseValue(depth + 1)); space();
                 if (pos < input.size() && input[pos] == ']') { ++pos; return value; } expect(','); } while (true);
        }
        if (c == '"') { value.scalar = parseString(); return value; }
        size_t start = pos;
        while (pos < input.size() && input[pos] != ',' && input[pos] != '}' && input[pos] != ']' && !std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
        if (start == pos) throw std::runtime_error("Malformed JSON value");
        value.scalar = input.substr(start, pos - start);
        return value;
    }
};

/// Uppercase and remove whitespace from a flight number for exact matching.
std::string normalize(std::string value)
{
    std::string out;
    for (unsigned char c : value) if (!std::isspace(c)) out += static_cast<char>(std::toupper(c));
    return out;
}

/// Read the first nonempty scalar matching any property name.
std::string field(const Json& object, std::initializer_list<const char*> keys)
{
    for (auto key : keys)
    {
        auto it = object.object.find(key);
        if (it != object.object.end() && !it->second.scalar.empty() && it->second.scalar != "null") return it->second.scalar;
    }
    return {};
}

/// Find flight data objects recursively, including codeshare numbers in arrays.
void findFlights(const Json& value, const std::string& wanted, std::vector<const Json*>& matches)
{
    if (!value.object.empty())
    {
        std::string number = field(value, {"flightNumber", "flight_number", "flightNo", "flightCode", "flight", "iata", "flight_iata"});
        std::string airline = field(value, {"airlineCode", "airline_code", "airlineIata", "airline_iata"});
        if (normalize(number) == wanted || normalize(airline + number) == wanted) matches.push_back(&value);
        for (const auto& [key, child] : value.object) { (void)key; findFlights(child, wanted, matches); }
    }
    for (const auto& child : value.array) findFlights(child, wanted, matches);
}

/// Extract HH:MM from a local board time or a dated local timestamp.
std::string localTime(const Json& flight)
{
    std::string time = field(flight, {"timeScheduled", "scheduledTime", "departureTime", "scheduled", "dep_time"});
    static const std::regex hhmm(R"((?:^|[ T])(\d\d:\d\d))");
    std::smatch match;
    if (std::regex_search(time, match, hhmm)) return match[1];
    return {};
}

/// Derive an estimated Terminal 5 A gate from a known contiguous stand range.
std::string gateFromStand(const std::string& stand)
{
    if (!std::regex_match(stand, std::regex(R"(5\d\d)"))) return {};
    int number = std::stoi(stand);
    if (number >= 501 && number <= 523) return "A" + std::to_string(number - 500);
    if (number >= 531 && number <= 548) return "B" + std::to_string(number - 500);
    if (number >= 551 && number <= 568) return "C" + std::to_string(number - 500);
    return {};
}

/// Format today's date at Heathrow, regardless of the machine's local timezone.
std::string heathrowDate()
{
    setenv("TZ", "Europe/London", 1); tzset();
    std::time_t now = std::time(nullptr); std::tm local{};
    if (!localtime_r(&now, &local)) throw std::runtime_error("Cannot determine Heathrow date");
    char date[16]; if (!std::strftime(date, sizeof(date), "%Y-%m-%d", &local)) throw std::runtime_error("Cannot format date");
    return date;
}

/// Validate a 24-hour HH:MM option.
bool validTime(const std::string& time)
{
    if (!std::regex_match(time, std::regex(R"(\d\d:\d\d)"))) return false;
    return std::stoi(time.substr(0, 2)) < 24 && std::stoi(time.substr(3, 2)) < 60;
}

/// Add today's date and selected time to a discovered board URL.
std::string boardUrl(std::string url, const std::string& date, const std::string& time)
{
    auto replace = [&](const std::string& marker, const std::string& value) {
        size_t pos = url.find(marker); if (pos != std::string::npos) url.replace(pos, marker.size(), value);
    };
    replace("{date}", date); replace("{time}", time);
    std::string separator = url.find('?') == std::string::npos ? "?" : "&";
    if (url.find("date=") == std::string::npos) { url += separator + "date=" + date; separator = "&"; }
    if (url.find("time=") == std::string::npos) url += separator + "time=" + time.substr(0, 2) + "%3A" + time.substr(3);
    return url;
}

/// Get candidate data URLs from the page and its same-origin JavaScript files.
std::vector<std::string> findEndpoints(const std::string& page, const std::string& html)
{
    auto urls = ut1::discoverFlightUrls(page, html, false);
    auto scripts = ut1::discoverFlightUrls(page, html, true);
    for (size_t i = 0; i < scripts.size() && i < 64; ++i)
    {
        try
        {
            auto more = ut1::discoverFlightUrls(page, ut1::httpGet(scripts[i], 4 * 1024 * 1024).body, false);
            for (const auto& url : more) if (std::find(urls.begin(), urls.end(), url) == urls.end()) urls.push_back(url);
        }
        catch (const std::exception&) { }
    }
    return urls;
}

/// Run the command and print the resolved gate or a useful failure.
int run(int argc, char* argv[])
{
    const char* usage = "Print today's Heathrow departure gate for a flight.\n\n"
                        "Usage: $programName [OPTIONS] FLIGHT\n\n"
                        "Examples:\n  $programName BA123\n  $programName --time 14:00 BA123";
    ut1::CommandLineParser cl("getgate", usage,
        "\n$programName version $version ($compileDate) *** Copyright (c) 2026 Johannes Overmann *** https://github.com/jovermann/getgate", "0.1.0");
    cl.addOption('a', "airport", "Airport IATA code (currently LHR only).", "CODE", "LHR");
    cl.addOption('t', "time", "Start of today's departure search, Heathrow local time.", "HH:MM", "00:00");
    cl.parse(argc, argv);
    if (cl.getArgs().size() != 1) cl.error("Expected exactly one flight number");
    if (normalize(cl.getStr("airport")) != "LHR") cl.error("Only LHR is supported");
    if (!validTime(cl.getStr("time"))) cl.error("--time must be HH:MM in 24-hour format");
    std::string wanted = normalize(cl.getArgs()[0]);
    if (!std::regex_match(wanted, std::regex(R"([A-Z0-9]{2,4}[0-9]{1,5}[A-Z]?)"))) cl.error("Invalid flight number");

    const char* pageOverride = std::getenv("GETGATE_PAGE_URL");
    const std::string page = pageOverride ? pageOverride : "https://www.heathrow.com/departures";
    std::string html = ut1::httpGet(page).body;
    auto endpoints = findEndpoints(page, html);
    if (endpoints.empty()) throw std::runtime_error("Could not discover a public departures data endpoint on Heathrow's page");
    std::string date = heathrowDate();
    bool boardParsed = false;
    bool flightFound = false;
    for (const auto& endpoint : endpoints)
    {
        try
        {
            std::string data = ut1::httpGet(boardUrl(endpoint, date, cl.getStr("time"))).body;
            Json root = JsonParser(data).parse();
            boardParsed = true;
            std::vector<const Json*> matches;
            findFlights(root, wanted, matches);
            for (const Json* flight : matches)
            {
                std::string flightDate = field(*flight, {"localDate", "departureDate", "scheduledDate", "date"});
                if (!flightDate.empty() && flightDate != date) continue;
                std::string scheduled = localTime(*flight);
                if (!scheduled.empty() && scheduled < cl.getStr("time")) continue;
                flightFound = true;
                std::string gate = field(*flight, {"gate", "gateNumber", "departureGate", "dep_gate"});
                if (gate.empty())
                {
                    std::string stand = field(*flight, {"stand", "standNumber", "departureStand", "dep_stand"});
                    gate = gateFromStand(stand);
                    if (!gate.empty()) std::cerr << "getgate: estimated from stand " << stand << "\n";
                }
                if (!gate.empty()) { std::cout << gate << '\n'; return 0; }
            }
        }
        catch (const std::exception&) { continue; }
    }
    if (flightFound) throw std::runtime_error("Flight found, but no gate or mappable stand is published");
    if (boardParsed) throw std::runtime_error("Flight not found in today's departures after the selected time");
    throw std::runtime_error("Discovered endpoints did not return usable departures data");
}
}

/// Program entry point; keep diagnostics on stderr and output machine-readable.
int main(int argc, char* argv[])
{
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "getgate: " << error.what() << '\n'; return 1; }
}
