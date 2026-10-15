# getgate

`getgate` prints the departure gate for one flight leaving London Heathrow (LHR) today. It downloads [Heathrow's departures page](https://www.heathrow.com/departures), searches that page and its JavaScript for same-origin flight-data URLs, then requests departures from the chosen London-local time. If a flight has no published gate, it can estimate one from a Terminal 5 stand number.

The single line on stdout is the gate (for example `A10`). A stand-derived gate is marked as an estimate on stderr. Errors return a nonzero status and write a diagnostic to stderr. Gate assignments can change; check Heathrow's display or your boarding pass before going to a gate.

## Build

Requires a C++23 compiler, `make`, and the libcurl development headers and library. The runtime also needs DNS access, HTTPS connectivity to heathrow.com, and the system's `Europe/London` timezone data. Python 3 is needed only for tests.

```sh
make
make test
```

The executable is left at `./getgate` after building and testing.

## Usage

```sh
./getgate BA123
./getgate --time 14:00 BA123
./getgate --airport LHR BA123
./getgate --help
```

`--time HH:MM` selects the first departure time to consider **today in London**. Its default is `00:00`. `--airport` defaults to `LHR`; other airports are rejected until a data source and gate rules are added for them. The flight number is the only positional argument. Matching ignores letter case and spaces.

## Data and limits

Heathrow's public departures page loads the live board separately, so the tool discovers likely data URLs from the page and its scripts on each run. The exact live endpoint and response shape could not be verified from this development environment because direct DNS access to heathrow.com was unavailable. The local fixture tests cover page and script discovery, fetching board data, flight matching, time filtering, published gates, and stand estimates. If Heathrow changes its website or uses an endpoint that is not exposed as a same-origin URL in the page's scripts, the tool reports an error rather than inventing a gate.

The stand rule is an estimate for Terminal 5's A, B, and C gate groups: stands `501`–`523` map to `A1`–`A23`, `531`–`548` to `B31`–`B48`, and `551`–`568` to `C51`–`C68`. Stands outside these ranges have no inferred gate. A published gate always takes priority.

For local fixture testing, `GETGATE_PAGE_URL` can point at a `file://` departures page. Normal use always downloads `https://www.heathrow.com/departures`.

## License

Boost Software License 1.0; see [LICENSE](LICENSE).
