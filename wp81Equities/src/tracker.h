#pragma once

#include "yahoo_chart.h"

// Tracking rules of euronext_tracker.py. Only depends on the CRT, so it can
// also be compiled for host-side tests.

// Closes in the multi-day change and in the storage history, as in the Python tracker
static const int HISTORY_SIZE = 5;

// Rounds a price to 2 decimals; prices are compared after rounding.
double Round2(double value);

struct PriceSummary
{
    double close;        // last close
    double change;       // vs the previous close (0 when there is none)
    double changePct;
    int    spanDays;     // intervals in the multi-day change, 0 when fewer than 2 closes
    double spanChange;   // last close vs the oldest of the last HISTORY_SIZE closes
    double spanPct;
};

// closes: oldest first, count >= 1. All values rounded to 2 decimals.
PriceSummary SummarizePrices(const DailyClose* closes, int count);

// Short-term momentum score, 0 to 100, from the change today (%), the
// multi-day change (%) and the up-day streak. See tracker.cpp for the rules.
double ComputeScore(double changeTodayPct, double change4dPct, int streakDays);

enum ScoreClass
{
    SCORE_STRONG,        // >= 75
    SCORE_INTERESTING,   // >= 65
    SCORE_WATCH,         // >= 55
    SCORE_NEUTRAL,       // >= 45
    SCORE_WEAK,          // < 45
    SCORE_CLASS_COUNT
};

// Rebound score, 20 to 95: high after a large multi-day fall, more so when
// today is up. Shown beside the score; does not change the sort order.
int ComputeReboundScore(double changeTodayPct, double change4dPct);

ScoreClass  ClassifyScore(double score);
const char* ScoreClassName(ScoreClass scoreClass);    // e.g. "STRONG SHORT-TERM"
const char* ScoreClassRange(ScoreClass scoreClass);   // e.g. "75+"

enum StreakSource
{
    STREAK_CONTINUED,       // stored streak continued with the newer closes
    STREAK_RECOMPUTED,      // no usable stored state: computed from the closes only
    STREAK_UP_TO_DATE,      // stored state already covers the newest close
    STREAK_STORAGE_NEWER,   // stored state is newer than these closes (old data)
};

struct StreakResult
{
    int          streak;
    char         lastDate[11];   // date of the last close counted ("" if none)
    StreakSource source;
};

// Up-day streak: +1 on a higher close, reset to 0 on a lower one, unchanged
// on an equal one. Continues storedStreak (counted up to storedDate, "" if
// none) with the closes after storedDate. When storedDate is older than the
// first close, the streak is recomputed from the closes alone.
// closes: oldest first, finished sessions only.
StreakResult ContinueStreak(int storedStreak, const char* storedDate, const DailyClose* closes, int count);
