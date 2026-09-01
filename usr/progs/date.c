// YOUR CODE HERE
#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"

#define LEAP_YEAR(y) (((y)%4 == 0 && (y)%100 != 0) || ((y)%400 == 0))

#define NSEC_PER_SEC (1000000000UL)
#define NSEC_PER_MIN (60UL * NSEC_PER_SEC)
#define NSEC_PER_HR (60UL * NSEC_PER_MIN)
#define NSEC_PER_DAY (24UL * NSEC_PER_HR)

static const int month_days[] = {
    31, 28, 31, 30, 31, 30,
    31, 31, 30, 31, 30, 31
};

static const char* months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" 
};

/*static const char* days_of_week[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};*/

void main(int argc, char** argv) {


    unsigned long long time;
    int fd = _open(-1, "/dev/rtc");
    long res = _read(fd, &time, 8);

    if (res < 8){
        dprintf(2, "%s", "FAILED TO FETCH TIME");
        return;
    }

    unsigned long long days = time / NSEC_PER_DAY;
    
    unsigned long long overflow_day = time % NSEC_PER_DAY;
    unsigned long long hours_cur_day = overflow_day / NSEC_PER_HR;
    unsigned long long overflow_min =  overflow_day % NSEC_PER_HR;
    unsigned long long mins_cur_hour = overflow_min / NSEC_PER_MIN;
    unsigned long long overflow_sec = overflow_min % NSEC_PER_MIN;
    unsigned long long seconds_cur_min = overflow_sec / NSEC_PER_SEC;


    unsigned long long year = 1970;
    unsigned long long month;
    
    //find cur year including leap years
    while ((LEAP_YEAR(year) && days >= 366) || (!LEAP_YEAR(year) && days >= 365)){
        if (LEAP_YEAR(year)){
            days -= 366;
        }
        else{
            days -=365;
        }
        year++;
    }

    //find cur month
    for (int i = 0; i < 12; i++){
        if (days >= 29 && i == 1 && LEAP_YEAR(year)){
                days -= 29;
                continue;
            }
        else if (days >= month_days[i]){
            days -= month_days[i];
        }
        else{
            month = i;
            break;
        }
    }

    //day of the week & month
    int day_of_month = days+1;

    dprintf(1, "%02d %s, %llu %02d:%02d:%02d\n", day_of_month, months[month], year, (int)hours_cur_day, (int)mins_cur_hour, (int)seconds_cur_min);
}