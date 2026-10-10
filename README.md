# wp81Equities

A Win32 console application for **Windows Phone 8.1 (ARM32)** that scans a list
of Euronext equities with Yahoo Finance daily data and ranks them by a
short-term momentum score.

For each tracked ticker it fetches
the daily chart over HTTPS, prints the last close, the daily change, the
multi-day change and the up-day streak, then sorts and groups the results by
score class. The streak is carried over between runs in
`euronext_storage.json`.

Output is plain ASCII (no ANSI colors, no emoji) so it reads cleanly in a
phone telnet session.

## Features

- Full scan of the tickers listed in `euronext_storage.json` (see
  [Adding or removing tickers](#adding-or-removing-tickers)).
- Held equities (with an Average Purchase Price, `app`) are listed first,
  with their gain or loss vs the last close: gains up today, then losses up
  today, then the ones flat or down today (each group sorted by score).
- The other tickers are sorted by score and grouped into classes:
  STRONG SHORT-TERM (75+), INTERESTING (65+), WATCH (55+), NEUTRAL (45+), WEAK.
- Rebound score (R, 20-95) shown beside the main score: high after a large
  4-day fall, higher still when today is up. It does not affect sorting.
- Up-day streak with an ALERT from 2 consecutive up days, continued from the
  storage file.
- Optional JSON output of the scan (`-j`), see below.
- Detailed view of a single ticker or of any URL.
- Offline mode: parse a previously saved response (`-i`).
- Certificate diagnosis mode (`--diag`) to find which TLS checks fail on the
  phone.

## Usage

```
wp81Equities.exe [-s FILE] [-n] [-j FILE] [-k | -u]
    scan all tracked tickers, sorted by short-term score
wp81Equities.exe [-s FILE] [-n] [-o FILE] [-k | -u] TICKER | URL
    show one ticker in detail
wp81Equities.exe [-s FILE] [-n] -i FILE
    show a saved response in detail (no network)
wp81Equities.exe --diag [TICKER | URL]
    try every combination of ignored certificate checks (default: AIR.PA)
wp81Equities.exe -h | --help
```

| Option    | Meaning                                                  |
|-----------|----------------------------------------------------------|
| `-s FILE` | use FILE as the storage instead of `euronext_storage.json` |
| `-n`      | do not update the storage                                |
| `-j FILE` | scan: also write the ranked list to FILE as JSON  |
| `-o FILE` | save the response body to FILE                           |
| `-i FILE` | parse a saved response instead of fetching               |
| `-k`      | ignore all certificate errors (diagnosis only, unsafe)   |
| `-u`      | ignore only "unknown CA" certificate errors              |

Examples:

```
wp81Equities.exe
wp81Equities.exe -j scan.json
wp81Equities.exe AIR.PA
wp81Equities.exe -o airbus.json AIR.PA
wp81Equities.exe -n -i airbus.json
```

Exit codes: `0` success, `1` failure (scan: at least one ticker failed),
`2` invalid arguments.

## JSON output

`-j FILE` writes the scan result, with the same content as the console:

```json
{
  "generated": "2026-10-09T17:45:12Z",
  "stocks": 54,
  "alerts": 3,
  "failed": 1,
  "ranked": [
    {
      "rank": 1,
      "symbol": "AIR.PA",
      "name": "Airbus SE",
      "categories": ["Aerospace", "Defense"],
      "group": "HELD: GAIN, UP TODAY",
      "app": 172.4,
      "gain": 24.1,
      "gain_pct": 13.98,
      "score": 78.4,
      "class": "STRONG SHORT-TERM",
      "class_range": "75+",
      "rebound": 20,
      "streak": 3,
      "alert": true,
      "currency": "EUR",
      "close": 196.5,
      "change": 2.1,
      "change_pct": 1.08,
      "span_days": 4,
      "span_change": 5.3,
      "span_pct": 2.77,
      "session_in_progress": false
    }
  ],
  "not_scored": [
    { "symbol": "XYZ.PA", "problem": "WARNING: XYZ.PA: no market data available" }
  ]
}
```

Prices and percentages are rounded to 2 decimals and the score to 1, as
printed on the console.

Top level:

| Property     | Type   | Meaning                                                        |
|--------------|--------|----------------------------------------------------------------|
| `generated`  | string | time the file was written, UTC, ISO 8601 (phone clock)         |
| `stocks`     | number | tickers in the scan (the records of the storage file)          |
| `alerts`     | number | scored tickers with an up-day streak of 2 or more              |
| `failed`     | number | tickers that could not be scored (listed in `not_scored`)      |
| `ranked`     | array  | scored tickers, by `group` then by score, best first (see below) |
| `not_scored` | array  | tickers that could not be scored, in storage file order        |

Each entry of `ranked`:

| Property              | Type           | Meaning                                                                 |
|-----------------------|----------------|-------------------------------------------------------------------------|
| `rank`                | number         | position in the list, from 1; ties keep the storage file order          |
| `symbol`              | string         | Yahoo Finance symbol, e.g. `AIR.PA`                                     |
| `name`                | string         | company name from Yahoo (long name, else short name, else symbol)       |
| `categories`          | array          | categories from the storage file (up to 8)                              |
| `group`               | string         | first sort key: `HELD: GAIN, UP TODAY`, `HELD: LOSS, UP TODAY`, `HELD: FLAT OR DOWN TODAY`, `NOT HELD` |
| `app`                 | number or null | Average Purchase Price from the storage file, `null` when not held      |
| `gain`                | number or null | `close` minus `app` (negative: loss), per share                         |
| `gain_pct`            | number or null | `gain` in percent of `app`                                              |
| `score`               | number         | short-term momentum score, 0-100: the sort key within a group           |
| `class`               | string         | score class: `STRONG SHORT-TERM`, `INTERESTING`, `WATCH`, `NEUTRAL`, `WEAK` |
| `class_range`         | string         | score range of the class: `75+`, `65-75`, `55-65`, `45-55`, `<45`       |
| `rebound`             | number         | rebound score, 20-95: high after a large multi-day fall, more when today is up; not used to sort |
| `streak`              | number         | consecutive up days (finished sessions only), continued from the storage file |
| `alert`               | boolean        | `true` when `streak` is 2 or more                                       |
| `currency`            | string         | currency of the prices, e.g. `EUR`                                      |
| `close`               | number         | last price                                                              |
| `change`              | number         | `close` minus the previous close                                        |
| `change_pct`          | number         | `change` in percent of the previous close                               |
| `span_days`           | number or null | sessions covered by the multi-day change (usually 4)                    |
| `span_change`         | number or null | `close` minus the close `span_days` sessions earlier                    |
| `span_pct`            | number or null | `span_change` in percent of that earlier close                          |
| `session_in_progress` | boolean        | `true` while today's session is still open (see below)                  |

The `span_*` properties are `null` when Yahoo returned a single close.

When `session_in_progress` is `true`, `close` is a live price, not a final
close: `change`, `change_pct`, the `span_*` values, `score` and `rebound`
are provisional and will move until the market closes. The streak ignores
that price and only counts finished sessions, so during market hours it can
lag one day behind the other values; today is counted on a run after the
close.

Each entry of `not_scored`:

| Property  | Type   | Meaning                                                             |
|-----------|--------|---------------------------------------------------------------------|
| `symbol`  | string | Yahoo Finance symbol                                                |
| `problem` | string | the console message, `ERROR: ...` (request or parsing failed) or `WARNING: ...` (no market data) |

## How HTTPS works on WP8.1

The WP8.1 SDK provides no import library for WinINet or WinHTTP. The app uses
MSXML6 `IXMLHTTPRequest2` (`FreeThreadedXMLHTTP60`), which can be created with
`CoCreateInstance` from a plain console exe. HTTP, redirects, chunked encoding
and gzip are handled by the OS network stack, and TLS 1.2 by Schannel. If COM
activation fails, the app loads `msxml6.dll` and calls its class factory
directly.

Yahoo's certificate chains to DigiCert Global Root G2, which the phone already
trusts, so no certificate needs installing.

**Certificate errors (`INET_E_INVALID_CERTIFICATE`, 0x800C0019) are almost
always caused by a wrong phone clock.** Yahoo's certificates last about seven
weeks, so a clock a few days behind sees a freshly renewed certificate as not
yet valid. Check the date and time in Settings first, then run `--diag`.

The app would stop working if Yahoo ever required TLS 1.3.

## Building

Requirements (paths are hard-coded in `CMakeLists.txt` and `CMakePresets.json`):

- Windows Phone 8.1 SDK
  (`C:/Program Files (x86)/Windows Phone Kits/8.1`)
- Visual Studio 2012 WP SDK headers and libraries
  (`C:/Program Files (x86)/Microsoft Visual Studio 11.0/VC/WPSDK`)
- LLVM (`clang-cl` and `lld-link` in `C:/Program Files/LLVM/bin`)
- CMake 3.20+ and Ninja

```
cd wp81Equities
cmake --preset arm32-windows
cmake --build build
```

This produces `build/wp81Equities.exe`, an ARM32 console executable linked
against `msvcr110.dll`. Copy it to the phone and run it from a console
session (for example over telnet).

Note: the SDK's `MSXML6.lib` is built with `/GL` and cannot be linked by
`lld-link`, so CLSIDs and IIDs come from the headers via `__uuidof()`.

## Project layout

```
wp81Equities/
  CMakeLists.txt        build definition (all src/*.cpp and src/*.c)
  CMakePresets.json     arm32-windows preset (clang-cl, lld-link, Ninja)
  src/
    wp81Equities.cpp    entry point, HTTP requests, scan and display
    yahoo_chart.cpp/.h  parsing of the Yahoo Finance chart JSON
    tracker.cpp/.h      price summary, streak, score and rebound score
    storage.cpp/.h      reading and writing euronext_storage.json
    byte_buffer.h       growable byte buffer for response bodies
    cJSON/              cJSON library (MIT, see its LICENSE)
```

`tracker.cpp` and `yahoo_chart.cpp` depend only on the CRT (and cJSON), so the scoring
and parsing logic can also be compiled and tested on a PC.

## Adding or removing tickers

The scanned tickers are the records of `euronext_storage.json` (next to the
exe, or the file given with `-s`). Edit it by hand; no rebuild is needed.
Each key is a Yahoo Finance symbol (e.g. `AIR.PA`, `ABN.AS`, `DEME.BR`). A new
record only needs its categories; the scan fills in the other properties:

```json
{
    "NEW.PA": { "categories": [ "industrials", "small cap" ] },
    "AIR.PA": { "categories": [ "aerospace", "defense" ], "app": 172.4 }
}
```

`app` (optional) is the Average Purchase Price of a held equity. The scan
keeps `categories` and `app` as they are. To stop tracking a ticker, delete
its record. Showing a single ticker that is not in the file does not add it.
