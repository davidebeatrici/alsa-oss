/*
 *  OSS -> ALSA compatibility layer
 *  Copyright (c) by Abramo Bagnara <abramo@alsa-project.org>,
 *		     Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>

#include "alsa-oss-emul.h"

#ifndef ATTRIBUTE_UNUSED
/** do not print warning (gcc) when function parameter is not used */
#define ATTRIBUTE_UNUSED __attribute__ ((__unused__))
#endif

#if 1
#define DEBUG_POLL
#define DEBUG_SELECT
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...) do { if (oss_wrapper_debug) fprintf(stderr, __VA_ARGS__); } while (0)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...) do { if (oss_wrapper_debug) fprintf(stderr, ##args); } while (0)
#endif
#else
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...)
#endif
#endif

int (*_select)(int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int (*_poll)(struct pollfd *ufds, unsigned int nfds, int timeout);
int (*_open)(const char *file, int oflag, ...);
int (*_close)(int fd);
ssize_t (*_write)(int fd, const void *buf, size_t n);
ssize_t (*_read)(int fd, void *buf, size_t n);
int (*_ioctl)(int fd, unsigned long request, ...);
int (*_fcntl)(int fd, int cmd, ...);
void *(*_mmap)(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int (*_munmap)(void* addr, size_t len);

FILE *(*_fopen)(const char *path, const char *mode);

typedef struct ops {
	int (*close)(int fd);
	ssize_t (*write)(int fd, const void *buf, size_t n);
	ssize_t (*read)(int fd, void *buf, size_t n);
	int (*ioctl)(int fd, unsigned long request, ...);
	int (*fcntl)(int fd, int cmd, ...);
	void *(*mmap)(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
	int (*munmap)(void* addr, size_t len);
} ops_t;

typedef enum {
	FD_OSS_DSP,
	FD_OSS_MIXER,
	FD_CLASSES,
} fd_class_t;                                                            

static ops_t ops[FD_CLASSES];

typedef struct {
	fd_class_t class;
	int oflags;
	void *mmap_area;
} fd_t;

static void initialize(void);
static int initialized = 0;

static int oss_wrapper_debug = 0;
static int open_max;
static int poll_fds_add = 0;
static fd_t **fds;

static int is_dsp_device(const char *pathname)
{
	if(!pathname) return 0;
	if(strncmp(pathname,"/dev/dsp",8) == 0) return 1;
	if(strncmp(pathname,"/dev/adsp",9) == 0) return 1;
	if(strncmp(pathname,"/dev/audio",10) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/dsp",14) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/adsp",15) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/audio",16) == 0) return 1;
	return 0;
}

static int is_mixer_device(const char *pathname)
{
	if(!pathname) return 0;
	if(strncmp(pathname,"/dev/mixer",10) == 0) return 1;
	if(strncmp(pathname,"/dev/sound/mixer",16) == 0) return 1;
	return 0;
}

static int oss_pcm_fcntl(int fd, int cmd, ...)
{
	int result;
	va_list args;
	long arg;

	if (!initialized)
		initialize();

	va_start(args, cmd);
	arg = va_arg(args, long);
	va_end(args);

	switch (cmd) {
	case F_GETFL:
		return fds[fd]->oflags;
        case F_SETFL:
		result = lib_oss_pcm_nonblock(fd, (arg & O_NONBLOCK) ? 1 : 0);
                if (result < 0) {
                        errno = -result;
                        return -1;
                }
                return 0;
	default:
		DEBUG("pcm_fcntl(%d, ", fd);
		result = _fcntl(fd, cmd, arg);
		if (result < 0)
			return result;
		DEBUG("%x, %ld)\n", cmd, arg);
		return result;
	}
	return -1;
}

static int oss_mixer_fcntl(int fd, int cmd, ...)
{
	int result;
	va_list args;
	long arg;

	va_start(args, cmd);
	arg = va_arg(args, long);
	va_end(args);

	switch (cmd) {
	case F_GETFL:
		return fds[fd]->oflags;
	default:
		DEBUG("mixer_fcntl(%d, ", fd);
		result = _fcntl(fd, cmd, arg);
		if (result < 0)
			return result;
		DEBUG("%x, %ld)\n", cmd, arg);
		return result;
	}
	return -1;
}

static ssize_t bad_write(int fd ATTRIBUTE_UNUSED, const void *buf ATTRIBUTE_UNUSED, size_t n ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return -1;
}

static ssize_t bad_read(int fd ATTRIBUTE_UNUSED, void *buf ATTRIBUTE_UNUSED, size_t n ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return -1;
}

static void *bad_mmap(void *addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED,
		      int prot ATTRIBUTE_UNUSED, int flags ATTRIBUTE_UNUSED,
		      int fd ATTRIBUTE_UNUSED, off_t offset ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return MAP_FAILED;
}

static int bad_munmap(void* addr ATTRIBUTE_UNUSED, size_t len ATTRIBUTE_UNUSED)
{
	errno = EBADFD;
	return -1;
}

static ops_t ops[FD_CLASSES] = {
        [FD_OSS_DSP] = {
		close: lib_oss_pcm_close,
		write: lib_oss_pcm_write,
		read: lib_oss_pcm_read,
		ioctl: lib_oss_pcm_ioctl,
		fcntl: oss_pcm_fcntl,
		mmap: lib_oss_pcm_mmap,
		munmap: lib_oss_pcm_munmap,
        },
        [FD_OSS_MIXER] = {
		close: lib_oss_mixer_close,
		write: bad_write,
		read: bad_read,
		ioctl: lib_oss_mixer_ioctl,
		fcntl: oss_mixer_fcntl,
		mmap: bad_mmap,
		munmap: bad_munmap,
	},
};

int open(const char *file, int oflag, ...)
{
	va_list args;
	mode_t mode = 0;
	int fd;

	if (!initialized)
		initialize();

	if (oflag & O_CREAT) {
		va_start(args, oflag);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	if (is_dsp_device(file)) {
		fd = lib_oss_pcm_open(file, oflag);
		if (fd >= 0) {
			fds[fd] = calloc(sizeof(fd_t), 1);
			if (fds[fd] == NULL) {
				ops[FD_OSS_DSP].close(fd);
				errno = ENOMEM;
				return -1;
			}
			fds[fd]->class = FD_OSS_DSP;
			fds[fd]->oflags = oflag;
			poll_fds_add += lib_oss_pcm_poll_fds(fd);
		}
	} else if (is_mixer_device(file)) {
		fd = lib_oss_mixer_open(file, oflag);
		if (fd >= 0) {
			fds[fd] = calloc(sizeof(fd_t), 1);
			if (fds[fd] == NULL) {
				ops[FD_OSS_MIXER].close(fd);
				errno = ENOMEM;
				return -1;
			}
			fds[fd]->class = FD_OSS_MIXER;
			fds[fd]->oflags = oflag;
		}
	} else {
		fd = _open(file, oflag, mode);
		if (fd >= 0)
			assert(fds[fd] == NULL);
	}
	return fd;
}

int close(int fd)
{
	if (!initialized)
		initialize();

	if (fd < 0 || fd >= open_max || fds[fd] == NULL) {
		return _close(fd);
	} else {
		fd_t *xfd = fds[fd];
		int err;

		fds[fd] = NULL;
		poll_fds_add -= lib_oss_pcm_poll_fds(fd);
		err = ops[xfd->class].close(fd);
		assert(err >= 0);
		return err;
	}
}

ssize_t write(int fd, const void *buf, size_t n)
{
	if (!initialized)
		initialize();

	if (fd < 0 || fd >= open_max || fds[fd] == NULL)
		return _write(fd, buf, n);
	else
		return ops[fds[fd]->class].write(fd, buf, n);
}

ssize_t read(int fd, void *buf, size_t n)
{
	if (!initialized)
		initialize();

	if (fd < 0 || fd >= open_max || fds[fd] == NULL)
		return _read(fd, buf, n);
	else
		return ops[fds[fd]->class].read(fd, buf, n);
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;

	if (!initialized)
		initialize();

	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);
	if (fd < 0 || fd >= open_max || fds[fd] == NULL)
		return _ioctl(fd, request, arg);
	else
		return ops[fds[fd]->class].ioctl(fd, request, arg);
}

int fcntl(int fd, int cmd, ...)
{
	va_list args;
	void *arg;

	va_start(args, cmd);
	arg = va_arg(args, void *);
	va_end(args);
	if (fd < 0 || fd >= open_max || fds[fd] == NULL)
		return _fcntl(fd, cmd, arg);
	else
		return ops[fds[fd]->class].fcntl(fd, cmd, arg);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	void *result;

	if (!initialized)
		initialize();

	if (fd < 0 || fd >= open_max || fds[fd] == NULL)
		return _mmap(addr, len, prot, flags, fd, offset);
	result = ops[fds[fd]->class].mmap(addr, len, prot, flags, fd, offset);
	if (result != NULL && result != MAP_FAILED)
		fds[fd]->mmap_area = result;
	return result;
}

int munmap(void *addr, size_t len)
{
	int fd;

	if (!initialized)
		initialize();

	for (fd = 0; fd < open_max; ++fd) {
		if (fds[fd] && fds[fd]->mmap_area == addr)
			break;
	}
	if (fd >= open_max)
		return _munmap(addr, len);
	fds[fd]->mmap_area = 0;
	return ops[fds[fd]->class].munmap(addr, len);
}

#ifdef DEBUG_POLL
void dump_poll(struct pollfd *pfds, unsigned long nfds, int timeout)
{
	unsigned int k;
	fprintf(stderr, "POLL nfds: %ld, timeout: %d\n", nfds, timeout);
	for (k = 0; k < nfds; ++k) {
		fprintf(stderr, "fd=%d, events=%x, revents=%x\n", 
			pfds[k].fd, pfds[k].events, pfds[k].revents);
	}
}
#endif

#ifdef DEBUG_SELECT
void dump_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
		 struct timeval *timeout)
{
	int k;
	fprintf(stderr, "SELECT nfds: %d, ", nfds);
	if (timeout)
		fprintf(stderr, "timeout: %ld.%06ld\n", timeout->tv_sec, timeout->tv_usec);
	else
		fprintf(stderr, "no timeout\n");
	if (rfds) {
		fprintf(stderr, "rfds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, rfds))
				putc('1', stderr);
			else
				putc('0', stderr);
		}
		putc('\n', stderr);
	}
	if (wfds) {
		fprintf(stderr, "wfds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, wfds))
				putc('1', stderr);
			else
				putc('0', stderr);
		}
		putc('\n', stderr);
	}
	if (efds) {
		fprintf(stderr, "efds: ");
		for (k = 0; k < nfds; ++k) {
			if (FD_ISSET(k, efds))
				putc('1', stderr);
			else
				putc('0', stderr);
		}
		putc('\n', stderr);
	}
}
#endif

int poll(struct pollfd *pfds, unsigned long nfds, int timeout)
{
	unsigned int k;
	unsigned int nfds1;
	int count, count1;
	int direct = 1;
	struct pollfd pfds1[nfds + poll_fds_add + 16];

	if (!initialized)
		initialize();

	nfds1 = 0;
	for (k = 0; k < nfds; ++k) {
		int fd = pfds[k].fd;
		pfds[k].revents = 0;
		if (fd >= open_max || !fds[fd])
			goto _std1;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			unsigned short events = pfds[k].events;
			int fmode = 0;
			if ((events & (POLLIN|POLLOUT)) == (POLLIN|POLLOUT))
				fmode = O_RDWR;
			else if (events & POLLIN)
				fmode = O_RDONLY;
			else
				fmode = O_WRONLY;
			nfds1 += lib_oss_pcm_poll_prepare(fd, fmode, &pfds1[nfds1]);
			direct = 0;
			break;
		}
		default:
		_std1:
			pfds1[nfds1].fd = pfds[k].fd;
			pfds1[nfds1].events = pfds[k].events;
			pfds1[nfds1].revents = 0;
			nfds1++;
			break;
		}
	}
	if (direct)
		return _poll(pfds, nfds, timeout);
#ifdef DEBUG_POLL
	if (oss_wrapper_debug) {
		fprintf(stderr, "Orig enter ");
		dump_poll(pfds, nfds, timeout);
		fprintf(stderr, "Changed enter ");
		dump_poll(pfds1, nfds1, timeout);
	}
#endif
	count = _poll(pfds1, nfds1, timeout);
	if (count <= 0)
		return count;
	nfds1 = 0;
	count1 = 0;
	for (k = 0; k < nfds; ++k) {
		int fd = pfds[k].fd;
		unsigned int revents;
		if (fd >= open_max || !fds[fd])
			goto _std2;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			int result = lib_oss_pcm_poll_result(fd, &pfds1[nfds1]);
			revents = 0;
			if (result < 0) {
				revents |= POLLNVAL;
			} else {
				revents |= ((result & OSS_WAIT_EVENT_ERROR) ? POLLERR : 0) |
					   ((result & OSS_WAIT_EVENT_READ) ? POLLIN : 0) |
					   ((result & OSS_WAIT_EVENT_WRITE) ? POLLOUT : 0);
			}
			nfds1 += lib_oss_pcm_poll_fds(fd);
			break;
		}
		default:
		_std2:
			revents = pfds1[nfds1].revents;
			nfds1++;
			break;
		}
		pfds[k].revents = revents;
		if (revents)
			count1++;
	}
#ifdef DEBUG_POLL
	if (oss_wrapper_debug) {
		fprintf(stderr, "Changed exit ");
		dump_poll(pfds1, nfds1, timeout);
		fprintf(stderr, "Orig exit ");
		dump_poll(pfds, nfds, timeout);
	}
#endif
	return count1;
}

int select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
	   struct timeval *timeout)
{
	fd_set _rfds1, _wfds1, _efds1;
	fd_set *rfds1, *wfds1, *efds1;
	int nfds1 = nfds;
	int count, count1;
	int fd;
	int direct = 1;

	if (!initialized)
		initialize();

	if (rfds) {
		_rfds1 = *rfds;
	} else {
		FD_ZERO(&_rfds1);
	}
	rfds1 = &_rfds1;
	if (wfds) {
		_wfds1 = *wfds;
	} else {
		FD_ZERO(&_wfds1);
	}
	wfds1 = &_wfds1;
	if (efds) {
		_efds1 = *efds;
		efds1 = &_efds1;
	} else {
		efds1 = NULL;
	}
	for (fd = 0; fd < nfds; ++fd) {
		int r = (rfds && FD_ISSET(fd, rfds));
		int w = (wfds && FD_ISSET(fd, wfds));
		int e = (efds && FD_ISSET(fd, efds));
		if (!(r || w || e))
			continue;
		if (!fds[fd])
			continue;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			int res, fmode = 0;
			
			if (r & w)
				fmode = O_RDWR;
			else if (r)
				fmode = O_RDONLY;
			else
				fmode = O_WRONLY;
			res = lib_oss_pcm_select_prepare(fd, fmode, rfds1, wfds1,
							 e ? efds1 : NULL);
			if (res < 0)
				return -1;
			if (nfds1 < res + 1)
				nfds1 = res + 1;
			if (r)
				FD_CLR(fd, rfds1);
			if (w)
				FD_CLR(fd, wfds1);
			if (e)
				FD_CLR(fd, efds1);
			direct = 0;
			break;
		}
		default:
			break;
		}
	}
	if (direct)
		return _select(nfds, rfds, wfds, efds, timeout);
#ifdef DEBUG_SELECT
	if (oss_wrapper_debug) {
		fprintf(stderr, "Orig enter ");
		dump_select(nfds, rfds, wfds, efds, timeout);
		fprintf(stderr, "Changed enter ");
		dump_select(nfds1, rfds1, wfds1, efds1, timeout);
	}
#endif
	count = _select(nfds1, rfds1, wfds1, efds1, timeout);
	if (count < 0)
		return count;
	if (count == 0) {
		if (rfds)
			FD_ZERO(rfds);
		if (wfds)
			FD_ZERO(wfds);
		if (efds)
			FD_ZERO(efds);
		return 0;
	}
	count1 = 0;
	for (fd = 0; fd < nfds; ++fd) {
		int r = (rfds && FD_ISSET(fd, rfds));
		int w = (wfds && FD_ISSET(fd, wfds));
		int e = (efds && FD_ISSET(fd, efds));
		int r1, w1, e1;
		if (!(r || w || e))
			continue;
		if (!fds[fd])
			continue;
		switch (fds[fd]->class) {
		case FD_OSS_DSP:
		{
			int result = lib_oss_pcm_select_result(fd, rfds1, wfds1, efds1);
			r1 = w1 = e1 = 0;
			if (result < 0 && e) {
				FD_SET(fd, efds);
				e1 = 1;
			} else {
				if (result & OSS_WAIT_EVENT_ERROR) {
					FD_SET(fd, efds);
					e1 = 1;
				}
				if (result & OSS_WAIT_EVENT_READ) {
					FD_SET(fd, rfds);
					r1 = 1;
				}
				if (result & OSS_WAIT_EVENT_WRITE) {
					FD_SET(fd, wfds);
					w1 = 1;
				}
			}
			break;
		}
		default:
			r1 = (r && FD_ISSET(fd, rfds1));
			w1 = (w && FD_ISSET(fd, wfds1));
			e1 = (e && FD_ISSET(fd, efds1));
			break;
		}
		if (r && !r1)
			FD_CLR(fd, rfds);
		if (w && !w1)
			FD_CLR(fd, wfds);
		if (e && !e1)
			FD_CLR(fd, efds);
		if (r1 || w1 || e1)
			count1++;
	}
#ifdef DEBUG_SELECT
	if (oss_wrapper_debug) {
		fprintf(stderr, "Changed exit ");
		dump_select(nfds1, rfds1, wfds1, efds1, timeout);
		fprintf(stderr, "Orig exit ");
		dump_select(nfds, rfds, wfds, efds, timeout);
	}
#endif
	return count1;
}


#include "stdioemu.c"

FILE *fopen(const char* path, const char *mode) {

	if (!initialized)
		initialize();

	if(!is_dsp_device(path)) 
		return _fopen (path, mode);
	
	return fake_fopen(path, mode);
}

#if 1
# define strong_alias(name, aliasname) \
  extern __typeof (name) aliasname __attribute__ ((alias (#name)));
strong_alias(open, __open);
strong_alias(close, __close);
strong_alias(write, __write);
strong_alias(read, __read);
strong_alias(ioctl, __ioctl);
strong_alias(fcntl, __fcntl);
strong_alias(mmap, __mmap);
strong_alias(munmap, __munmap);
strong_alias(poll, __poll);
strong_alias(select, __select);
strong_alias(fopen, __fopen);
#else
int dup(int fd)
{
	return fcntl(fd, F_DUPFD, 0);
}

int dup2(int fd, int fd2)
{
	int save;

	if (fd2 < 0 || fd2 >= open_max) {
		errno = EBADF;
		return -1;
	}
	
	if (fcntl(fd, F_GETFL) < 0)
		return -1;
	
	if (fd == fd2)
		return fd2;
	
	save = errno;
	close(fd2);
	errno = save;
	
	return fcntl(fd, F_DUPFD, fd2);
}

#ifndef O_LARGEFILE
#define O_LARGEFILE 0100000
#endif

int open64(const char *file, int oflag, ...)
{
	va_list args;
	mode_t mode = 0;

	if (oflag & O_CREAT) {
		va_start(args, oflag);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	return open(file, oflag | O_LARGEFILE, mode);
}
#endif

static void initialize() __attribute__ ((constructor));

static void initialize()
{
	char *s = getenv("ALSA_OSS_DEBUG");
	if (s)
		oss_wrapper_debug = 1;
	open_max = sysconf(_SC_OPEN_MAX);
	if (open_max < 0)
		exit(1);
	fds = calloc(open_max, sizeof(*fds));
	if (!fds)
		exit(1);
	_open = dlsym(RTLD_NEXT, "open");
	_close = dlsym(RTLD_NEXT, "close");
	_write = dlsym(RTLD_NEXT, "write");
	_read = dlsym(RTLD_NEXT, "read");
	_ioctl = dlsym(RTLD_NEXT, "ioctl");
	_fcntl = dlsym(RTLD_NEXT, "fcntl");
	_mmap = dlsym(RTLD_NEXT, "mmap");
	_munmap = dlsym(RTLD_NEXT, "munmap");
	_select = dlsym(RTLD_NEXT, "select");
	_poll = dlsym(RTLD_NEXT, "poll");
	_fopen = dlsym(RTLD_NEXT, "fopen");
	initialized = 1;
}
