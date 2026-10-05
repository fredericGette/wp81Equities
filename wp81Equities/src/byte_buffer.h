#pragma once

#include <stdlib.h>
#include <string.h>

// ─── Growable byte buffer (avoids depending on Msvcp110.dll) ─────────────────
// The content is always NUL-terminated, so it can be used as a C string.
struct ByteBuffer
{
    char*  data;
    size_t size;
    size_t capacity;

    ByteBuffer() : data(nullptr), size(0), capacity(0) {}
    ~ByteBuffer() { free(data); }

    bool Append(const void* src, size_t len)
    {
        // Keep one spare byte so the content can always be NUL-terminated
        if (size + len + 1 > capacity)
        {
            size_t newCapacity = capacity ? capacity : 4096;
            while (size + len + 1 > newCapacity)
                newCapacity *= 2;
            char* p = static_cast<char*>(realloc(data, newCapacity));
            if (!p)
                return false;
            data     = p;
            capacity = newCapacity;
        }
        memcpy(data + size, src, len);
        size += len;
        data[size] = '\0';
        return true;
    }

    bool Append(const char* text)
    {
        return Append(text, strlen(text));
    }

private:
    // Owns its memory: copying would free it twice
    ByteBuffer(const ByteBuffer&);
    ByteBuffer& operator=(const ByteBuffer&);
};
