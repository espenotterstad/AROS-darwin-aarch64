struct itimerval;

/* The host's struct timespec: two longs on every hosted ABI */
struct host_timespec
{
    long tv_sec;
    long tv_nsec;
};

struct PlatformTimer
{
    APTR	   hostlibBase;
    APTR	   libcHandle;
    int  	   (*setitimer)(int which, const struct itimerval *value, struct itimerval *ovalue);
    int		   (*clock_gettime)(int clock_id, struct host_timespec *tp); /* NULL: count a fixed period per tick */
    struct timeval tb_HostStart;	/* Host clock at Timer_Init			*/
    struct timeval tb_HostLast;		/* Host clock at the last EClock update		*/
    struct timeval tb_VBlankTime;	/* Software-emulated periodic timer interval	*/
    unsigned int   tb_VBlankTicks;	/* Divisor reload value for VBlank		*/
    unsigned int   tb_TimerCount;	/* VBlank tick counter				*/
};

#define HostLibBase TimerBase->tb_Platform.hostlibBase

struct TimerBase;
void EClockInit(struct TimerBase *TimerBase);
