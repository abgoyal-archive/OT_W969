

#ifndef DateComponents_h
#define DateComponents_h

#include <limits>
#include <wtf/unicode/Unicode.h>

namespace WebCore {

class String;

// A DateComponents instance represents one of the following date and time combinations:
// * Month type: year-month
// * Date type: year-month-day
// * Week type: year-week
// * Time type: hour-minute-second-millisecond
// * DateTime or DateTimeLocal type: year-month-day hour-minute-second-millisecond
class DateComponents {
public:
    DateComponents()
        : m_millisecond(0)
        , m_second(0)
        , m_minute(0)
        , m_hour(0)
        , m_monthDay(0)
        , m_month(0)
        , m_year(0)
        , m_week(0)
        , m_type(Invalid)
    {
    }

    int millisecond() const { return m_millisecond; }
    int second() const { return m_second; }
    int minute() const { return m_minute; }
    int hour() const { return m_hour; }
    int monthDay() const { return m_monthDay; }
    int month() const { return m_month; }
    int fullYear() const { return m_year; }
    int week() const { return m_week; }

    enum SecondFormat {
        None, // Suppress the second part and the millisecond part if they are 0.
        Second, // Always show the second part, and suppress the millisecond part if it is 0.
        Millisecond // Always show the second part and the millisecond part.
    };

    // Returns an ISO 8601 representation for this instance.
    // The format argument is valid for DateTime, DateTimeLocal, and Time types.
    String toString(SecondFormat format = None) const;

    // parse*() and setMillisecondsSince*() functions are initializers for an
    // DateComponents instance. If these functions return false, the instance
    // might be invalid.

    // The following six functions parse the input 'src' whose length is
    // 'length', and updates some fields of this instance. The parsing starts at
    // src[start] and examines characters before src[length].
    // 'src' must be non-null. The 'src' string doesn't need to be
    // null-terminated.
    // The functions return true if the parsing succeeds, and set 'end' to the
    // next index after the last consumed. Extra leading characters cause parse
    // failures, and the trailing extra characters don't cause parse failures.

    // Sets year and month.
    bool parseMonth(const UChar* src, unsigned length, unsigned start, unsigned& end);
    // Sets year, month and monthDay.
    bool parseDate(const UChar* src, unsigned length, unsigned start, unsigned& end);
    // Sets year and week.
    bool parseWeek(const UChar* src, unsigned length, unsigned start, unsigned& end);
    // Sets hour, minute, second and millisecond.
    bool parseTime(const UChar* src, unsigned length, unsigned start, unsigned& end);
    // Sets year, month, monthDay, hour, minute, second and millisecond.
    bool parseDateTimeLocal(const UChar* src, unsigned length, unsigned start, unsigned& end);
    // Sets year, month, monthDay, hour, minute, second and millisecond, and adjusts timezone.
    bool parseDateTime(const UChar* src, unsigned length, unsigned start, unsigned& end);

    // The following setMillisecondsSinceEpochFor*() functions take
    // the number of milliseconds since 1970-01-01 00:00:00.000 UTC as
    // the argument, and update all fields for the corresponding
    // DateComponents type. The functions return true if it succeeds, and
    // false if they fail.

    // For Date type. Updates m_year, m_month and m_monthDay.
    bool setMillisecondsSinceEpochForDate(double ms);
    // For DateTime type. Updates m_year, m_month, m_monthDay, m_hour, m_minute, m_second and m_millisecond.
    bool setMillisecondsSinceEpochForDateTime(double ms);
    // For DateTimeLocal type. Updates m_year, m_month, m_monthDay, m_hour, m_minute, m_second and m_millisecond.
    bool setMillisecondsSinceEpochForDateTimeLocal(double ms);
    // For Month type. Updates m_year and m_month.
    bool setMillisecondsSinceEpochForMonth(double ms);
    // For Week type. Updates m_year and m_week.
    bool setMillisecondsSinceEpochForWeek(double ms);

    // For Time type. Updates m_hour, m_minute, m_second and m_millisecond.
    bool setMillisecondsSinceMidnight(double ms);

    // Another initializer for Month type. Updates m_year and m_month.
    bool setMonthsSinceEpoch(double months);

    // Returns the number of milliseconds from 1970-01-01 00:00:00 UTC.
    // For a DateComponents initialized with parseDateTimeLocal(),
    // millisecondsSinceEpoch() returns a value for UTC timezone.
    double millisecondsSinceEpoch() const;
    // Returns the number of months from 1970-01.
    // Do not call this for types other than Month.
    double monthsSinceEpoch() const;
    static inline double invalidMilliseconds() { return std::numeric_limits<double>::quiet_NaN(); }

private:
    // Returns the maximum week number in this DateComponents's year.
    // The result is either of 52 and 53.
    int maxWeekNumberInYear() const;
    bool parseYear(const UChar* src, unsigned length, unsigned start, unsigned& end);
    bool addDay(int);
    bool addMinute(int);
    bool parseTimeZone(const UChar* src, unsigned length, unsigned start, unsigned& end);
    // Helper for millisecondsSinceEpoch().
    double millisecondsSinceEpochForTime() const;
    // Helpers for setMillisecondsSinceEpochFor*().
    bool setMillisecondsSinceEpochForDateInternal(double ms);
    void setMillisecondsSinceMidnightInternal(double ms);
    // Helper for toString().
    String toStringForTime(SecondFormat) const;

    // m_weekDay values
    enum {
        Sunday = 0,
        Monday,
        Tuesday,
        Wednesday,
        Thursday,
        Friday,
        Saturday,
    };

    int m_millisecond; // 0 - 999
    int m_second;
    int m_minute;
    int m_hour;
    int m_monthDay; // 1 - 31
    int m_month; // 0:January - 11:December
    int m_year; //  1582 -
    int m_week; // 1 - 53

    enum Type {
        Invalid,
        Date,
        DateTime,
        DateTimeLocal,
        Month,
        Time,
        Week,
    };
    Type m_type;
};


} // namespace WebCore

#endif // DateComponents_h
