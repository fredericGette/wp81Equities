#pragma once

#include <stddef.h>

// euronext_storage.json, shared with euronext_tracker.py:
// {
//     "AIR.PA": {
//         "history": [ { "date": "2026-10-01", "close": 185.6 }, ... ],   (oldest first)
//         "streak": 1,
//         "name": "Airbus SE",
//         "last_update": "2026-10-02",
//         "categories": [ "aerospace", "defense", "aviation" ],
//         "app": 172.4                                                    (optional)
//     },
//     "NEW.PA": { "categories": [ "industrials" ] },                     (added by hand)
//     ...
// }
// The file is the list of tracked tickers: a record added by hand needs
// only "categories"; the other properties are filled by the next scan.
// "app" is the Average Purchase Price of a held equity, set by hand.
// Legacy records with "last_close" instead of "history" are migrated on load.

static const int STORED_HISTORY_MAX    = 32;
static const int STORED_CATEGORIES_MAX = 8;

struct StoredClose
{
    char   date[11];       // "YYYY-MM-DD", "" = null
    double close;
};

struct StoredTicker
{
    char        symbol[32];
    char        name[160];         // UTF-8, "" = null
    char        lastUpdate[11];    // date of the last close counted in streak, "" = null
    int         streak;
    StoredClose history[STORED_HISTORY_MAX];
    int         historyCount;
    char        categories[STORED_CATEGORIES_MAX][48];
    int         categoryCount;
    bool        hasApp;            // "app" present
    double      app;               // Average Purchase Price
};

struct Storage
{
    StoredTicker* tickers;
    int           count;
    int           capacity;
};

void InitStorage(Storage* storage);
void FreeStorage(Storage* storage);

// A missing file gives an empty storage. Returns false, with the reason in
// error, when the file exists but cannot be used: it must not be overwritten.
bool LoadStorage(const wchar_t* path, Storage* storage, char* error, size_t errorSize);

// Writes the same layout as Python's json.dump(indent=4) (non-ASCII escaped
// as \uXXXX), to a temporary file first, then renames it over path.
bool SaveStorage(const wchar_t* path, const Storage& storage, char* error, size_t errorSize);

StoredTicker* FindTicker(Storage* storage, const char* symbol);

// Appends an empty record; returns nullptr when out of memory
StoredTicker* AddTicker(Storage* storage, const char* symbol);
