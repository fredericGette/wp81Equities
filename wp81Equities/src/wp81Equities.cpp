/**
 * Context:
 * win32 console application,
 * linked with Msvcr110.dll,
 * architecture ARM 32bit little endian.
 * Use secure functions when possible, example: _snprintf_s instead of _snprintf
 *
 * Port of euronext_tracker.py: for each ticker of EQUITIES (equities.cpp),
 * fetch the Yahoo Finance daily chart over HTTPS with MSXML6
 * IXMLHTTPRequest2, parse it with cJSON and print the last close, daily
 * change, multi-day change and up-day streak (ALERT from 2 days). The
 * streak is continued from euronext_storage.json (same file and format as
 * the Python script), which is then updated. The scan is sorted by a
 * short-term momentum score (tracker.cpp, ComputeScore) and grouped by
 * class. Plain ASCII output: no ANSI colors, no emoji. A single ticker can
 * also be shown in detail.
 *
 * Why MSXML6 IXMLHTTPRequest2 for HTTPS:
 * - The WP8.1 SDK offers no import library for WinINet or WinHTTP, only
 *   Winsock (ws2_32) and MSXML6. With Winsock we would have to write HTTP/1.1
 *   (chunked encoding, redirects, gzip) and drive Schannel for TLS by hand,
 *   or bundle a TLS library and its own CA certificates.
 * - IXMLHTTPRequest2 is the HTTP client Windows 8 / WP8.1 exposes to store
 *   apps, and its COM class (FreeThreadedXMLHTTP60) is also registered for
 *   plain Win32 processes: CoCreateInstance works from this console exe.
 *   It does HTTP/1.1, TLS, redirects, chunked encoding and gzip for us.
 *
 * Why it still works with today's Yahoo despite its age:
 * - MSXML6 itself dates from 2005, but IXMLHTTPRequest2 was added with
 *   Windows 8 (2012). It is only an API: the transfer is done by the OS
 *   network stack (WinINet, hence the INET_E_* / 12xxx error codes) and TLS
 *   by the OS Schannel, which on WP8.1 (Windows 8.1 core) supports TLS 1.2,
 *   SNI, ECDHE key exchange and AES-GCM.
 * - query1.finance.yahoo.com still accepts TLS 1.2 with suites Schannel
 *   knows (checked 2026-10: ECDHE-RSA-AES128-GCM-SHA256, AES128-GCM-SHA256,
 *   AES128-SHA256...). It would stop working if Yahoo ever required TLS 1.3.
 * - Yahoo's certificate chains to DigiCert Global Root G2 (2013), which is
 *   already in the phone's root store: no certificate has to be installed.
 * - The phone clock must be correct: Yahoo's certificates last ~7 weeks, so
 *   a clock a few days late sees a freshly renewed certificate as "not yet
 *   valid" (INET_E_INVALID_CERTIFICATE). See CheckClockSkew() and --diag.
 *
 * Usage: see PrintUsage() or run "wp81Equities.exe --help".
 */

#include <windows.h>
#include <msxml6.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON/cJSON.h"
#include "byte_buffer.h"
#include "yahoo_chart.h"
#include "tracker.h"
#include "storage.h"
#include "equities.h"

// ─── Configuration ────────────────────────────────────────────────────────────
static const wchar_t* DEFAULT_TICKER     = L"AIR.PA";
#ifndef CHART_URL_FORMAT_OVERRIDE   // host-side tests point it to a local mock server
static const wchar_t* CHART_URL_FORMAT   = L"https://query1.finance.yahoo.com/v8/finance/chart/%s?range=10d&interval=1d";
#else
static const wchar_t* CHART_URL_FORMAT   = CHART_URL_FORMAT_OVERRIDE;
#endif
static const wchar_t* USER_AGENT         = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
static const DWORD    REQUEST_TIMEOUT_MS = 30 * 1000;
static const DWORD    WAIT_TIMEOUT_MS    = 45 * 1000;   // > REQUEST_TIMEOUT_MS
static const size_t   PREVIEW_BYTES      = 400;   // body shown when parsing fails
static const wchar_t* STORAGE_FILE_NAME  = L"euronext_storage.json";   // next to the exe
static const LONGLONG CLOCK_SKEW_WARNING_SECONDS = 5 * 60;
static const int      SCAN_ABORT_AFTER_FAILURES  = 3;   // failed requests, before any success, that stop a scan
// ─────────────────────────────────────────────────────────────────────────────

// The SDK's MSXML6.lib only holds GUIDs, compiled with /GL (unusable by lld-link),
// so CLSIDs and IIDs are taken from the header declarations with __uuidof().

// ─── MSXML6 class activation ─────────────────────────────────────────────────
// LoadLibraryExW is exported by KernelBase but hidden by the phone partition.
typedef HMODULE (WINAPI *PFN_LoadLibraryExW)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
typedef HRESULT (STDAPICALLTYPE *PFN_DllGetClassObject)(REFCLSID rclsid, REFIID riid, LPVOID* ppv);

// Returns the base address of the module containing the given address
static HMODULE GetBaseAddress(const void* address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(address, &mbi, sizeof(mbi)))
        return nullptr;
    if (*reinterpret_cast<WORD*>(mbi.AllocationBase) != IMAGE_DOS_SIGNATURE)
        return nullptr;
    return reinterpret_cast<HMODULE>(mbi.AllocationBase);
}

// Bypasses the COM registry: loads msxml6.dll and asks its class factory directly
static HRESULT CreateXhrFromDll(IXMLHTTPRequest2** xhr)
{
    HMODULE kernelBase = GetBaseAddress(reinterpret_cast<void*>(&::DisableThreadLibraryCalls));
    if (!kernelBase)
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

    PFN_LoadLibraryExW pLoadLibraryExW =
        reinterpret_cast<PFN_LoadLibraryExW>(GetProcAddress(kernelBase, "LoadLibraryExW"));
    if (!pLoadLibraryExW)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    // Kept loaded for the lifetime of the process
    HMODULE msxml = pLoadLibraryExW(L"msxml6.dll", nullptr, 0);
    if (!msxml)
        return HRESULT_FROM_WIN32(GetLastError());

    PFN_DllGetClassObject pDllGetClassObject =
        reinterpret_cast<PFN_DllGetClassObject>(GetProcAddress(msxml, "DllGetClassObject"));
    if (!pDllGetClassObject)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    IClassFactory* factory = nullptr;
    HRESULT hr = pDllGetClassObject(__uuidof(FreeThreadedXMLHTTP60), __uuidof(IClassFactory),
                                    reinterpret_cast<void**>(&factory));
    if (FAILED(hr))
        return hr;

    hr = factory->CreateInstance(nullptr, __uuidof(IXMLHTTPRequest2), reinterpret_cast<void**>(xhr));
    factory->Release();
    return hr;
}

static HRESULT CreateXhr(IXMLHTTPRequest2** xhr)
{
    HRESULT hr = CoCreateInstance(__uuidof(FreeThreadedXMLHTTP60), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IXMLHTTPRequest2), reinterpret_cast<void**>(xhr));
    if (SUCCEEDED(hr))
        return hr;
    printf("  CoCreateInstance(FreeThreadedXMLHTTP60) failed: 0x%08lX, trying msxml6.dll directly\n", hr);

    hr = CreateXhrFromDll(xhr);
    if (SUCCEEDED(hr))
        printf("  XHR created via msxml6.dll!DllGetClassObject\n");
    return hr;
}

// ─── IXMLHTTPRequest2 callback ───────────────────────────────────────────────
// Callbacks run on a thread-pool thread; main() only reads the fields after
// m_completed is signaled.
class HttpCallback : public IXMLHTTPRequest2Callback
{
public:
    HttpCallback()
        : m_refCount(1), m_status(0), m_result(E_PENDING)
    {
        m_reason[0]      = L'\0';
        m_contentType[0] = L'\0';
        m_date[0]        = L'\0';
        m_completed = CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    }

    ~HttpCallback()
    {
        if (m_completed)
            CloseHandle(m_completed);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IXMLHTTPRequest2Callback))
        {
            *ppv = static_cast<IXMLHTTPRequest2Callback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release()
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return count;
    }

    // IXMLHTTPRequest2Callback
    STDMETHODIMP OnRedirect(IXMLHTTPRequest2*, const WCHAR* pwszRedirectUrl)
    {
        printf("  redirect -> %ls\n", pwszRedirectUrl);
        return S_OK;
    }

    STDMETHODIMP OnHeadersAvailable(IXMLHTTPRequest2* pXHR, DWORD dwStatus, const WCHAR* pwszStatus)
    {
        m_status = dwStatus;
        StringCchCopyW(m_reason, _countof(m_reason), pwszStatus ? pwszStatus : L"");

        WCHAR* contentType = nullptr;
        if (SUCCEEDED(pXHR->GetResponseHeader(L"Content-Type", &contentType)) && contentType)
        {
            StringCchCopyW(m_contentType, _countof(m_contentType), contentType);
            CoTaskMemFree(contentType);
        }

        WCHAR* date = nullptr;
        if (SUCCEEDED(pXHR->GetResponseHeader(L"Date", &date)) && date)
        {
            StringCchCopyW(m_date, _countof(m_date), date);
            CoTaskMemFree(date);
        }
        return S_OK;
    }

    STDMETHODIMP OnDataAvailable(IXMLHTTPRequest2*, ISequentialStream*)
    {
        return S_OK;   // the whole body is read in OnResponseReceived
    }

    STDMETHODIMP OnResponseReceived(IXMLHTTPRequest2*, ISequentialStream* pResponseStream)
    {
        HRESULT hr = S_OK;
        char chunk[4096];
        for (;;)
        {
            ULONG read = 0;
            hr = pResponseStream->Read(chunk, sizeof(chunk), &read);
            if (FAILED(hr) || read == 0)
                break;
            if (!m_body.Append(chunk, read))
            {
                hr = E_OUTOFMEMORY;
                break;
            }
        }
        m_result = FAILED(hr) ? hr : S_OK;
        SetEvent(m_completed);
        return S_OK;
    }

    STDMETHODIMP OnError(IXMLHTTPRequest2*, HRESULT hrError)
    {
        m_result = hrError;
        SetEvent(m_completed);
        return S_OK;
    }

    HANDLE     m_completed;
    DWORD      m_status;
    WCHAR      m_reason[128];
    WCHAR      m_contentType[128];
    WCHAR      m_date[64];          // "Date" response header (server clock)
    HRESULT    m_result;
    ByteBuffer m_body;

private:
    volatile ULONG m_refCount;
};

// ─── Diagnostics ─────────────────────────────────────────────────────────────
// Errors surface either as INET_E_* (0x800C00xx) or as HRESULT_FROM_WIN32(12xxx)
static const char* DescribeError(HRESULT hr, bool* isCertError)
{
    *isCertError = false;

    switch (hr)
    {
    case INET_E_INVALID_URL:          return "INET_E_INVALID_URL";
    case INET_E_CANNOT_CONNECT:       return "INET_E_CANNOT_CONNECT";
    case INET_E_RESOURCE_NOT_FOUND:   return "INET_E_RESOURCE_NOT_FOUND (DNS / no network?)";
    case INET_E_DOWNLOAD_FAILURE:     return "INET_E_DOWNLOAD_FAILURE";
    case INET_E_CONNECTION_TIMEOUT:   return "INET_E_CONNECTION_TIMEOUT";
    case INET_E_SECURITY_PROBLEM:     return "INET_E_SECURITY_PROBLEM (TLS protocol / cipher suite?)";
    case INET_E_REDIRECT_FAILED:      return "INET_E_REDIRECT_FAILED";
    case INET_E_INVALID_CERTIFICATE:  *isCertError = true; return "INET_E_INVALID_CERTIFICATE";
    }

    if (HRESULT_FACILITY(hr) != FACILITY_WIN32)
        return "";

    switch (HRESULT_CODE(hr))
    {
    case 12002: return "timeout";
    case 12007: return "host name not resolved (DNS / no network?)";
    case 12029: return "cannot connect";
    case 12030: return "connection aborted";
    case 12152: return "invalid server response";
    case 12157: return "security channel error (TLS protocol / cipher suite mismatch)";
    case 12175: return "secure failure (TLS handshake or certificate)";
    case 12037: *isCertError = true; return "certificate date invalid";
    case 12038: *isCertError = true; return "certificate common name invalid";
    case 12044: return "server requests a client certificate";
    case 12045: *isCertError = true; return "unknown certificate authority (root missing from phone store)";
    case 12057: *isCertError = true; return "certificate revocation check failed";
    case 12169: *isCertError = true; return "invalid certificate";
    case 12179: *isCertError = true; return "certificate chain problem";
    default:    return "";
    }
}

// Crude lookup of "key":<value> in the JSON body, enough to prove the payload is real
// ─── Clock check ─────────────────────────────────────────────────────────────
// HTTPS certificate validation uses the phone clock: when it is wrong, a
// recently renewed server certificate looks "not yet valid".

static void PrintPhoneClock()
{
    SYSTEMTIME now;
    GetSystemTime(&now);
    printf("Phone clock (UTC): %04u-%02u-%02u %02u:%02u:%02u\n",
           now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
}

// Parses an RFC 1123 HTTP date, e.g. "Sun, 04 Oct 2026 20:31:26 GMT"
static bool ParseHttpDate(const WCHAR* text, SYSTEMTIME* st)
{
    static const wchar_t* MONTHS[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                       L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    wchar_t  month[4] = {};
    unsigned day, year, hour, minute, second;
    if (swscanf_s(text, L"%*3s, %u %3s %u %u:%u:%u", &day, month, static_cast<unsigned>(_countof(month)),
                  &year, &hour, &minute, &second) != 6)
        return false;

    ZeroMemory(st, sizeof(*st));
    for (WORD i = 0; i < 12; ++i)
    {
        if (wcscmp(month, MONTHS[i]) == 0)
            st->wMonth = i + 1;
    }
    if (st->wMonth == 0)
        return false;

    st->wYear   = static_cast<WORD>(year);
    st->wDay    = static_cast<WORD>(day);
    st->wHour   = static_cast<WORD>(hour);
    st->wMinute = static_cast<WORD>(minute);
    st->wSecond = static_cast<WORD>(second);
    return true;
}

// Warns when the phone clock differs from the server's "Date" header
static void CheckClockSkew(const WCHAR* serverDate)
{
    SYSTEMTIME serverTime;
    FILETIME   serverFileTime;
    if (!serverDate[0] || !ParseHttpDate(serverDate, &serverTime) ||
        !SystemTimeToFileTime(&serverTime, &serverFileTime))
    {
        printf("  warning: cannot check the phone clock (Date header: \"%ls\")\n", serverDate);
        return;
    }

    FILETIME phoneFileTime;
    GetSystemTimeAsFileTime(&phoneFileTime);

    ULARGE_INTEGER server, phone;
    server.LowPart  = serverFileTime.dwLowDateTime;
    server.HighPart = serverFileTime.dwHighDateTime;
    phone.LowPart   = phoneFileTime.dwLowDateTime;
    phone.HighPart  = phoneFileTime.dwHighDateTime;

    // FILETIME is in 100 ns units
    LONGLONG skew    = (static_cast<LONGLONG>(phone.QuadPart) - static_cast<LONGLONG>(server.QuadPart)) / 10000000;
    LONGLONG absSkew = skew < 0 ? -skew : skew;
    if (absSkew <= CLOCK_SKEW_WARNING_SECONDS)
        return;

    printf("Warning: phone clock is %lldd %02lldh %02lldm %s the server (server: %ls).\n",
           absSkew / 86400, (absSkew % 86400) / 3600, (absSkew % 3600) / 60,
           skew < 0 ? "behind" : "ahead of", serverDate);
    printf("         HTTPS certificate checks depend on it: set the date and time in Settings.\n");
}

#if defined(WINAPI_FAMILY_PHONE_APP) && WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
// Exported by mincore.lib but hidden by the phone partition
extern "C" __declspec(dllimport) DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
#endif

// STORAGE_FILE_NAME in the exe's folder, so it does not depend on the current directory
static void GetDefaultStoragePath(wchar_t* path, size_t pathCount)
{
    DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(pathCount));
    wchar_t* slash = (length > 0 && length < pathCount) ? wcsrchr(path, L'\\') : nullptr;
    if (slash)
        slash[1] = L'\0';
    else
        path[0] = L'\0';
    StringCchCatW(path, pathCount, STORAGE_FILE_NAME);
}

static long long UnixNow()
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart  = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    return static_cast<long long>((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

// ─── Chart analysis ──────────────────────────────────────────────────────────
// The last close is provisional while today's session is still open
static bool IsSessionInProgress(const ChartData& chart)
{
    char sessionDate[11];
    FormatDate(chart.sessionStart, chart.gmtOffset, sessionDate, _countof(sessionDate));
    const DailyClose& last = chart.closes[chart.closeCount - 1];
    return chart.sessionEnd != 0 && strcmp(last.date, sessionDate) == 0 && UnixNow() < chart.sessionEnd;
}

struct TickerReport
{
    bool         intraday;   // the last close is from a session still open
    int          finished;   // closes from finished sessions: the ones counted and stored
    StreakResult streak;
    PriceSummary prices;
    double       score;      // ComputeScore(), 0 to 100
    ScoreClass   scoreClass;
    int          rebound;    // ComputeReboundScore(), 20 to 95
};

// Continues the stored streak (stored may be nullptr) with the chart's closes
static TickerReport AnalyzeChart(const ChartData& chart, const StoredTicker* stored)
{
    TickerReport report;
    report.intraday = IsSessionInProgress(chart);
    report.finished = report.intraday ? chart.closeCount - 1 : chart.closeCount;
    report.streak   = ContinueStreak(stored ? stored->streak : 0, stored ? stored->lastUpdate : "",
                                     chart.closes, report.finished);
    report.prices   = SummarizePrices(chart.closes, chart.closeCount);
    report.score    = ComputeScore(report.prices.changePct, report.prices.spanPct, report.streak.streak);
    report.scoreClass = ClassifyScore(report.score);
    report.rebound    = ComputeReboundScore(report.prices.changePct, report.prices.spanPct);
    return report;
}

static void UpdateStoredTicker(StoredTicker* stored, const ChartData& chart, const TickerReport& report,
                               const Equity* equity)
{
    strncpy_s(stored->name, _countof(stored->name), chart.name, _TRUNCATE);
    if (equity)
    {
        // Categories come from the EQUITIES table, as in the Python tracker
        stored->categoryCount = 0;
        for (int i = 0; i < EQUITY_MAX_CATEGORIES && equity->categories[i]; ++i)
            strncpy_s(stored->categories[stored->categoryCount++], _countof(stored->categories[0]),
                      equity->categories[i], _TRUNCATE);
    }

    if (report.finished == 0 || report.streak.source == STREAK_STORAGE_NEWER)
        return;   // nothing new; never rewind the state to older data

    stored->streak = report.streak.streak;
    strncpy_s(stored->lastUpdate, _countof(stored->lastUpdate), report.streak.lastDate, _TRUNCATE);

    // History: the last HISTORY_SIZE finished closes, rounded like the Python tracker
    int first = report.finished > HISTORY_SIZE ? report.finished - HISTORY_SIZE : 0;
    stored->historyCount = 0;
    for (int i = first; i < report.finished; ++i)
    {
        StoredClose& entry = stored->history[stored->historyCount++];
        strncpy_s(entry.date, _countof(entry.date), chart.closes[i].date, _TRUNCATE);
        entry.close = Round2(chart.closes[i].close);
    }
}

// " | 4d: -3.80 EUR / -1.97%", or "" when there is a single close
static void FormatSpan(const ChartData& chart, const PriceSummary& prices, char* out, size_t outSize)
{
    out[0] = '\0';
    if (prices.spanDays > 0)
        _snprintf_s(out, outSize, _TRUNCATE, " | %dd: %+.2f %s / %+.2f%%", prices.spanDays,
                    prices.spanChange, chart.currency, prices.spanPct);
}

// ─── Single ticker (detailed) ────────────────────────────────────────────────
static void PrintChartDetails(const ChartData& chart, const TickerReport& report)
{
    long offsetMinutes = labs(chart.gmtOffset) / 60;
    printf("%s (%s)  %s  %s (UTC%c%02ld:%02ld)\n", chart.name, chart.symbol, chart.currency,
           chart.exchangeTimezone, chart.gmtOffset < 0 ? '-' : '+', offsetMinutes / 60, offsetMinutes % 60);

    printf("  Date             Close    Change\n");
    for (int i = 0; i < chart.closeCount; ++i)
    {
        double close = Round2(chart.closes[i].close);
        printf("  %s %10.2f", chart.closes[i].date, close);
        if (i > 0)
        {
            double prev   = Round2(chart.closes[i - 1].close);
            double change = Round2(close - prev);
            printf("  %+8.2f %+7.2f%%", change, prev != 0.0 ? Round2(change / prev * 100.0) : 0.0);
        }
        if (i == chart.closeCount - 1 && report.intraday)
            printf("  (session in progress)");
        printf("\n");
    }

    const PriceSummary& prices = report.prices;
    char span[96];
    FormatSpan(chart, prices, span, _countof(span));
    printf("Last: %.2f %s (%+.2f %s / %+.2f%% today)%s\n", prices.close, chart.currency, prices.change,
           chart.currency, prices.changePct, span);

    const StreakResult& streak = report.streak;
    if (!streak.lastDate[0])
        printf("Streak: 0 (no finished session yet)\n");
    else
    {
        const char* basis = "";
        switch (streak.source)
        {
        case STREAK_CONTINUED:     basis = "continued from the stored history";    break;
        case STREAK_RECOMPUTED:    basis = "computed from these closes only";      break;
        case STREAK_UP_TO_DATE:    basis = "already up to date";                   break;
        case STREAK_STORAGE_NEWER: basis = "stored state is newer than this data"; break;
        }
        printf("%s%d%s (as of %s, %s)\n", streak.streak >= 2 ? "ALERT: " : "Streak: ", streak.streak,
               streak.streak >= 2 ? " day streak!" : "", streak.lastDate, basis);
    }
    printf("Score: %.1f (%s), rebound score: %d\n", report.score, ScoreClassName(report.scoreClass), report.rebound);
    if (report.intraday)
        printf("Today's session is in progress: its close will be counted on a later run.\n");
}

// Parses one chart response, prints it in detail and, when save is true,
// updates its record in the storage file.
static bool ShowTicker(const ByteBuffer& body, const wchar_t* storagePath, bool save)
{
    ChartData chart;
    char      error[256];
    if (body.size == 0)
    {
        printf("Empty response.\n");
        return false;
    }
    if (!ParseChart(body.data, &chart, error, _countof(error)))
    {
        printf("Cannot use the response: %s\n", error);
        if (strncmp(error, "Yahoo error", 11) != 0)
        {
            size_t previewLen = body.size < PREVIEW_BYTES ? body.size : PREVIEW_BYTES;
            printf("--- body ---\n%.*s%s\n------------\n", static_cast<int>(previewLen), body.data,
                   previewLen < body.size ? "..." : "");
        }
        return false;
    }

    Storage storage;
    InitStorage(&storage);
    bool storageOk = LoadStorage(storagePath, &storage, error, _countof(error));
    if (!storageOk)
        printf("Warning: cannot use %ls: %s. It will not be updated.\n", storagePath, error);

    StoredTicker* stored = storageOk ? FindTicker(&storage, chart.symbol) : nullptr;
    TickerReport  report = AnalyzeChart(chart, stored);
    PrintChartDetails(chart, report);

    if (storageOk && save)
    {
        if (!stored)
            stored = AddTicker(&storage, chart.symbol);
        if (!stored)
            printf("Warning: out of memory, %ls not updated.\n", storagePath);
        else
        {
            UpdateStoredTicker(stored, chart, report, FindEquity(chart.symbol));
            if (SaveStorage(storagePath, storage, error, _countof(error)))
                printf("Saved to %ls\n", storagePath);
            else
                printf("Warning: cannot save %ls: %s\n", storagePath, error);
        }
    }

    FreeStorage(&storage);
    return true;
}

// ─── Full scan ───────────────────────────────────────────────────────────────
static HRESULT Fetch(const wchar_t* url, ULONGLONG certIgnoreFlags, bool closeConnection, HttpCallback* callback);

// One ticker of a scan: either scored, or the reason it could not be
struct ScanEntry
{
    const Equity* equity;
    bool          scored;
    ChartData     chart;
    TickerReport  report;
    char          problem[192];   // "ERROR: ..." / "WARNING: ..." when not scored
};

// One line per ticker, two for an alert, prefixed with "[score / R rebound score]"
static void PrintScanLine(const ScanEntry& entry)
{
    const Equity&       equity = *entry.equity;
    const ChartData&    chart  = entry.chart;
    const TickerReport& report = entry.report;

    char categories[160] = "";
    for (int i = 0; i < EQUITY_MAX_CATEGORIES && equity.categories[i]; ++i)
    {
        if (i > 0)
            strcat_s(categories, _countof(categories), " | ");
        strcat_s(categories, _countof(categories), equity.categories[i]);
    }

    const PriceSummary& prices = report.prices;
    char span[96];
    FormatSpan(chart, prices, span, _countof(span));
    const char* inProgress = report.intraday ? " (session in progress)" : "";

    if (report.streak.streak >= 2)
    {
        printf("[%5.1f / R %2d] ALERT: %s (%s) [%s] - %d DAY STREAK!\n", report.score, report.rebound, chart.name,
               equity.symbol, categories, report.streak.streak);
        printf("                 Price: %.2f %s (%+.2f %s / %+.2f%%)%s%s\n", prices.close, chart.currency,
               prices.change, chart.currency, prices.changePct, span, inProgress);
    }
    else
        printf("[%5.1f / R %2d] %s (%s) [%s]: %.2f %s (%+.2f %s / %+.2f%% today)%s%s\n", report.score,
               report.rebound, chart.name, equity.symbol, categories, prices.close, chart.currency, prices.change,
               chart.currency, prices.changePct, span, inProgress);
}

// Same content as the console output of a scan: the scored tickers in rank
// order (order[0..orderCount-1] index entries), then the ones not scored.
// Values are rounded as printed. Returns false with the reason in error.
static bool WriteScanJson(const wchar_t* path, const ScanEntry* entries, const int* order, int orderCount,
                          int alerts, int failures, char* error, size_t errorSize)
{
    SYSTEMTIME now;
    GetSystemTime(&now);
    char generated[32];
    _snprintf_s(generated, _countof(generated), _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "generated", generated);
    cJSON_AddNumberToObject(root, "stocks", EQUITY_COUNT);
    cJSON_AddNumberToObject(root, "alerts", alerts);
    cJSON_AddNumberToObject(root, "failed", failures);

    cJSON* ranked = cJSON_AddArrayToObject(root, "ranked");
    for (int k = 0; k < orderCount; ++k)
    {
        const ScanEntry&    entry  = entries[order[k]];
        const ChartData&    chart  = entry.chart;
        const TickerReport& report = entry.report;
        const PriceSummary& prices = report.prices;

        cJSON* item = cJSON_CreateObject();
        cJSON_AddItemToArray(ranked, item);
        cJSON_AddNumberToObject(item, "rank", k + 1);
        cJSON_AddStringToObject(item, "symbol", entry.equity->symbol);
        cJSON_AddStringToObject(item, "name", chart.name);
        cJSON* categories = cJSON_AddArrayToObject(item, "categories");
        for (int i = 0; i < EQUITY_MAX_CATEGORIES && entry.equity->categories[i]; ++i)
            cJSON_AddItemToArray(categories, cJSON_CreateString(entry.equity->categories[i]));
        cJSON_AddNumberToObject(item, "score", floor(report.score * 10.0 + 0.5) / 10.0);
        cJSON_AddStringToObject(item, "class", ScoreClassName(report.scoreClass));
        cJSON_AddStringToObject(item, "class_range", ScoreClassRange(report.scoreClass));
        cJSON_AddNumberToObject(item, "rebound", report.rebound);
        cJSON_AddNumberToObject(item, "streak", report.streak.streak);
        cJSON_AddBoolToObject(item, "alert", report.streak.streak >= 2);
        cJSON_AddStringToObject(item, "currency", chart.currency);
        cJSON_AddNumberToObject(item, "close", prices.close);
        cJSON_AddNumberToObject(item, "change", prices.change);
        cJSON_AddNumberToObject(item, "change_pct", prices.changePct);
        if (prices.spanDays > 0)
        {
            cJSON_AddNumberToObject(item, "span_days", prices.spanDays);
            cJSON_AddNumberToObject(item, "span_change", prices.spanChange);
            cJSON_AddNumberToObject(item, "span_pct", prices.spanPct);
        }
        else
        {
            cJSON_AddNullToObject(item, "span_days");
            cJSON_AddNullToObject(item, "span_change");
            cJSON_AddNullToObject(item, "span_pct");
        }
        cJSON_AddBoolToObject(item, "session_in_progress", report.intraday);
    }

    cJSON* notScored = cJSON_AddArrayToObject(root, "not_scored");
    for (int i = 0; i < EQUITY_COUNT; ++i)
    {
        if (entries[i].scored)
            continue;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddItemToArray(notScored, item);
        cJSON_AddStringToObject(item, "symbol", entries[i].equity->symbol);
        cJSON_AddStringToObject(item, "problem", entries[i].problem);
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
    {
        strncpy_s(error, errorSize, "out of memory", _TRUNCATE);
        return false;
    }

    FILE* f = nullptr;
    errno_t err = _wfopen_s(&f, path, L"wb");
    if (err != 0 || !f)
    {
        _snprintf_s(error, errorSize, _TRUNCATE, "cannot create the file (errno %d)", err);
        cJSON_free(text);
        return false;
    }
    size_t length  = strlen(text);
    bool   written = fwrite(text, 1, length, f) == length && fwrite("\n", 1, 1, f) == 1;
    written = fclose(f) == 0 && written;
    cJSON_free(text);
    if (!written)
        strncpy_s(error, errorSize, "cannot write the file", _TRUNCATE);
    return written;
}

// Fetches every ticker of EQUITIES, then prints them sorted by score (best
// first) and grouped by class, and updates the storage once at the end.
// A ticker that fails keeps its stored record. When jsonPath is not nullptr,
// the result is also written there as JSON. Returns the number of tickers
// that failed.
static int ScanAll(ULONGLONG certIgnoreFlags, const wchar_t* storagePath, bool save, const wchar_t* jsonPath)
{
    char error[256];
    Storage storage;
    InitStorage(&storage);
    bool storageOk = LoadStorage(storagePath, &storage, error, _countof(error));
    if (!storageOk)
        printf("Warning: cannot use %ls: %s. It will not be updated.\n", storagePath, error);

    ScanEntry* entries = static_cast<ScanEntry*>(calloc(EQUITY_COUNT, sizeof(ScanEntry)));
    if (!entries)
    {
        printf("Out of memory.\n");
        FreeStorage(&storage);
        return EQUITY_COUNT;
    }

    printf("--- Euronext Daily Scan (%d stocks) ---\n", EQUITY_COUNT);
    printf("Fetching: ");
    fflush(stdout);

    int         failures     = 0;
    int         scored       = 0;
    WCHAR       serverDate[64] = L"";   // from the first response, checked after the progress line
    const char* stopReason   = nullptr;
    ULONGLONG   startTick    = GetTickCount64();

    for (int i = 0; i < EQUITY_COUNT; ++i)
    {
        ScanEntry&    entry  = entries[i];
        const Equity& equity = EQUITIES[i];
        entry.equity = &equity;

        if (stopReason)
        {
            _snprintf_s(entry.problem, _countof(entry.problem), _TRUNCATE, "ERROR: %s: not fetched (scan stopped)",
                        equity.symbol);
            ++failures;
            continue;
        }

        wchar_t symbol[32];   // tickers are ASCII
        int     length = 0;
        for (; equity.symbol[length] && length < _countof(symbol) - 1; ++length)
            symbol[length] = static_cast<wchar_t>(equity.symbol[length]);
        symbol[length] = L'\0';

        wchar_t url[256];
        StringCchPrintfW(url, _countof(url), CHART_URL_FORMAT, symbol);

        HttpCallback* callback = new HttpCallback();
        HRESULT hr = Fetch(url, certIgnoreFlags, false, callback);
        if (SUCCEEDED(hr))
            hr = callback->m_result;

        if (FAILED(hr))
        {
            bool isCertError = false;
            const char* desc = DescribeError(hr, &isCertError);
            _snprintf_s(entry.problem, _countof(entry.problem), _TRUNCATE, "ERROR: %s: request failed: 0x%08lX %s",
                        equity.symbol, hr, desc);
            // Every other ticker would fail the same way; and each failed
            // connection takes seconds, so do not wait for all the others
            if (isCertError && !certIgnoreFlags)
                stopReason = "certificate problem";
            else if (scored == 0 && failures + 1 == SCAN_ABORT_AFTER_FAILURES)
                stopReason = "no connection to Yahoo";
        }
        else
        {
            if (!serverDate[0])
                StringCchCopyW(serverDate, _countof(serverDate), callback->m_date);

            if (callback->m_status == 429)
                _snprintf_s(entry.problem, _countof(entry.problem), _TRUNCATE,
                            "ERROR: %s: rate limited by Yahoo (HTTP 429)", equity.symbol);
            else if (callback->m_body.size == 0)
                _snprintf_s(entry.problem, _countof(entry.problem), _TRUNCATE, "ERROR: %s: empty response (HTTP %lu)",
                            equity.symbol, callback->m_status);
            else if (!ParseChart(callback->m_body.data, &entry.chart, error, _countof(error)))
            {
                if (strncmp(error, "no close price", 14) == 0)
                    _snprintf_s(entry.problem, _countof(entry.problem), _TRUNCATE,
                                "WARNING: %s: no market data available", equity.symbol);
                else
                    _snprintf_s(entry.problem, _countof(entry.problem), _TRUNCATE, "ERROR: %s: %s", equity.symbol, error);
            }
            else
            {
                StoredTicker* stored = storageOk ? FindTicker(&storage, equity.symbol) : nullptr;
                entry.report = AnalyzeChart(entry.chart, stored);
                entry.scored = true;
                ++scored;

                if (storageOk)
                {
                    if (!stored)
                        stored = AddTicker(&storage, equity.symbol);
                    if (stored)
                        UpdateStoredTicker(stored, entry.chart, entry.report, &equity);
                }
            }
        }
        callback->Release();

        if (!entry.scored)
            ++failures;
        printf("%c", entry.scored ? '.' : 'x');
        fflush(stdout);
    }
    printf("\n");
    if (serverDate[0])
        CheckClockSkew(serverDate);
    if (stopReason)
        printf("Scan stopped: %s.%s\n", stopReason,
               strcmp(stopReason, "certificate problem") == 0 ? " A wrong phone clock is the usual cause."
                                                              : " Check the phone's network.");

    // Stable sort of the scored tickers by score, best first (54 entries: insertion sort)
    int order[256];
    int orderCount = 0;
    for (int i = 0; i < EQUITY_COUNT && orderCount < _countof(order); ++i)
    {
        if (!entries[i].scored)
            continue;
        int j = orderCount++;
        while (j > 0 && entries[order[j - 1]].report.score < entries[i].report.score)
        {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
    }

    if (orderCount > 0)
        printf("[score / R rebound score], sorted by score\n");

    int alerts = 0;
    int shownClass = -1;
    for (int k = 0; k < orderCount; ++k)
    {
        const ScanEntry& entry = entries[order[k]];
        if (entry.report.scoreClass != shownClass)
        {
            shownClass = entry.report.scoreClass;
            printf("=== %s (%s) ===\n", ScoreClassName(entry.report.scoreClass), ScoreClassRange(entry.report.scoreClass));
        }
        PrintScanLine(entry);
        if (entry.report.streak.streak >= 2)
            ++alerts;
    }

    if (failures > 0)
    {
        printf("=== NOT SCORED ===\n");
        for (int i = 0; i < EQUITY_COUNT; ++i)
        {
            if (!entries[i].scored)
                printf("%s\n", entries[i].problem);
        }
    }

    printf("--- %d stocks, %d alert(s), %d failed, %llu s ---\n", EQUITY_COUNT, alerts, failures,
           (GetTickCount64() - startTick) / 1000);

    if (jsonPath)
    {
        if (WriteScanJson(jsonPath, entries, order, orderCount, alerts, failures, error, _countof(error)))
            printf("Wrote %ls\n", jsonPath);
        else
            printf("Warning: cannot write %ls: %s\n", jsonPath, error);
    }

    if (storageOk && save && scored > 0)
    {
        if (SaveStorage(storagePath, storage, error, _countof(error)))
            printf("Saved to %ls\n", storagePath);
        else
            printf("Warning: cannot save %ls: %s\n", storagePath, error);
    }

    free(entries);
    FreeStorage(&storage);
    return failures;
}

static bool SaveBody(const wchar_t* path, const ByteBuffer& body)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return false;
    size_t written = fwrite(body.data, 1, body.size, f);
    fclose(f);
    return written == body.size;
}

static bool LoadBody(const wchar_t* path, ByteBuffer* body)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return false;
    char   chunk[4096];
    size_t read;
    bool   ok = true;
    while (ok && (read = fread(chunk, 1, sizeof(chunk), f)) > 0)
        ok = body->Append(chunk, read);
    fclose(f);
    return ok;
}

static void SetXhrProperty(IXMLHTTPRequest2* xhr, XHR_PROPERTY property, ULONGLONG value, const char* name)
{
    HRESULT hr = xhr->SetProperty(property, value);
    if (FAILED(hr))
        printf("  warning: SetProperty(XHR_PROP_%s) failed: 0x%08lX\n", name, hr);
}

static void PrintUsage()
{
    printf("wp81Equities - Euronext daily scan with Yahoo Finance data\n");
    printf("\n");
    printf("Usage:\n");
    printf("  wp81Equities.exe [-s FILE] [-n] [-j FILE] [-k | -u]\n");
    printf("      scan all %d tracked tickers, sorted by short-term score\n", EQUITY_COUNT);
    printf("  wp81Equities.exe [-s FILE] [-n] [-o FILE] [-k | -u] TICKER | URL\n");
    printf("      show one ticker in detail\n");
    printf("  wp81Equities.exe [-s FILE] [-n] -i FILE\n");
    printf("      show a saved response in detail (no network)\n");
    printf("  wp81Equities.exe --diag [TICKER | URL]\n");
    printf("      try every combination of ignored certificate checks,\n");
    printf("      to find which ones fail on this phone (default: %ls)\n", DEFAULT_TICKER);
    printf("  wp81Equities.exe -h | --help\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  TICKER      Yahoo Finance symbol, e.g. AIR.PA\n");
    printf("  URL         full http:// or https:// URL, fetched as is\n");
    printf("\n");
    printf("Options:\n");
    printf("  -s FILE     use FILE as the storage instead of %ls\n", STORAGE_FILE_NAME);
    printf("  -n          do not update the storage\n");
    printf("  -j FILE     scan: also write the ranked list to FILE as JSON\n");
    printf("  -o FILE     save the response body to FILE\n");
    printf("  -i FILE     parse a saved response instead of fetching\n");
    printf("  -k          ignore all certificate errors (diagnosis only, unsafe)\n");
    printf("  -u          ignore only \"unknown CA\" certificate errors\n");
    printf("  -h, --help  show this help\n");
    printf("\n");
    printf("Score (0-100): 4-day change, today's change, their consistency, the\n");
    printf("up-day streak, minus penalties for extreme moves. Classes: STRONG\n");
    printf("SHORT-TERM 75+, INTERESTING 65+, WATCH 55+, NEUTRAL 45+, WEAK.\n");
    printf("Rebound score (R, 20-95, shown beside the score, not used to sort):\n");
    printf("high after a large 4-day fall, more when today is up.\n");
    printf("The up-day streak (ALERT from 2 days) is continued from %ls,\n", STORAGE_FILE_NAME);
    printf("next to the exe, same file and format as euronext_tracker.py.\n");
    printf("Certificate errors are usually caused by a wrong phone clock:\n");
    printf("check the date and time in Settings first.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  wp81Equities.exe\n");
    printf("  wp81Equities.exe -j scan.json\n");
    printf("  wp81Equities.exe AIR.PA\n");
    printf("  wp81Equities.exe -o airbus.json AIR.PA\n");
    printf("  wp81Equities.exe -n -i airbus.json\n");
    printf("\n");
    printf("Exit code: 0 success, 1 failure (scan: at least one ticker failed),\n");
    printf("           2 invalid arguments.\n");
}

// ─── HTTP request ────────────────────────────────────────────────────────────
// Sends one GET and waits for it to complete. On success the outcome is in
// callback->m_result / m_status / m_body.
static HRESULT Fetch(const wchar_t* url, ULONGLONG certIgnoreFlags, bool closeConnection, HttpCallback* callback)
{
    IXMLHTTPRequest2* xhr = nullptr;
    HRESULT hr = CreateXhr(&xhr);
    if (FAILED(hr))
    {
        printf("Could not create IXMLHTTPRequest2: 0x%08lX\n", hr);
        return hr;
    }

    hr = xhr->Open(L"GET", url, callback, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr))
    {
        printf("Open failed: 0x%08lX\n", hr);
        xhr->Release();
        return hr;
    }

    SetXhrProperty(xhr, XHR_PROP_TIMEOUT, REQUEST_TIMEOUT_MS, "TIMEOUT");
    SetXhrProperty(xhr, XHR_PROP_NO_CRED_PROMPT, TRUE, "NO_CRED_PROMPT");
    SetXhrProperty(xhr, XHR_PROP_NO_CACHE, TRUE, "NO_CACHE");
    SetXhrProperty(xhr, XHR_PROP_EXTENDED_ERROR, TRUE, "EXTENDED_ERROR");
    if (certIgnoreFlags)    // takes a XHR_CERT_IGNORE_* bitmask, not a BOOL
        SetXhrProperty(xhr, XHR_PROP_IGNORE_CERT_ERRORS, certIgnoreFlags, "IGNORE_CERT_ERRORS");

    xhr->SetRequestHeader(L"User-Agent", USER_AGENT);
    xhr->SetRequestHeader(L"Accept", L"application/json");
    if (closeConnection)
    {
        HRESULT hrHeader = xhr->SetRequestHeader(L"Connection", L"close");
        if (FAILED(hrHeader))
            printf("  warning: cannot set \"Connection: close\": 0x%08lX\n", hrHeader);
    }

    hr = xhr->Send(nullptr, 0);
    if (FAILED(hr))
        printf("Send failed: 0x%08lX\n", hr);
    else if (WaitForSingleObjectEx(callback->m_completed, WAIT_TIMEOUT_MS, FALSE) != WAIT_OBJECT_0)
    {
        printf("No completion after %lu ms, aborting.\n", WAIT_TIMEOUT_MS);
        xhr->Abort();
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    xhr->Release();
    return hr;
}

// ─── Certificate diagnosis ───────────────────────────────────────────────────
// Tries every combination of XHR_CERT_IGNORE_* flags, fewest flags first, to
// find which certificate checks fail on this phone.
static int DiagnoseCertificate(const wchar_t* url)
{
    static const struct { ULONGLONG flag; const char* name; } IGNORE_FLAGS[] = {
        { XHR_CERT_IGNORE_UNKNOWN_CA,        "UNKNOWN_CA"        },
        { XHR_CERT_IGNORE_REVOCATION_FAILED, "REVOCATION_FAILED" },
        { XHR_CERT_IGNORE_CERT_DATE_INVALID, "DATE_INVALID"      },
        { XHR_CERT_IGNORE_CERT_CN_INVALID,   "CN_INVALID"        },
    };
    const int flagCount = _countof(IGNORE_FLAGS);

    PrintPhoneClock();
    printf("GET %ls\n", url);
    printf("Ignored checks -> result (\"Connection: close\" on every request)\n");

    bool anyOk = false;
    for (int bitCount = 0; bitCount <= flagCount; ++bitCount)
    {
        for (int mask = 0; mask < (1 << flagCount); ++mask)
        {
            int bits = 0;
            for (int i = 0; i < flagCount; ++i)
                bits += (mask >> i) & 1;
            if (bits != bitCount)
                continue;

            ULONGLONG flags     = 0;
            char      names[96] = "(none)";
            for (int i = 0; i < flagCount; ++i)
            {
                if (!(mask & (1 << i)))
                    continue;
                if (flags == 0)
                    names[0] = '\0';
                else
                    strcat_s(names, _countof(names), "+");
                strcat_s(names, _countof(names), IGNORE_FLAGS[i].name);
                flags |= IGNORE_FLAGS[i].flag;
            }

            HttpCallback* callback = new HttpCallback();
            HRESULT hr = Fetch(url, flags, true, callback);
            if (SUCCEEDED(hr))
                hr = callback->m_result;

            if (SUCCEEDED(hr))
            {
                printf("  %-52s OK (HTTP %lu)%s\n", names, callback->m_status,
                       anyOk ? "" : "  <== first success");
                anyOk = true;
            }
            else
            {
                bool isCertError = false;
                printf("  %-52s 0x%08lX %s\n", names, hr, DescribeError(hr, &isCertError));
            }
            callback->Release();
        }
    }

    if (anyOk)
        printf("The checks ignored on the first success are the ones failing on this phone.\n");
    else
        printf("No combination works: the certificate fails a check these flags cannot skip.\n");
    return anyOk ? 0 : 1;
}

// ─── Entry point ─────────────────────────────────────────────────────────────
static void PrintIgnoredCertificateChecks(ULONGLONG certIgnoreFlags)
{
    if (certIgnoreFlags == XHR_CERT_IGNORE_ALL_SERVER_ERRORS)
        printf("  (all certificate errors ignored)\n");
    else if (certIgnoreFlags)
        printf("  (\"unknown CA\" certificate errors ignored)\n");
}

int wmain(int argc, wchar_t* argv[])
{
    ULONGLONG      certIgnoreFlags = 0;   // XHR_CERT_IGNORE_* bitmask
    bool           diagnose        = false;
    const wchar_t* outFile         = nullptr;
    const wchar_t* jsonFile        = nullptr;
    const wchar_t* inFile          = nullptr;
    const wchar_t* storageFile     = nullptr;
    bool           save            = true;
    const wchar_t* target          = nullptr;   // nullptr: scan every ticker of EQUITIES

    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"-k") == 0)
            certIgnoreFlags = XHR_CERT_IGNORE_ALL_SERVER_ERRORS;
        else if (wcscmp(argv[i], L"-u") == 0)
            certIgnoreFlags = XHR_CERT_IGNORE_UNKNOWN_CA;
        else if (wcscmp(argv[i], L"--diag") == 0)
            diagnose = true;
        else if (wcscmp(argv[i], L"-o") == 0 && i + 1 < argc)
            outFile = argv[++i];
        else if (wcscmp(argv[i], L"-j") == 0 && i + 1 < argc)
            jsonFile = argv[++i];
        else if (wcscmp(argv[i], L"-i") == 0 && i + 1 < argc)
            inFile = argv[++i];
        else if (wcscmp(argv[i], L"-s") == 0 && i + 1 < argc)
            storageFile = argv[++i];
        else if (wcscmp(argv[i], L"-n") == 0)
            save = false;
        else if (wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--help") == 0)
        {
            PrintUsage();
            return 0;
        }
        else if (argv[i][0] == L'-')
        {
            if (wcscmp(argv[i], L"-o") == 0 || wcscmp(argv[i], L"-i") == 0 || wcscmp(argv[i], L"-s") == 0 ||
                wcscmp(argv[i], L"-j") == 0)
                printf("Option %ls needs a file name.\n\n", argv[i]);
            else
                printf("Unknown option: %ls\n\n", argv[i]);
            PrintUsage();
            return 2;
        }
        else
            target = argv[i];
    }

    if (outFile && !target)
    {
        printf("Option -o needs a TICKER or URL.\n\n");
        PrintUsage();
        return 2;
    }

    if (jsonFile && (target || inFile || diagnose))
    {
        printf("Option -j is only for a full scan.\n\n");
        PrintUsage();
        return 2;
    }

    wchar_t storagePath[MAX_PATH];
    if (storageFile)
        StringCchCopyW(storagePath, _countof(storagePath), storageFile);
    else
        GetDefaultStoragePath(storagePath, _countof(storagePath));

    if (inFile)
    {
        ByteBuffer body;
        if (!LoadBody(inFile, &body))
        {
            printf("Cannot read %ls\n", inFile);
            return 1;
        }
        printf("Parsing %ls\n", inFile);
        return ShowTicker(body, storagePath, save) ? 0 : 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        printf("CoInitializeEx failed: 0x%08lX\n", hr);
        return 1;
    }

    if (!target && !diagnose)
    {
        PrintIgnoredCertificateChecks(certIgnoreFlags);
        int failures = ScanAll(certIgnoreFlags, storagePath, save, jsonFile);
        CoUninitialize();
        return failures ? 1 : 0;
    }

    if (!target)
        target = DEFAULT_TICKER;

    wchar_t url[1024];
    if (_wcsnicmp(target, L"https://", 8) == 0 || _wcsnicmp(target, L"http://", 7) == 0)
        StringCchCopyW(url, _countof(url), target);
    else
        StringCchPrintfW(url, _countof(url), CHART_URL_FORMAT, target);

    if (diagnose)
    {
        int diagExitCode = DiagnoseCertificate(url);
        CoUninitialize();
        return diagExitCode;
    }

    printf("GET %ls\n", url);
    PrintIgnoredCertificateChecks(certIgnoreFlags);

    int           exitCode  = 1;
    HttpCallback* callback  = new HttpCallback();
    ULONGLONG     startTick = GetTickCount64();

    hr = Fetch(url, certIgnoreFlags, false, callback);
    if (FAILED(hr))
        goto cleanup;

    if (FAILED(callback->m_result))
    {
        bool isCertError = false;
        const char* desc = DescribeError(callback->m_result, &isCertError);
        printf("Request failed: 0x%08lX %s\n", callback->m_result, desc);
        if (isCertError && !certIgnoreFlags)
        {
            // No response headers to compare with, so show the clock for the user to check
            printf("Hint: certificate problem. A wrong phone clock is the usual cause:\n      ");
            PrintPhoneClock();
            printf("      Run with --diag to see which certificate checks fail.\n");
        }
        goto cleanup;
    }

    CheckClockSkew(callback->m_date);

    printf("  HTTP %lu %ls, %lu bytes, %llu ms\n", callback->m_status, callback->m_reason,
           static_cast<unsigned long>(callback->m_body.size), GetTickCount64() - startTick);

    if (outFile)
    {
        if (SaveBody(outFile, callback->m_body))
            printf("Saved body to %ls\n", outFile);
        else
            printf("Could not write %ls\n", outFile);
    }

    // Non-200 answers are parsed too: Yahoo explains a 404 (unknown ticker) in JSON
    if (callback->m_status == 429)
        printf("Rate limited (429): Yahoo rejected the request (User-Agent / rate).\n");
    else if (ShowTicker(callback->m_body, storagePath, save))
        exitCode = 0;

cleanup:
    callback->Release();
    CoUninitialize();
    return exitCode;
}
