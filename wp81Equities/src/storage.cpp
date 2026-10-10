#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage.h"
#include "byte_buffer.h"
#include "cJSON/cJSON.h"

// ─── Records ─────────────────────────────────────────────────────────────────
void InitStorage(Storage* storage)
{
    storage->tickers  = nullptr;
    storage->count    = 0;
    storage->capacity = 0;
}

void FreeStorage(Storage* storage)
{
    free(storage->tickers);
    InitStorage(storage);
}

StoredTicker* FindTicker(Storage* storage, const char* symbol)
{
    for (int i = 0; i < storage->count; ++i)
    {
        if (strcmp(storage->tickers[i].symbol, symbol) == 0)
            return &storage->tickers[i];
    }
    return nullptr;
}

StoredTicker* AddTicker(Storage* storage, const char* symbol)
{
    if (storage->count == storage->capacity)
    {
        int newCapacity = storage->capacity ? storage->capacity * 2 : 64;
        StoredTicker* p = static_cast<StoredTicker*>(realloc(storage->tickers, sizeof(StoredTicker) * newCapacity));
        if (!p)
            return nullptr;
        storage->tickers  = p;
        storage->capacity = newCapacity;
    }
    StoredTicker* ticker = &storage->tickers[storage->count++];
    memset(ticker, 0, sizeof(*ticker));
    strncpy_s(ticker->symbol, _countof(ticker->symbol), symbol, _TRUNCATE);
    return ticker;
}

// ─── Loading ─────────────────────────────────────────────────────────────────
static void CopyJsonString(const cJSON* item, char* out, size_t outSize)
{
    out[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring)
        strncpy_s(out, outSize, item->valuestring, _TRUNCATE);
}

static void AddHistory(StoredTicker* ticker, const char* date, double close)
{
    // Keep the most recent STORED_HISTORY_MAX entries
    if (ticker->historyCount == STORED_HISTORY_MAX)
    {
        memmove(&ticker->history[0], &ticker->history[1], sizeof(StoredClose) * (STORED_HISTORY_MAX - 1));
        --ticker->historyCount;
    }
    StoredClose& entry = ticker->history[ticker->historyCount++];
    strncpy_s(entry.date, _countof(entry.date), date, _TRUNCATE);
    entry.close = close;
}

static bool LoadTicker(const cJSON* item, StoredTicker* ticker, char* error, size_t errorSize)
{
    if (!cJSON_IsObject(item))
    {
        _snprintf_s(error, errorSize, _TRUNCATE, "entry \"%s\" is not an object", ticker->symbol);
        return false;
    }

    CopyJsonString(cJSON_GetObjectItemCaseSensitive(item, "name"), ticker->name, _countof(ticker->name));
    CopyJsonString(cJSON_GetObjectItemCaseSensitive(item, "last_update"), ticker->lastUpdate, _countof(ticker->lastUpdate));

    const cJSON* streak = cJSON_GetObjectItemCaseSensitive(item, "streak");
    ticker->streak = cJSON_IsNumber(streak) ? streak->valueint : 0;

    const cJSON* history = cJSON_GetObjectItemCaseSensitive(item, "history");
    const cJSON* entry;
    cJSON_ArrayForEach(entry, history)
    {
        const cJSON* close = cJSON_GetObjectItemCaseSensitive(entry, "close");
        if (!cJSON_IsNumber(close))
            continue;
        char date[11];
        CopyJsonString(cJSON_GetObjectItemCaseSensitive(entry, "date"), date, _countof(date));
        AddHistory(ticker, date, close->valuedouble);
    }

    // Legacy record: "last_close" instead of "history" (same migration as the Python script)
    const cJSON* lastClose = cJSON_GetObjectItemCaseSensitive(item, "last_close");
    if (!history && cJSON_IsNumber(lastClose))
        AddHistory(ticker, ticker->lastUpdate, lastClose->valuedouble);

    const cJSON* category;
    cJSON_ArrayForEach(category, cJSON_GetObjectItemCaseSensitive(item, "categories"))
    {
        if (ticker->categoryCount < STORED_CATEGORIES_MAX && cJSON_IsString(category))
            CopyJsonString(category, ticker->categories[ticker->categoryCount++], _countof(ticker->categories[0]));
    }

    const cJSON* app = cJSON_GetObjectItemCaseSensitive(item, "app");
    ticker->hasApp = cJSON_IsNumber(app);
    ticker->app    = ticker->hasApp ? app->valuedouble : 0.0;
    return true;
}

bool LoadStorage(const wchar_t* path, Storage* storage, char* error, size_t errorSize)
{
    error[0] = '\0';
    FreeStorage(storage);

    FILE* f = nullptr;
    errno_t err = _wfopen_s(&f, path, L"rb");
    if (err == ENOENT)
        return true;   // first run: empty storage
    if (err != 0 || !f)
    {
        _snprintf_s(error, errorSize, _TRUNCATE, "cannot open the file (errno %d)", err);
        return false;
    }

    ByteBuffer content;
    char   chunk[4096];
    size_t read;
    bool   ok = true;
    while (ok && (read = fread(chunk, 1, sizeof(chunk), f)) > 0)
        ok = content.Append(chunk, read);
    fclose(f);
    if (!ok)
    {
        strncpy_s(error, errorSize, "out of memory", _TRUNCATE);
        return false;
    }
    if (content.size == 0)
        return true;

    const char* text = content.data;
    if (content.size >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0)
        text += 3;   // UTF-8 BOM

    const char* parseEnd = nullptr;
    cJSON* root = cJSON_ParseWithOpts(text, &parseEnd, false);
    if (!root)
    {
        // Line and column of the parse error, for a file edited by hand
        int line = 1, column = 1;
        for (const char* p = text; parseEnd && p < parseEnd && *p; ++p)
        {
            if (*p == '\n')
            {
                ++line;
                column = 1;
            }
            else
                ++column;
        }
        _snprintf_s(error, errorSize, _TRUNCATE, "invalid JSON at line %d, column %d (missing comma?)", line, column);
        return false;
    }
    if (!cJSON_IsObject(root))
    {
        strncpy_s(error, errorSize, "the top level is not a JSON object", _TRUNCATE);
        cJSON_Delete(root);
        return false;
    }

    const cJSON* item;
    cJSON_ArrayForEach(item, root)
    {
        StoredTicker* ticker = AddTicker(storage, item->string ? item->string : "");
        if (!ticker)
        {
            strncpy_s(error, errorSize, "out of memory", _TRUNCATE);
            ok = false;
            break;
        }
        if (!LoadTicker(item, ticker, error, errorSize))
        {
            ok = false;
            break;
        }
    }

    cJSON_Delete(root);
    if (!ok)
        FreeStorage(storage);
    return ok;
}

// ─── Saving (same text as Python json.dump(data, f, indent=4)) ──────────────
static void WriteIndent(ByteBuffer* out, int level)
{
    for (int i = 0; i < level; ++i)
        out->Append("    ");
}

// JSON string with Python's ensure_ascii escaping: non-ASCII as \uXXXX
// (surrogate pairs above U+FFFF), invalid UTF-8 as �
static void WriteString(ByteBuffer* out, const char* text)
{
    char escape[16];
    out->Append("\"");
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p)
    {
        unsigned c = *p;
        if (c < 0x80)
        {
            ++p;
            switch (c)
            {
            case '"':  out->Append("\\\""); break;
            case '\\': out->Append("\\\\"); break;
            case '\n': out->Append("\\n");  break;
            case '\r': out->Append("\\r");  break;
            case '\t': out->Append("\\t");  break;
            case '\b': out->Append("\\b");  break;
            case '\f': out->Append("\\f");  break;
            default:
                if (c < 0x20 || c == 0x7F)   // Python escapes all but ' '..'~'
                {
                    _snprintf_s(escape, _countof(escape), _TRUNCATE, "\\u%04x", c);
                    out->Append(escape);
                }
                else
                    out->Append(p - 1, 1);
            }
            continue;
        }

        // Decode one UTF-8 sequence
        int      length    = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        unsigned codePoint = length == 4 ? c & 0x07 : length == 3 ? c & 0x0F : c & 0x1F;
        bool     valid     = length != 0;
        for (int i = 1; valid && i < length; ++i)
        {
            if ((p[i] & 0xC0) != 0x80)
                valid = false;
            else
                codePoint = (codePoint << 6) | (p[i] & 0x3F);
        }
        if (!valid || codePoint > 0x10FFFF)
        {
            codePoint = 0xFFFD;
            length    = 1;
        }
        p += length;

        if (codePoint >= 0x10000)
        {
            codePoint -= 0x10000;
            _snprintf_s(escape, _countof(escape), _TRUNCATE, "\\u%04x\\u%04x",
                        0xD800 + (codePoint >> 10), 0xDC00 + (codePoint & 0x3FF));
        }
        else
            _snprintf_s(escape, _countof(escape), _TRUNCATE, "\\u%04x", codePoint);
        out->Append(escape);
    }
    out->Append("\"");
}

static void WriteStringOrNull(ByteBuffer* out, const char* text)
{
    if (text[0])
        WriteString(out, text);
    else
        out->Append("null");
}

// Shortest decimal text that reads back as the same double, like Python's
// float repr for prices (always with a decimal point: 88.0, 193.1)
static void WriteFloat(ByteBuffer* out, double value)
{
    char text[64];
    for (int decimals = 1; decimals <= 17; ++decimals)
    {
        _snprintf_s(text, _countof(text), _TRUNCATE, "%.*f", decimals, value);
        if (strtod(text, nullptr) == value)
            break;
    }
    out->Append(text);
}

static void WriteTicker(ByteBuffer* out, const StoredTicker& ticker)
{
    char number[32];

    out->Append("{\n");
    WriteIndent(out, 2);
    out->Append("\"history\": ");
    if (ticker.historyCount == 0)
        out->Append("[]");
    else
    {
        out->Append("[\n");
        for (int i = 0; i < ticker.historyCount; ++i)
        {
            WriteIndent(out, 3);
            out->Append("{\n");
            WriteIndent(out, 4);
            out->Append("\"date\": ");
            WriteStringOrNull(out, ticker.history[i].date);
            out->Append(",\n");
            WriteIndent(out, 4);
            out->Append("\"close\": ");
            WriteFloat(out, ticker.history[i].close);
            out->Append("\n");
            WriteIndent(out, 3);
            out->Append(i + 1 < ticker.historyCount ? "},\n" : "}\n");
        }
        WriteIndent(out, 2);
        out->Append("]");
    }
    out->Append(",\n");

    WriteIndent(out, 2);
    _snprintf_s(number, _countof(number), _TRUNCATE, "\"streak\": %d,\n", ticker.streak);
    out->Append(number);

    WriteIndent(out, 2);
    out->Append("\"name\": ");
    WriteStringOrNull(out, ticker.name);
    out->Append(",\n");

    WriteIndent(out, 2);
    out->Append("\"last_update\": ");
    WriteStringOrNull(out, ticker.lastUpdate);
    out->Append(",\n");

    WriteIndent(out, 2);
    out->Append("\"categories\": ");
    if (ticker.categoryCount == 0)
        out->Append("[]");
    else
    {
        out->Append("[\n");
        for (int i = 0; i < ticker.categoryCount; ++i)
        {
            WriteIndent(out, 3);
            WriteString(out, ticker.categories[i]);
            out->Append(i + 1 < ticker.categoryCount ? ",\n" : "\n");
        }
        WriteIndent(out, 2);
        out->Append("]");
    }

    if (ticker.hasApp)
    {
        out->Append(",\n");
        WriteIndent(out, 2);
        out->Append("\"app\": ");
        WriteFloat(out, ticker.app);
    }
    out->Append("\n");
    WriteIndent(out, 1);
    out->Append("}");
}

bool SaveStorage(const wchar_t* path, const Storage& storage, char* error, size_t errorSize)
{
    error[0] = '\0';

    ByteBuffer out;
    if (storage.count == 0)
        out.Append("{}");
    else
    {
        out.Append("{\n");
        for (int i = 0; i < storage.count; ++i)
        {
            WriteIndent(&out, 1);
            WriteString(&out, storage.tickers[i].symbol);
            out.Append(": ");
            WriteTicker(&out, storage.tickers[i]);
            out.Append(i + 1 < storage.count ? ",\n" : "\n");
        }
        out.Append("}");
    }
    if (!out.data)
    {
        strncpy_s(error, errorSize, "out of memory", _TRUNCATE);
        return false;
    }

    // Write a temporary file, then replace: a crash never leaves a half-written file
    wchar_t tempPath[MAX_PATH];
    if (_snwprintf_s(tempPath, _countof(tempPath), _TRUNCATE, L"%s.tmp", path) < 0)
    {
        strncpy_s(error, errorSize, "path too long", _TRUNCATE);
        return false;
    }

    FILE* f = nullptr;
    errno_t err = _wfopen_s(&f, tempPath, L"wb");
    if (err != 0 || !f)
    {
        _snprintf_s(error, errorSize, _TRUNCATE, "cannot create the temporary file (errno %d)", err);
        return false;
    }
    bool written = fwrite(out.data, 1, out.size, f) == out.size;
    written = fclose(f) == 0 && written;
    if (!written)
    {
        DeleteFileW(tempPath);
        strncpy_s(error, errorSize, "cannot write the temporary file", _TRUNCATE);
        return false;
    }

    if (!MoveFileExW(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DWORD lastError = GetLastError();
        DeleteFileW(tempPath);
        _snprintf_s(error, errorSize, _TRUNCATE, "cannot replace the file (error %lu)", lastError);
        return false;
    }
    return true;
}
