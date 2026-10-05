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

- Full scan of the tracked tickers (see `src/equities.cpp`), sorted by score
  and grouped into classes:
  STRONG SHORT-TERM (75+), INTERESTING (65+), WATCH (55+), NEUTRAL (45+), WEAK.
- Rebound score (R, 20-95) shown beside the main score: high after a large
  4-day fall, higher still when today is up. It does not affect sorting.
- Up-day streak with an ALERT from 2 consecutive up days, continued from the
  storage file.
- Detailed view of a single ticker or of any URL.
- Offline mode: parse a previously saved response (`-i`).
- Certificate diagnosis mode (`--diag`) to find which TLS checks fail on the
  phone.

## Usage

```
wp81Equities.exe [-s FILE] [-n] [-k | -u]
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
| `-o FILE` | save the response body to FILE                           |
| `-i FILE` | parse a saved response instead of fetching               |
| `-k`      | ignore all certificate errors (diagnosis only, unsafe)   |
| `-u`      | ignore only "unknown CA" certificate errors              |

Examples:

```
wp81Equities.exe
wp81Equities.exe AIR.PA
wp81Equities.exe -o airbus.json AIR.PA
wp81Equities.exe -n -i airbus.json
```

Exit codes: `0` success, `1` failure (scan: at least one ticker failed),
`2` invalid arguments.

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
    equities.cpp/.h     list of tracked tickers and their categories
    yahoo_chart.cpp/.h  parsing of the Yahoo Finance chart JSON
    tracker.cpp/.h      price summary, streak, score and rebound score
    storage.cpp/.h      reading and writing euronext_storage.json
    byte_buffer.h       growable byte buffer for response bodies
    cJSON/              cJSON library (MIT, see its LICENSE)
```

`tracker.cpp` and `yahoo_chart.cpp` depend only on the CRT (and cJSON), so the scoring
and parsing logic can also be compiled and tested on a PC.

## Adding or removing tickers

Edit the `EQUITIES` table in `src/equities.cpp`. Each entry is a Yahoo
Finance symbol (e.g. `AIR.PA`, `ABN.AS`, `DEME.BR`) with up to three
categories. Rebuild afterwards.
