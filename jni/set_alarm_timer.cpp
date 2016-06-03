#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <linux/ioctl.h>
#include <linux/android_alarm.h>
#include <linux/rtc.h>

/*
int timerfd_create(int clockid, int flags);

int timerfd_settime(int fd, int flags,
                   const struct itimerspec *new_value,
                   struct itimerspec *old_value);

int timerfd_gettime(int fd, struct itimerspec *curr_value);

---------------------------------------------------------------------
timerfd_create() creates a new timer object, and returns a file
       descriptor that refers to that timer.
       
timerfd_settime() arms (starts) or disarms (stops) the timer referred
       to by the file descriptor fd.
       
       struct timespec {
           time_t tv_sec;                // Seconds 
           long   tv_nsec;               // Nanoseconds 
       };

       struct itimerspec {
           struct timespec it_interval;  // Interval for periodic timer 
           struct timespec it_value;     // Initial expiration 
       };

       Setting both fields of new_value.it_value to zero disarms the timer.
       If both fields of new_value.it_interval are zero, the timer expires just once, 
       at the time specified by new_value.it_value.

       The flags argument is either 0, to start a relative timer
       (new_value.it_value specifies a time relative to the current value of
       the clock specified by clockid), 
       
       or TFD_TIMER_ABSTIME, to start an
       absolute timer (new_value.it_value specifies an absolute time for the
       clock specified by clockid; that is, the timer will expire when the
       value of that clock reaches the value specified in
       new_value.it_value).

timerfd_gettime() returns, in curr_value, an itimerspec structure
       that contains the current setting of the timer referred to by the
       file descriptor fd.       

===========================================================================
The epoll API performs a similar task to poll(2): monitoring multiple
       file descriptors to see if I/O is possible on any of them.
       

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event); 
        epoll_ctl - control interface for an epoll descriptor
        
        EPOLL_CTL_ADD
              Register the target file descriptor fd on the epoll instance
              referred to by the file descriptor epfd and associate the
              event event with the internal file linked to fd.
        EPOLL_CTL_DEL
              Remove (deregister) the target file descriptor fd from the
              epoll instance referred to by epfd.  The event is ignored and
              can be NULL (but see BUGS below).
        
        EPOLLIN
              The associated file is available for read(2) operations.
        EPOLLWAKEUP (since Linux 3.5)
              If EPOLLONESHOT and EPOLLET are clear and the process has the
              CAP_BLOCK_SUSPEND capability, ensure that the system does not
              enter "suspend" or "hibernate" while this event is pending or
              being processed.


       
*/



static const clockid_t android_alarm_to_clockid[] = {
    CLOCK_REALTIME_ALARM,
};
#define  N_ANDROID_TIMERFDS   ( sizeof(android_alarm_to_clockid)/sizeof(clockid_t))

/* to match the legacy alarm driver implementation, we need an extra
   CLOCK_REALTIME fd which exists specifically to be canceled on RTC changes */

class AlarmImpl
{
public:
    AlarmImpl(int *fds, size_t n_fds);
    virtual ~AlarmImpl();

    virtual int set(int type, struct timespec *ts) = 0;
    virtual int clear(int type, struct timespec *ts) = 0;
    virtual int setTime(struct timeval *tv) = 0;
    virtual int waitForAlarm() = 0;

protected:
    int *fds;
    size_t n_fds;
};

class AlarmImplAlarmDriver : public AlarmImpl
{
public:
    AlarmImplAlarmDriver(int fd) : AlarmImpl(&fd, 1) { }

    int set(int type, struct timespec *ts);
    int clear(int type, struct timespec *ts);
    int setTime(struct timeval *tv);
    int waitForAlarm();
};

class AlarmImplTimerFd : public AlarmImpl
{
public:
    AlarmImplTimerFd(int fds[N_ANDROID_TIMERFDS], int epollfd, int rtc_id) :
        AlarmImpl(fds, N_ANDROID_TIMERFDS), epollfd(epollfd), rtc_id(rtc_id) { }
    ~AlarmImplTimerFd();

    int set(int type, struct timespec *ts);
    int clear(int type, struct timespec *ts);
    int setTime(struct timeval *tv);
    int waitForAlarm();

private:
    int epollfd;
    int rtc_id;
};

AlarmImpl::AlarmImpl(int *fds_, size_t n_fds) : fds(new int[n_fds]),
        n_fds(n_fds)
{
    memcpy(fds, fds_, n_fds * sizeof(fds[0]));
}

AlarmImpl::~AlarmImpl()
{
    for (size_t i = 0; i < n_fds; i++) {
        close(fds[i]);
    }
    delete [] fds;
}

int AlarmImplAlarmDriver::set(int type, struct timespec *tsin)
{
	struct timespec ts;

	int result = ioctl(fds[0], ANDROID_ALARM_GET_TIME(ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP), &ts);
	if (result < 0) {
		fprintf(stdout, "Unable to get alarm time\n");
		return -2;
	}

	ts.tv_sec += tsin->tv_sec;
    
    return ioctl(fds[0], ANDROID_ALARM_SET(type), &ts);
}



int AlarmImplAlarmDriver::clear(int type, struct timespec *ts)
{
    return ioctl(fds[0], ANDROID_ALARM_CLEAR(type), ts);
}

int AlarmImplAlarmDriver::setTime(struct timeval *tv)
{
    struct timespec ts;
    int res;

    ts.tv_sec = tv->tv_sec;
    ts.tv_nsec = tv->tv_usec * 1000;
    res = ioctl(fds[0], ANDROID_ALARM_SET_RTC, &ts);
    if (res < 0)
        fprintf(stdout,"ANDROID_ALARM_SET_RTC ioctl failed: %s\n", strerror(errno));
    return res;
}

int AlarmImplAlarmDriver::waitForAlarm()
{
    return ioctl(fds[0], ANDROID_ALARM_WAIT);
}

AlarmImplTimerFd::~AlarmImplTimerFd()
{
    for (size_t i = 0; i < N_ANDROID_TIMERFDS; i++) {
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fds[i], NULL);
    }
    close(epollfd);
}

int AlarmImplTimerFd::set(int type, struct timespec *ts)
{
    if (type > ANDROID_ALARM_TYPE_COUNT) {
        errno = EINVAL;
        return -1;
    }

    if (!ts->tv_nsec && !ts->tv_sec) {
        ts->tv_nsec = 1;
    }
    /* timerfd interprets 0 = disarm, so replace with a practically
       equivalent deadline of 1 ns */

    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    memcpy(&spec.it_value, ts, sizeof(spec.it_value));

    // return timerfd_settime(fds[type], TFD_TIMER_ABSTIME, &spec, NULL);
    return timerfd_settime(fds[type], 0, &spec, NULL);
}

int AlarmImplTimerFd::clear(int type, struct timespec *ts)
{
    if (type > ANDROID_ALARM_TYPE_COUNT) {
        errno = EINVAL;
        return -1;
    }

    ts->tv_sec = 0;
    ts->tv_nsec = 0;

    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    memcpy(&spec.it_value, ts, sizeof(spec.it_value));

    return timerfd_settime(fds[type], TFD_TIMER_ABSTIME, &spec, NULL);
}

int AlarmImplTimerFd::setTime(struct timeval *tv)
{
    struct rtc_time rtc;
    struct tm tm, *gmtime_res;
    int fd;
    int res;
    char rtc_dev[20] = {0} ;

    res = settimeofday(tv, NULL);
    if (res < 0) {
        fprintf(stdout,"settimeofday() failed: %s\n", strerror(errno));
        return -1;
    }

    if (rtc_id < 0) {
        fprintf(stdout,"Not setting RTC because wall clock RTC was not found");
        errno = ENODEV;
        return -1;
    }

    sprintf(rtc_dev, "/dev/rtc%d", rtc_id);
    fd = open(rtc_dev, O_RDWR);
    if (fd < 0) {
        fprintf(stdout,"Unable to open %s: %s\n", rtc_dev, strerror(errno));
        return res;
    }

    gmtime_res = gmtime_r(&tv->tv_sec, &tm);
    if (!gmtime_res) {
        fprintf(stdout,"gmtime_r() failed: %s\n", strerror(errno));
        res = -1;
        goto done;
    }

    memset(&rtc, 0, sizeof(rtc));
    rtc.tm_sec = tm.tm_sec;
    rtc.tm_min = tm.tm_min;
    rtc.tm_hour = tm.tm_hour;
    rtc.tm_mday = tm.tm_mday;
    rtc.tm_mon = tm.tm_mon;
    rtc.tm_year = tm.tm_year;
    rtc.tm_wday = tm.tm_wday;
    rtc.tm_yday = tm.tm_yday;
    rtc.tm_isdst = tm.tm_isdst;
    res = ioctl(fd, RTC_SET_TIME, &rtc);
    if (res < 0)
        fprintf(stdout,"RTC_SET_TIME ioctl failed: %s\n", strerror(errno));
done:
    close(fd);
    return res;
}

int AlarmImplTimerFd::waitForAlarm()
{
    epoll_event events[N_ANDROID_TIMERFDS];

    int nevents = epoll_wait(epollfd, events, N_ANDROID_TIMERFDS, -1);
    if (nevents < 0) {
        return nevents;
    }

    int result = 0;
    for (int i = 0; i < nevents; i++) {
        uint32_t alarm_idx = events[i].data.u32;
        uint64_t unused;
        ssize_t err = read(fds[alarm_idx], &unused, sizeof(unused));
        if (err < 0) {
            if (alarm_idx == ANDROID_ALARM_TYPE_COUNT && errno == ECANCELED) {
                result |= ANDROID_ALARM_TIME_CHANGE_MASK;
            } else {
                return err;
            }
        } else {
            result |= (1 << alarm_idx);
        }
    }

    return result;
}

static AlarmImpl * init_alarm_driver()
{
    int fd = open("/dev/alarm", O_RDWR);
    if (fd < 0) {
        fprintf(stdout,"opening alarm driver failed: %s", strerror(errno));
        return 0;
    }

    AlarmImpl *ret = new AlarmImplAlarmDriver(fd);
    return ret;
}


static const char rtc_sysfs[] = "/sys/class/rtc";

static bool rtc_is_hctosys(unsigned int rtc_id)
{
    char hctosys_path[50] = {0};
    sprintf(hctosys_path, "%s/rtc%u/hctosys",rtc_sysfs, rtc_id);

    FILE *file = fopen(hctosys_path, "re");
    if (!file) {
        fprintf(stdout,"failed to open %s: %s", hctosys_path, strerror(errno));
        return false;
    }

    unsigned int hctosys;
    bool ret = false;
    int err = fscanf(file, "%u", &hctosys);
    if (err == EOF)
        fprintf(stdout,"failed to read from %s: %s", hctosys_path,
                strerror(errno));
    else if (err == 0)
        fprintf(stdout,"%s did not have expected contents", hctosys_path);
    else
        ret = hctosys;

    fclose(file);
    return ret;
}

static int wall_clock_rtc()
{
    DIR *dir = opendir(rtc_sysfs);
    if (!dir) {
        fprintf(stdout,"failed to open %s: %s", rtc_sysfs, strerror(errno));
        return -1;
    }

    struct dirent *dirent;
    while (errno = 0, dirent = readdir(dir)) {
        unsigned int rtc_id;
        int matched = sscanf(dirent->d_name, "rtc%u \n", &rtc_id);

        if (matched < 0)
            break;
        else if (matched != 1)
            continue;

        if (rtc_is_hctosys(rtc_id)) {
            fprintf(stdout,"found wall clock RTC %u \n", rtc_id);
            return rtc_id;
        }
    }

    if (errno == 0)
        fprintf(stdout,"no wall clock RTC found");
    else
        fprintf(stdout,"failed to enumerate RTCs: %s", strerror(errno));

    return -1;
}


static AlarmImpl * init_timerfd()
{
    int epollfd;
    int fds[N_ANDROID_TIMERFDS];

    epollfd = epoll_create(N_ANDROID_TIMERFDS);
    if (epollfd < 0) {
        fprintf(stdout,"epoll_create(%zu) failed: %s", N_ANDROID_TIMERFDS,
                strerror(errno));
        return 0;
    }

    for (size_t i = 0; i < N_ANDROID_TIMERFDS; i++) {
        fds[i] = timerfd_create(android_alarm_to_clockid[i], 0);
        if (fds[i] < 0) {
            fprintf(stdout,"timerfd_create(%u) failed: %s",  android_alarm_to_clockid[i],
                    strerror(errno));
            close(epollfd);
            for (size_t j = 0; j < i; j++) {
                close(fds[j]);
            }
            return 0;
        }
    }

    AlarmImpl *ret = new AlarmImplTimerFd(fds, epollfd, wall_clock_rtc());

    for (size_t i = 0; i < N_ANDROID_TIMERFDS; i++) {
        epoll_event event;
        event.events = EPOLLIN | EPOLLWAKEUP;
        event.data.u32 = i;

        int err = epoll_ctl(epollfd, EPOLL_CTL_ADD, fds[i], &event);
        if (err < 0) {
            fprintf(stdout,"epoll_ctl(EPOLL_CTL_ADD) failed: %s", strerror(errno));
            delete ret;
            return 0;
        }
    }

    // struct itimerspec spec;
    // memset(&spec, 0, sizeof(spec));
    
    /* 0 = disarmed; the timerfd doesn't need to be armed to get
       RTC change notifications, just set up as cancelable */

       
    // int err = timerfd_settime(fds[ANDROID_ALARM_TYPE_COUNT],
    //         TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &spec, NULL);
    // if (err < 0) {
    //     fprintf(stdout, "timerfd_settime() failed: %s", strerror(errno));
    //     delete ret;
    //     return 0;
    // }

    return ret ;
}

static AlarmImpl * AlarmManagerService_init()
{
    // AlarmImpl * ret = init_alarm_driver();
    // if (ret) {
    //     return ret;
    // }

    return init_timerfd();
}


// static int alarm_set(int fd, unsigned long elapsedSec) {
// 	int result = 0;
// 	struct timespec ts;
// 
// 	result = ioctl(fd, ANDROID_ALARM_GET_TIME(ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP), &ts);
// 	if (result < 0) {
// 		fprintf(stdout, "Unable to get alarm time\n");
// 		return -2;
// 	}
// 
// 	ts.tv_sec += elapsedSec;
// 
// 	result = ioctl(fd, ANDROID_ALARM_SET(ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP), &ts);
// 	if (result < 0) {
// 		fprintf(stdout, "Unable to set alarm to %lu\n", ts.tv_sec);
// 		return -3;
// 	}
// 
// 	printf("set alarm to %lu\n", ts.tv_sec);
// 	return 0;
// }
// 
// static int alarm_wait(int fd) {
// 	int result = 0;
// 
// 	do {
// 		result = ioctl(fd, ANDROID_ALARM_WAIT);
// 	} while (result < 0 && errno == EINTR);
// 
// 	if (result < 0) {
// 		fprintf(stdout, "Unable to wait on alarm\n");
// 		return -4;
// 	}
// 
// 	return 0;
// }

int main(int argc, char** argv) 
{
	int fd;
    struct timespec ts = {0};
	unsigned long elapsedSec = (unsigned long) atoi(argv[1]);
	char *suspend_cmd = (char*)"echo mem > /sys/power/state";
	int result;
    

	system("echo set_alarm_timer > /sys/power/wake_lock");

	if (argc == 3) 
    {
		suspend_cmd = argv[2];
        printf("argc[2] = %s \n", suspend_cmd);
	}

    
	// fd = open("/dev/alarm", O_RDWR);
	// if (fd < 0) {
	// 	fprintf(stdout, "error open /dev/alarm\n");
	// 	system("echo set_alarm_timer > /sys/power/wake_unlock");
	// 	exit(-1);
	// }
    // 
	// result = alarm_set(fd, elapsedSec);
	// if (result < 0) {
	// 	close(fd);
	// 	fprintf(stdout, "error set alarm\n");
	// 	system("echo set_alarm_timer > /sys/power/wake_unlock");
	// 	exit(result);
	// }
    
    AlarmImpl * palarm =  AlarmManagerService_init() ; 
    system("sync");

    ts.tv_sec = elapsedSec;
    palarm->set( 0, &ts) ;

    if ( strlen(suspend_cmd) != 0 )
        system(suspend_cmd);

	// result = alarm_wait(fd);
    result = palarm->waitForAlarm();
    palarm->clear(0, &ts);
	if (result < 0) 
    {
		delete palarm ;
		fprintf(stdout, "error wait alarm\n");
		exit(result);
	}

	delete palarm ;
    printf("*** set alarm was done ***\n");
    system("sync");
    
    system("echo set_alarm_timer > /sys/power/wake_unlock");
    
    return 0;
}


