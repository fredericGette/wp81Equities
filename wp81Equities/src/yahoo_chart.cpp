// Parsing of the Yahoo Finance chart JSON with cJSON.
//
// Expected shape (only the fields used here):
// {"chart":{"result":[{
//     "meta":{"symbol":"AIR.PA","longName":"Airbus SE","currency":"EUR",
//             "exchangeTimezoneName":"Europe/Paris","gmtoffset":7200,
//             "regularMarketPrice":189.3,"regularMarketTime":1790955322,
//             "currentTradingPeriod":{"regular":{"start":...,"end":...}}},
//     "timestamp":[1789974000,...],
//     "indicators":{"quote":[{"close":[194.77999877929688,null,...]}]}}],
//   "error":null}}
// Unknown ticker (HTTP 404):
// {"chart":{"result":null,"error":{"code":"Not Found","description":"No data found, symbol may be delisted"}}}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yahoo_chart.h"
#include "cJSON/cJSON.h"

static const cJSON* GetItem(const cJSON* object, const char* key)
{
    return object ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

static bool CopyString(const cJSON* item, char* out, size_t outSize)
{
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
        return false;
    strncpy_s(out, outSize, item->valuestring, _TRUNCATE);
    return true;
}

static double GetNumber(const cJSON* item, double fallback)
{
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

void FormatDate(long long unixSeconds, long offsetSeconds, char* out, size_t outSize)
{
    // Days since 1970-01-01, then Howard Hinnant's civil_from_days
    long long days = (unixSeconds + offsetSeconds) / 86400;
    if (unixSeconds + offsetSeconds < 0 && (unixSeconds + offsetSeconds) % 86400 != 0)
        --days;

    days += 719468;
    long long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned  doe = static_cast<unsigned>(days - era * 146097);            // [0, 146096]
    unsigned  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    long long y   = static_cast<long long>(yoe) + era * 400;
    unsigned  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
    unsigned  mp  = (5 * doy + 2) / 153;                                   // [0, 11]
    unsigned  d   = doy - (153 * mp + 2) / 5 + 1;                          // [1, 31]
    unsigned  m   = mp < 10 ? mp + 3 : mp - 9;                             // [1, 12]
    if (m <= 2)
        ++y;

    _snprintf_s(out, outSize, _TRUNCATE, "%04lld-%02u-%02u", y, m, d);
}

static bool Fail(char* error, size_t errorSize, const char* message)
{
    strncpy_s(error, errorSize, message, _TRUNCATE);
    return false;
}

bool ParseChart(const char* json, ChartData* chart, char* error, size_t errorSize)
{
    memset(chart, 0, sizeof(*chart));
    error[0] = '\0';

    cJSON* root = cJSON_Parse(json);
    if (!root)
    {
        const char* where = cJSON_GetErrorPtr();
        if (!where || !where[0])
            return Fail(error, errorSize, "invalid JSON (truncated response?)");
        char message[96];
        _snprintf_s(message, _countof(message), _TRUNCATE, "invalid JSON near \"%.20s\"", where);
        return Fail(error, errorSize, message);
    }

    bool ok = false;
    const cJSON* chartItem = GetItem(root, "chart");
    const cJSON* yahooError = GetItem(chartItem, "error");
    const cJSON* result = cJSON_GetArrayItem(GetItem(chartItem, "result"), 0);

    if (!chartItem)
        Fail(error, errorSize, "no \"chart\" object");
    else if (yahooError && !cJSON_IsNull(yahooError))
    {
        char code[64] = "?", description[160] = "";
        CopyString(GetItem(yahooError, "code"), code, _countof(code));
        CopyString(GetItem(yahooError, "description"), description, _countof(description));
        _snprintf_s(error, errorSize, _TRUNCATE, "Yahoo error: %s (%s)", code, description);
    }
    else if (!result)
        Fail(error, errorSize, "no chart result");
    else
    {
        const cJSON* meta = GetItem(result, "meta");
        CopyString(GetItem(meta, "symbol"), chart->symbol, _countof(chart->symbol));
        if (!CopyString(GetItem(meta, "longName"), chart->name, _countof(chart->name)) &&
            !CopyString(GetItem(meta, "shortName"), chart->name, _countof(chart->name)))
            strncpy_s(chart->name, _countof(chart->name), chart->symbol, _TRUNCATE);
        CopyString(GetItem(meta, "currency"), chart->currency, _countof(chart->currency));
        CopyString(GetItem(meta, "exchangeTimezoneName"), chart->exchangeTimezone, _countof(chart->exchangeTimezone));
        chart->gmtOffset          = static_cast<long>(GetNumber(GetItem(meta, "gmtoffset"), 0));
        chart->regularMarketPrice = GetNumber(GetItem(meta, "regularMarketPrice"), 0);
        chart->regularMarketTime  = static_cast<long long>(GetNumber(GetItem(meta, "regularMarketTime"), 0));

        const cJSON* regular = GetItem(GetItem(meta, "currentTradingPeriod"), "regular");
        chart->sessionStart = static_cast<long long>(GetNumber(GetItem(regular, "start"), 0));
        chart->sessionEnd   = static_cast<long long>(GetNumber(GetItem(regular, "end"), 0));

        const cJSON* timestamps = GetItem(result, "timestamp");
        const cJSON* quote      = cJSON_GetArrayItem(GetItem(GetItem(result, "indicators"), "quote"), 0);
        const cJSON* closes     = GetItem(quote, "close");

        int count = 0;
        if (cJSON_IsArray(timestamps) && cJSON_IsArray(closes))
        {
            count = cJSON_GetArraySize(timestamps);
            if (cJSON_GetArraySize(closes) < count)
                count = cJSON_GetArraySize(closes);
        }

        // Keep the most recent CHART_MAX_CLOSES non-null closes, oldest first
        for (int i = 0; i < count; ++i)
        {
            const cJSON* timestamp = cJSON_GetArrayItem(timestamps, i);
            const cJSON* close     = cJSON_GetArrayItem(closes, i);
            if (!cJSON_IsNumber(timestamp) || !cJSON_IsNumber(close))
                continue;   // Yahoo sends null for missing bars

            if (chart->closeCount == CHART_MAX_CLOSES)
            {
                memmove(&chart->closes[0], &chart->closes[1], sizeof(DailyClose) * (CHART_MAX_CLOSES - 1));
                --chart->closeCount;
            }
            DailyClose& entry = chart->closes[chart->closeCount++];
            FormatDate(static_cast<long long>(timestamp->valuedouble), chart->gmtOffset,
                       entry.date, _countof(entry.date));
            entry.close = close->valuedouble;
        }

        if (chart->closeCount == 0)
            Fail(error, errorSize, "no close price in the response");
        else
            ok = true;
    }

    cJSON_Delete(root);
    return ok;
}
