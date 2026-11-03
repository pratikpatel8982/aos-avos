/*
 * test_mixerdev_oss.c -- unit tests for Source/mixerdev_oss.c (OSS /dev/mixer volume control).
 *
 * White box: the real source is compiled into the test so the file-local descriptor `mixer_fd`
 * can be pointed at a real kernel object. No OSS mixer exists on a CI host, so the successful
 * ioctl (MIXER_WRITE) cannot be reached; everything around it is exercised with real descriptors:
 * the failing open of the two device nodes, the "already open" guard, closing, and the error path of
 * the volume ioctl (a regular file and a pipe both answer ENOTTY, exactly what a non-mixer fd does).
 */
#include "avos_test.h"
#include "Source/mixerdev_oss.c"

#include <fcntl.h>
#include <stdio.h>

static int fd_is_open(int fd)
{
	return fcntl(fd, F_GETFD) != -1;
}

/* the host must not have an OSS mixer, otherwise the failing-open tests below would be meaningless */
static void require_no_oss_mixer(void)
{
	REQUIRE(access("/dev/mixer", F_OK) != 0);
	REQUIRE(access("/dev/snd/mixer", F_OK) != 0);
}

TEST(open_fails_and_returns_null_when_neither_mixer_node_exists)
{
	require_no_oss_mixer();
	mixer_fd = -1;
	CHECK_NULL(mixer_oss_open(0));
	CHECK_EQ_INT(mixer_fd, -1);			/* no stale descriptor is left behind */
}

TEST(open_refuses_a_second_open_while_a_descriptor_is_held)
{
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	mixer_fd = fds[0];
	CHECK_NULL(mixer_oss_open(0));
	CHECK_EQ_INT(mixer_fd, fds[0]);			/* untouched */
	CHECK(fd_is_open(fds[0]));
	close(fds[0]);
	close(fds[1]);
}

TEST(debug_logging_of_open_close_and_volume_does_not_change_behaviour)
{
	require_no_oss_mixer();
	mixer_fd = -1;
	Debug[DBG_AUDIODEVICE] = 2;
	CHECK_NULL(mixer_oss_open(0));
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	mixer_fd = fds[0];
	void *h = &mixer_fd;
	CHECK_EQ_INT(mixer_oss_set_volume(h, 10, 20), 1);
	CHECK_EQ_INT(mixer_oss_close(&h), 0);
	Debug[DBG_AUDIODEVICE] = 0;
	close(fds[1]);
}

TEST(close_without_a_handle_reports_failure)
{
	CHECK_EQ_INT(mixer_oss_close(NULL), 1);
}

TEST(close_releases_the_descriptor_and_clears_the_handle)
{
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	mixer_fd = fds[0];
	void *h = &mixer_fd;

	CHECK_EQ_INT(mixer_oss_close(&h), 0);

	CHECK_NULL(h);
	CHECK_EQ_INT(mixer_fd, -1);
	CHECK(!fd_is_open(fds[0]));			/* really closed in the kernel */
	CHECK(fd_is_open(fds[1]));			/* the other end is not touched */
	close(fds[1]);
}

TEST(close_of_an_already_closed_mixer_is_harmless)
{
	mixer_fd = -1;
	void *h = &mixer_fd;
	CHECK_EQ_INT(mixer_oss_close(&h), 0);
	CHECK_NULL(h);
	CHECK_EQ_INT(mixer_fd, -1);
}

TEST(a_closed_mixer_can_be_reopened_after_close)
{
	require_no_oss_mixer();
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	mixer_fd = fds[0];
	void *h = &mixer_fd;
	CHECK_EQ_INT(mixer_oss_close(&h), 0);
	/* the "already open" guard is gone: open now tries the device nodes (and fails on this host) */
	CHECK_NULL(mixer_oss_open(0));
	CHECK_EQ_INT(mixer_fd, -1);
	close(fds[1]);
}

TEST(set_volume_reports_failure_when_the_descriptor_is_not_a_mixer)
{
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	CHECK_EQ_INT(mixer_oss_set_volume(&fds[0], 50, 50), 1);

	char path[] = "/tmp/avos_mixer_XXXXXX";
	int file = mkstemp(path);
	REQUIRE(file >= 0);
	CHECK_EQ_INT(mixer_oss_set_volume(&file, 0, 100), 1);
	CHECK_EQ_INT(errno, ENOTTY);
	close(file);
	unlink(path);
	close(fds[0]);
	close(fds[1]);
}

TEST(set_volume_reports_failure_on_a_closed_descriptor)
{
	int fd = -1;
	CHECK_EQ_INT(mixer_oss_set_volume(&fd, 50, 50), 1);
	CHECK_EQ_INT(errno, EBADF);
}

/*
 * UNREACHABLE on host: the successful half of mixer_oss_open() (a /dev/mixer or /dev/snd/mixer node
 * that opens, then fcntl + return &mixer_fd), the `return 0` of mixer_oss_set_volume() and the
 * second `open` fallback succeeding: they need a real OSS mixer device, and creating one would mean
 * faking the driver. They are verified on devices by the device/ harnesses.
 */
