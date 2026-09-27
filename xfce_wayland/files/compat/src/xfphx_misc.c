/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * xfce-wayland compat: small libphoenix gaps.
 */

#include <fcntl.h>
#include <unistd.h>


/* BSD/glibc daemon(): continue in a child that has left the caller's session.
 * The parent exits at once with status 0 (the caller of daemon() never returns
 * in the parent), the child becomes a session leader, optionally moves to "/"
 * and points stdin/stdout/stderr at /dev/null. */
int daemon(int nochdir, int noclose)
{
	pid_t pid = fork();

	if (pid < 0) {
		return -1;
	}
	if (pid > 0) {
		_exit(0);
	}
	if (setsid() < 0) {
		return -1;
	}
	if ((nochdir == 0) && (chdir("/") < 0)) {
		return -1;
	}
	if (noclose == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd < 0) {
			return -1;
		}
		(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fd, STDOUT_FILENO);
		(void)dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO) {
			(void)close(fd);
		}
	}
	return 0;
}
