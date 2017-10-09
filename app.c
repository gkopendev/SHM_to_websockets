#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <semaphore.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <mqueue.h>
#include <string.h>

#include <sys/types.h>
#include <sys/shm.h>
#include <sys/stat.h> 
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <sys/stat.h> 	/* For mode constants */
#include <fcntl.h> 	/* For O_* constants */

/******************************************************************************/
/*
	Create semlocked and lockfree SHM which can be accessed from Python or other 
	C apps (and cleanup)
*/
// func prototypes
void cleanup_channels_shm(void); 
void cleanup_eeprom_shm(void);
void mq_to_py(void);

// shm layout
struct channels_shm {
	float longitude;
	float latitude;
	float altitude;
	char spare[1024-12];
};

struct eeprom_shm {
	int eep_test;
	char spare[4096-4];
};

/* Use this in the app to access data */ 
struct channels_shm *p_channels_shm = MAP_FAILED ;

/* Use this in the app to access data, but only after acquiring semaphore */ 
struct eeprom_shm *p_eeprom_shm = MAP_FAILED ;
sem_t *p_eeprom_sem = SEM_FAILED;

/* example of SHM shared with python without lock */
int32_t init_connect_channels_shm(void)
{
	int32_t  fd; 
	
	shm_unlink("/channels_shm");
	/* Some app has to O_CREAT it */
	fd = shm_open("/channels_shm", O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
	ftruncate(fd, 1024);
	p_channels_shm = mmap(NULL, 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p_channels_shm ==  MAP_FAILED ) {
		printf("mmap channels fail\n");
		cleanup_channels_shm();
		return -1;
	}
	else 
		printf("mmap channels done \n");
	return 0;              
}

void cleanup_channels_shm(void)
{
	if (p_channels_shm != MAP_FAILED)
		munmap(p_channels_shm, 1024);
	shm_unlink("/channels_shm");
}

/* example of SHM shared with python with lock */
int32_t init_connect_eeprom_shm(void)
{
	int32_t  fd;
	
	shm_unlink("/eeprom_shm");
	/* Some app has to O_CREAT it */
	fd = shm_open("/eeprom_shm", O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
	ftruncate(fd, 4096);
	p_eeprom_shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p_eeprom_shm ==  MAP_FAILED ) {
		printf("mmap eeprom fail\n");
		cleanup_eeprom_shm();
		return -1;
	}
	else 
		printf("mmap eep done \n");
	
	sem_unlink("/eeprom_sem");
	p_eeprom_sem = sem_open("/eeprom_sem", O_CREAT, 0600, 0);
	if (p_eeprom_sem == SEM_FAILED)
		printf("sem - %s", strerror(errno));
	else
		sem_post(p_eeprom_sem); // named semaphore starts at zero, so increment value first else first lock will never return
	return 0;              
}

void cleanup_eeprom_shm(void)
{
	if (p_eeprom_shm != MAP_FAILED)
		munmap(p_eeprom_shm, 4096);
	shm_unlink("/eeprom_shm");
	
	if (p_eeprom_sem !=  SEM_FAILED ) {
		sem_close(p_eeprom_sem);
		sem_unlink("/eeprom_sem");
	}
}

void lock_eeprom_shm()
{
	int ret = -1;
	do {ret = sem_wait(p_eeprom_sem);}while(ret != 0);
}

void unlock_eeprom_shm()
{
	int ret = -1;
	do {ret = sem_post(p_eeprom_sem);}while(ret != 0);
}
/******************************************************************************/


/******************************************************************************/
/**********************************EPOLL***************************************/
/*
	Globals for epolling in app, for asynchrous handling we assign file descriptors
	and react when they return in main polling loop. All async events like signals,
	ipc messages , timeouts, peripheral devices files(e.g. commports) can be added
	to epoll handler.
*/
#define MAX_EVENTS 10
static volatile sig_atomic_t exit_app = 0;
static int epoll_fd 	= -1; 
struct epoll_event events[MAX_EVENTS];
/******************************************************************************/

/******************************************************************************/
/***************************Signals to application*****************************/
static int signal_fd 	= -1;
/* Add signalfd file descriptor to epoll, simplifies signal handling */
int add_signal_fd_epoll(void)
{
	struct epoll_event event;
	sigset_t mask;
	
	sigfillset(&mask);

	// Block signals so you receive them in signalfd
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		printf("Failed to set signal mask");
		return 0;
	}

	signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (signal_fd < 0) {
		printf("Failed to create signal descriptor");
		return 0;
	}
	
	event.events = EPOLLIN | EPOLLPRI;
	event.data.fd = signal_fd;
	
	return epoll_ctl (epoll_fd, EPOLL_CTL_ADD, signal_fd, &event);
}

void signal_handler(uint32_t revents)
{
	struct signalfd_siginfo si;
	ssize_t result;
	
	if (revents & (EPOLLIN | EPOLLPRI)) {
		result = read(signal_fd, &si, sizeof(si));
		if (result != sizeof(si))
			return;

		switch (si.ssi_signo) {
			case SIGCHLD:
				break;
			case SIGINT:
			case SIGTERM:
			case SIGSEGV:
				printf("Quitting main\n");
				exit_app = 1;
				break;
		}
	}
}
/******************************************************************************/

/******************************************************************************/
/******************Example of timerfd and it's epolling************************/
static int timer_1s_fd;

int add_timer_epoll(void)
{
	int ret; uint8_t tmp[8];
	struct itimerspec timeout;
	struct itimerspec old_timer;
	struct epoll_event event;
	
	timer_1s_fd = timerfd_create(CLOCK_MONOTONIC, (TFD_NONBLOCK | TFD_CLOEXEC));
	if (timer_1s_fd <= 0) {
		printf("Failed to create timer\n");
		return -1;
	}
	
	/* 
	If it_value is set to zero(i.e both tv_sec and tv_nsec set to 0), it will
	disarm the timer rather than meaning expire immediately. So if you set it_value
	to zero, set it_interval to desired timeout and later in code epoll it, it will
	never trigger(expire)! So set it to a token first expiry duration(usually same as 
	it_interval)
	*/
	timeout.it_value.tv_sec = 15;  //make sure SHM's, queue is created before invoking app.py
	timeout.it_value.tv_nsec = 0; 
	timeout.it_interval.tv_sec = 0;
	timeout.it_interval.tv_nsec = 10 * 1000 * 1000;
	ret = timerfd_settime(timer_1s_fd, 0, &timeout, &old_timer);
	if (ret) {
		printf("Failed to set timer duration\n");
		return -1;
	}
	
	event.events = EPOLLPRI | EPOLLIN;
	event.data.fd = timer_1s_fd;
	
	return epoll_ctl (epoll_fd, EPOLL_CTL_ADD, event.data.fd, &event);
}

static void timer_handler(uint32_t revents)
{
	uint8_t tmp[8];
	size_t s = 0;
	struct itimerspec timeout;
	struct itimerspec old_timer;
	int32_t ret;
	
	float x=0.9914, y=51.9895, z=115;
	static int e = 0;
	
	/* read data from timer so it can re-trigger after next expiration it will 
	keep triggering if data isn't read. The 8 bytes(uint64_t) should tell you
	how many times timer expired since last settime()
	*/
	if ((read(timer_1s_fd, tmp, 8)) != -1) {
		// operate on SHM, // pseudo-random GPS location
		p_channels_shm->longitude= x + (rand() % 5)/1000.0 ;
		p_channels_shm->latitude = y + (rand() % 5)/1000.0 ;
		p_channels_shm->altitude = z + (rand() % 5)/1000.0 ;
		
		lock_eeprom_shm();
		p_eeprom_shm->eep_test = e;
		unlock_eeprom_shm();
		
		printf(" %d - %f %f %f \n", e, p_channels_shm->longitude, 
					p_channels_shm->latitude, 
					p_channels_shm->altitude);
							
		e++;
		mq_to_py();
	}
}

/********************************IPC to python ********************************/
int mq_fd;
int connect_py_q(void)
{
	struct mq_attr attr;
	attr.mq_maxmsg = 10;
	attr.mq_msgsize = 64;
	mq_unlink("/PY_MQ");
	mq_fd = mq_open("/PY_MQ", O_CREAT | O_WRONLY | O_CLOEXEC , 0664, &attr);
	return mq_fd;
}

void mq_to_py(void)
{
	uint32_t signal = 1;
	if (mq_fd > 0)
	mq_send(mq_fd, (char *)&signal, 4, 0);
}
/******************************************************************************/

void main()
{
	int res, poll_forever = -1, i;
	
	// create and connect to locked and lockfree SHM's
	init_connect_channels_shm();
	init_connect_eeprom_shm();
	
	// Create epoll watcher, but make sure children dont steal them 
	if (-1 == (epoll_fd = epoll_create1(EPOLL_CLOEXEC)))
		printf("Epoll errno %m", errno);
	
	if (add_signal_fd_epoll() < 0)
		printf("Signal fd error \n");
	else
		printf("sigfd epoll \n");
	
	if (add_timer_epoll() < 0)
		printf("timer fd error");
	else
		printf("timerfd epoll \n");
	
	if (connect_py_q() < 0)
		printf("cannot connect mq");

	srand(100); // for random generator in timer handler

	
	while(!exit_app) {
		res = epoll_wait(epoll_fd, events, MAX_EVENTS, poll_forever);
		if (res == -1) {
			printf("Poll wait failed, quitting \n");
			exit_app = 1;
			break;
		}
		for (i=0; i<res; i++) {
			if (exit_app)
				break;
			if (events[i].data.fd == signal_fd)
				signal_handler(events[i].events);
			else if (events[i].data.fd == timer_1s_fd) 
				timer_handler(events[i].events);
		}
	}
	
	// cleanup shm's
	cleanup_channels_shm();
	cleanup_eeprom_shm();
	// cleanup queue
	mq_close(mq_fd);
	mq_unlink("/PY_MQ");
}
