#pragma once

#include <stddef.h>

// Parsed Yahoo Finance /v8/finance/chart response (range=10d, interval=1d).
// Only depends on the CRT, so it can also be compiled for host-side tests.

static const int CHART_MAX_CLOSES = 32;

struct DailyClose
{
    char   date[11];           // exchange-local date, "YYYY-MM-DD"
    double close;
};

struct ChartData
{
    char       symbol[32];
    char       name[160];          // longName, else shortName, else symbol (UTF-8)
    char       currency[8];
    char       exchangeTimezone[48];
    long       gmtOffset;          // seconds: exchange local time = UTC + gmtOffset
    double     regularMarketPrice;
    long long  regularMarketTime;  // unix seconds
    long long  sessionStart;       // currentTradingPeriod.regular, unix seconds
    long long  sessionEnd;
    DailyClose closes[CHART_MAX_CLOSES];   // oldest first, null closes skipped
    int        closeCount;
};

// Parses a chart response body. Returns false on malformed JSON, on a Yahoo
// "error" object (e.g. unknown ticker) or when no close is available; the
// reason is written to error.
bool ParseChart(const char* json, ChartData* chart, char* error, size_t errorSize);

// Formats unix seconds shifted by offsetSeconds as "YYYY-MM-DD"
void FormatDate(long long unixSeconds, long offsetSeconds, char* out, size_t outSize);
