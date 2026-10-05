#pragma once

// Tickers scanned by default, with their categories (from euronext_tracker.py)

static const int EQUITY_MAX_CATEGORIES = 3;

struct Equity
{
    const char* symbol;
    const char* categories[EQUITY_MAX_CATEGORIES];   // unused slots are nullptr
};

extern const Equity EQUITIES[];
extern const int    EQUITY_COUNT;

// Returns nullptr when symbol is not in EQUITIES
const Equity* FindEquity(const char* symbol);
