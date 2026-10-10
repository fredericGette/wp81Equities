#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tracker.h"

double Round2(double value)
{
    double rounded = floor(value * 100.0 + 0.5) / 100.0;
    return rounded == 0.0 ? 0.0 : rounded;   // avoids printing "-0.00"
}

PriceSummary SummarizePrices(const DailyClose* closes, int count)
{
    PriceSummary summary;
    summary.close     = Round2(closes[count - 1].close);
    double prev       = count > 1 ? Round2(closes[count - 2].close) : summary.close;
    summary.change    = Round2(summary.close - prev);
    summary.changePct = prev != 0.0 ? Round2(summary.change / prev * 100.0) : 0.0;

    int first = count > HISTORY_SIZE ? count - HISTORY_SIZE : 0;
    summary.spanDays   = count - 1 - first;
    summary.spanChange = 0.0;
    summary.spanPct    = 0.0;
    if (summary.spanDays > 0)
    {
        double oldest      = Round2(closes[first].close);
        summary.spanChange = Round2(summary.close - oldest);
        summary.spanPct    = oldest != 0.0 ? Round2(summary.spanChange / oldest * 100.0) : 0.0;
    }
    return summary;
}

static double Clamp(double value, double low, double high)
{
    return value < low ? low : value > high ? high : value;
}

double ComputeScore(double changeTodayPct, double change4dPct, int streakDays)
{
    // 1. Four-day momentum: +10% = 100, 0% = 50, -10% = 0
    double momentumScore = Clamp(50.0 + 5.0 * change4dPct, 0.0, 100.0);

    // 2. Today's confirmation: +5% = 100, 0% = 50, -5% = 0
    double todayScore = Clamp(50.0 + 10.0 * changeTodayPct, 0.0, 100.0);

    // 3. Momentum consistency
    double consistencyBonus;
    if (change4dPct > 0 && changeTodayPct > 0)
        consistencyBonus = 10;
    else if (change4dPct > 0 && changeTodayPct < 0)
        consistencyBonus = -5;
    else if (change4dPct < 0 && changeTodayPct > 0)
        consistencyBonus = 3;
    else
        consistencyBonus = -5;

    // 4. Streak bonus (the algorithm says "== 3"; streaks longer than 3 days
    //    get the same bonus, as they can exceed 3 with the stored history)
    double streakBonus = streakDays >= 3 ? 10 : streakDays == 2 ? 6 : streakDays == 1 ? 2 : 0;

    // 5. Extreme fall: do not read a crash as normal momentum
    double extremePenalty = change4dPct <= -30 ? -25 : change4dPct <= -15 ? -15 : change4dPct <= -10 ? -8 : 0;

    // 6. Overbought: do not chase stocks that already exploded
    double overboughtPenalty = change4dPct >= 20 ? -15 : change4dPct >= 10 ? -8 : 0;

    // 7. Final score
    double score = 0.50 * momentumScore + 0.30 * todayScore + consistencyBonus + streakBonus
                 + extremePenalty + overboughtPenalty;
    return Clamp(score, 0.0, 100.0);
}

int ComputeReboundScore(double changeTodayPct, double change4dPct)
{
    int rebound = change4dPct <= -30 ? 70 : change4dPct <= -15 ? 55 : change4dPct <= -10 ? 45 : 20;
    if (changeTodayPct > 0)
        rebound += 15;
    if (changeTodayPct > 3)
        rebound += 10;
    return rebound;
}

ScoreClass ClassifyScore(double score)
{
    if (score >= 75)
        return SCORE_STRONG;
    if (score >= 65)
        return SCORE_INTERESTING;
    if (score >= 55)
        return SCORE_WATCH;
    if (score >= 45)
        return SCORE_NEUTRAL;
    return SCORE_WEAK;
}

const char* ScoreClassName(ScoreClass scoreClass)
{
    static const char* NAMES[SCORE_CLASS_COUNT] = { "STRONG SHORT-TERM", "INTERESTING", "WATCH", "NEUTRAL", "WEAK" };
    return NAMES[scoreClass];
}

const char* ScoreClassRange(ScoreClass scoreClass)
{
    static const char* RANGES[SCORE_CLASS_COUNT] = { "75+", "65-75", "55-65", "45-55", "<45" };
    return RANGES[scoreClass];
}

Position ComputePosition(double close, double app)
{
    Position position;
    double   price   = Round2(app);
    position.gain    = Round2(Round2(close) - price);
    position.gainPct = price != 0.0 ? Round2(position.gain / price * 100.0) : 0.0;
    return position;
}

RankGroup ClassifyRank(bool held, double gain, double changeToday)
{
    if (!held)
        return RANK_NOT_HELD;
    if (changeToday <= 0)
        return RANK_HELD_OTHER;
    return gain >= 0 ? RANK_HELD_GAIN_UP : RANK_HELD_LOSS_UP;
}

const char* RankGroupName(RankGroup group)
{
    static const char* NAMES[RANK_GROUP_COUNT] = { "HELD: GAIN, UP TODAY", "HELD: LOSS, UP TODAY",
                                                   "HELD: FLAT OR DOWN TODAY", "NOT HELD" };
    return NAMES[group];
}

static void ApplyCloses(StreakResult* result, const DailyClose* closes, int from, int count)
{
    for (int i = from; i < count; ++i)
    {
        double close = Round2(closes[i].close);
        double prev  = Round2(closes[i - 1].close);
        if (close > prev)
            ++result->streak;
        else if (close < prev)
            result->streak = 0;
    }
    strncpy_s(result->lastDate, _countof(result->lastDate), closes[count - 1].date, _TRUNCATE);
}

StreakResult ContinueStreak(int storedStreak, const char* storedDate, const DailyClose* closes, int count)
{
    StreakResult result;
    result.streak = storedStreak;
    strncpy_s(result.lastDate, _countof(result.lastDate), storedDate ? storedDate : "", _TRUNCATE);

    bool hasStored = storedDate && storedDate[0];
    if (count == 0)
    {
        result.source = hasStored ? STREAK_UP_TO_DATE : STREAK_RECOMPUTED;
        return result;
    }

    // Dates are "YYYY-MM-DD", so strcmp orders them chronologically
    if (hasStored)
    {
        int cmp = strcmp(storedDate, closes[count - 1].date);
        if (cmp >= 0)
        {
            result.source = cmp == 0 ? STREAK_UP_TO_DATE : STREAK_STORAGE_NEWER;
            return result;
        }

        if (strcmp(storedDate, closes[0].date) >= 0)
        {
            // Anchor on the last close on or before the stored date
            int anchor = 0;
            while (anchor + 1 < count && strcmp(closes[anchor + 1].date, storedDate) <= 0)
                ++anchor;
            ApplyCloses(&result, closes, anchor + 1, count);
            result.source = STREAK_CONTINUED;
            return result;
        }
    }

    // No stored state, or a gap since the stored date: start from the closes
    result.streak = 0;
    ApplyCloses(&result, closes, 1, count);
    result.source = STREAK_RECOMPUTED;
    return result;
}
