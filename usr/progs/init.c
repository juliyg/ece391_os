// first program ran on intialiation. sets up AB1043 syscall and shells

#include "../string.h"
#include "../syscall.h"
#include "../shell.h"

#define NUART 2
#define BUFSZ 16
#define SHELL_PATH "/c/shell"
#define UART_PATH "/dev/uart"

#define LEAP_YEAR(y) (((y)%4 == 0 && (y)%100 != 0) || ((y)%400 == 0))

#define NSEC_PER_SEC (1000000000UL)
#define NSEC_PER_MIN (60UL * NSEC_PER_SEC)
#define NSEC_PER_HR (60UL * NSEC_PER_MIN)
#define NSEC_PER_DAY (24UL * NSEC_PER_HR)

static const int month_days[] = {
    31, 28, 31, 30, 31, 30,
    31, 31, 30, 31, 30, 31
};

static const char * const ab1043_bracket[] = {
    [AB1043_B1] = "<13",
    [AB1043_B2] = "13-15",
    [AB1043_B3] = "16-17",
    [AB1043_B4] = "18+"
};

static unsigned long long date_to_unix_ts(int year, int month, int day) {
    unsigned long long days_since_1970 = 0;

    if (year < 1970) // before unix epoch
        return 0;

    for (int y = 1970; y < year; y++) 
        days_since_1970 += LEAP_YEAR(y) ? 366 : 365;

    for (int m = 0; m < month - 1; m++)
        days_since_1970 += month_days[month - 1];

    if (LEAP_YEAR(year) && month > 2) days_since_1970++;
    
    days_since_1970 += day-1;

    return days_since_1970 * NSEC_PER_DAY;
}

// Parses a date string in the form YYYY-MM-DD. Returns unix timestamp of
// that date through /dob_unix/. Returns 0 on success and -1 on failure.
//
// On entry parse_date() assumes:
// - /buf/ is a valid NUL-terminated string.
// - /dob_unix/ is non-NULL.
//
// On return pars_date() guarantees (on success):
// - /dob_unix/ stores a unix timestamp representing the passed in date.
static int parse_date(char * buf, unsigned long long * dob_unix) {
    char * nptr = buf;
    int day, month, year;

    if (*nptr == '\0') return -1;

    year = strtoul(nptr, &nptr, 10);
    if (*(nptr++) == '\0') return -1;

    month = strtoul(nptr, &nptr, 10);
    if (*(nptr++) == '\0') return -1;

    day = strtoul(nptr, &nptr, 10);

    if (month < 1 || month > 12) return -1;
    if (day < 1 || day > month_days[month-1]) {
        // check leap year special case
        if (!(month == 2 && day == 29 && LEAP_YEAR(year))) 
            return -1;
    }

    // date validated
    *dob_unix = date_to_unix_ts(year, month, day);
    return 0;
}


void main() {
        unsigned long long dob;
    int ret = parse_date("1970-01-01", &dob);

    if (ret < 0) {
        _print("parse_date failed\n");
        return;
    }

    int bracket = _ab1043(dob);

    _print("AB1043 bracket: ");
    _print(ab1043_bracket[bracket]);
    _print("\n");

    return;
}