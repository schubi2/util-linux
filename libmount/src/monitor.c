/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * This file is part of libmount from util-linux project.
 *
 * Copyright (C) 2014-2018 Karel Zak <kzak@redhat.com>
 *
 * libmount is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

/**
 * SECTION: monitor
 * @title: Monitor
 * @short_description: interface to monitor mount tables
 *
 * For example monitor VFS (/proc/self/mountinfo) for changes:
 *
 * <informalexample>
 *   <programlisting>
 * const char *filename;
 * struct libmount_monitor *mn = mnt_new_monitor();
 *
 * mnt_monitor_enable_mountinfo(mn, TRUE));
 *
 * printf("waiting for changes...\n");
 * while (mnt_monitor_wait(mn, -1) > 0) {
 *    while (mnt_monitor_next_change(mn, &filename, NULL) == 0)
 *       printf(" %s: change detected\n", filename);
 * }
 * mnt_unref_monitor(mn);
 *   </programlisting>
 * </informalexample>
 *
 */

#include "mountP.h"
#include "monitor.h"

#include <sys/epoll.h>

/**
 * mnt_new_monitor:
 *
 * The initial refcount is 1, and needs to be decremented to
 * release the resources of the filesystem.
 *
 * Returns: newly allocated struct libmnt_monitor.
 */
struct libmnt_monitor *mnt_new_monitor(void)
{
	struct libmnt_monitor *mn = calloc(1, sizeof(*mn));
	if (!mn)
		return NULL;

	mn->refcount = 1;
	mn->fd = -1;
	INIT_LIST_HEAD(&mn->ents);

	DBG(MONITOR, ul_debugobj(mn, "alloc"));
	return mn;
}

/**
 * mnt_ref_monitor:
 * @mn: monitor pointer
 *
 * Increments reference counter.
 */
void mnt_ref_monitor(struct libmnt_monitor *mn)
{
	if (mn)
		mn->refcount++;
}

void free_monitor_entry(struct monitor_entry *me)
{
	if (!me)
		return;

	if (me->opers->op_free_data)
		me->opers->op_free_data(me);

	list_del(&me->ents);
	if (me->fd >= 0)
		close(me->fd);
	free(me->path);
	free(me);
}

/**
 * mnt_unref_monitor:
 * @mn: monitor pointer
 *
 * Decrements the reference counter, on zero the @mn is automatically
 * deallocated.
 */
void mnt_unref_monitor(struct libmnt_monitor *mn)
{
	if (!mn)
		return;

	mn->refcount--;
	if (mn->refcount <= 0) {
		mnt_monitor_close_fd(mn);	/* destroys all file descriptors */

		while (!list_empty(&mn->ents)) {
			struct monitor_entry *me = list_entry(mn->ents.next,
						  struct monitor_entry, ents);
			free_monitor_entry(me);
		}

		free(mn);
	}
}

struct monitor_entry *monitor_new_entry(struct libmnt_monitor *mn)
{
	struct monitor_entry *me;

	assert(mn);

	me = calloc(1, sizeof(*me));
	if (!me)
		return NULL;
        INIT_LIST_HEAD(&me->ents);
	list_add_tail(&me->ents, &mn->ents);

	me->fd = -1;
	me->id = -1;

	return me;
}

static int monitor_next_entry(struct libmnt_monitor *mn,
			      struct libmnt_iter *itr,
			      struct monitor_entry **me)
{
	int rc = 1;

	assert(mn);
	assert(itr);

	if (me)
		*me = NULL;

	if (!itr->head)
		MNT_ITER_INIT(itr, &mn->ents);
	if (itr->p != itr->head) {
		if (me)
			*me = MNT_ITER_GET_ENTRY(itr, struct monitor_entry, ents);
		MNT_ITER_ITERATE(itr);
		rc = 0;
	}

	return rc;
}

/* returns entry by type */
struct monitor_entry *monitor_get_entry(struct libmnt_monitor *mn, int type, int id)
{
	struct libmnt_iter itr;
	struct monitor_entry *me;

	mnt_reset_iter(&itr, MNT_ITER_FORWARD);
	while (monitor_next_entry(mn, &itr, &me) == 0) {
		if (me->type == type && me->id == id)
			return me;
	}
	return NULL;
}


/*
 * Add/Remove monitor entry to/from monitor epoll.
 */
int monitor_modify_epoll(struct libmnt_monitor *mn,
				struct monitor_entry *me, int enable)
{
	assert(mn);
	assert(me);

	me->enabled = enable ? 1 : 0;
	me->active = 0;

	if (mn->fd < 0)
		return 0;	/* no epoll, ignore request */

	if (enable) {
		struct epoll_event ev = { .events = me->events };
		int fd;

		assert(me->opers->op_get_fd);
		assert(me->opers->op_process_event);

		fd = me->opers->op_get_fd(mn, me);
		if (fd < 0)
			goto err;

		DBG(MONITOR, ul_debugobj(mn, " add fd=%d (for %s)", fd, me->path));

		ev.data.ptr = (void *) me;

		if (epoll_ctl(mn->fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
			if (errno != EEXIST)
				goto err;
		}
		if (me->events & (EPOLLIN | EPOLLET)) {
			/* Drain initial events generated for /proc/self/mountinfo */
			struct epoll_event events[1];
			while (epoll_wait(mn->fd, events, 1, 0) > 0);
		}
	} else if (me->fd) {
		DBG(MONITOR, ul_debugobj(mn, " remove fd=%d (for %s)", me->fd, me->path));
		if (epoll_ctl(mn->fd, EPOLL_CTL_DEL, me->fd, NULL) < 0) {
			if (errno != ENOENT)
				goto err;
		}
	}

	return 0;
err:
	return -errno;
}

/**
 * mnt_monitor_close_fd:
 * @mn: monitor
 *
 * Close monitor file descriptor. This is usually unnecessary, because
 * mnt_unref_monitor() cleanups all.
 *
 * The function is necessary only if you want to reset monitor setting. The
 * next mnt_monitor_get_fd() or mnt_monitor_wait() will use newly initialized
 * monitor.  This restart is unnecessary for mnt_monitor_enable_*() functions.
 *
 * Returns: 0 on success, <0 on error.
 */
int mnt_monitor_close_fd(struct libmnt_monitor *mn)
{
	struct libmnt_iter itr;
	struct monitor_entry *me;

	if (!mn)
		return -EINVAL;

	mnt_reset_iter(&itr, MNT_ITER_FORWARD);

	/* disable all monitor entries */
	while (monitor_next_entry(mn, &itr, &me) == 0) {

		/* remove entry from epoll */
		if (mn->fd >= 0)
			monitor_modify_epoll(mn, me, FALSE);

		/* close entry FD */
		me->opers->op_close_fd(mn, me);
	}

	if (mn->fd >= 0) {
		DBG(MONITOR, ul_debugobj(mn, "closing top-level monitor fd"));
		close(mn->fd);
	}
	mn->fd = -1;
	return 0;
}

/**
 * mnt_monitor_get_fd:
 * @mn: monitor
 *
 * The file descriptor is associated with all monitored files and it's usable
 * for example for epoll. You have to call mnt_monitor_event_cleanup() or
 * mnt_monitor_next_change() after each event.
 *
 * Returns: >=0 (fd) on success, <0 on error
 */
int mnt_monitor_get_fd(struct libmnt_monitor *mn)
{
	struct libmnt_iter itr;
	struct monitor_entry *me;
	int rc = 0;

	if (!mn)
		return -EINVAL;
	if (mn->fd >= 0)
		return mn->fd;

	DBG(MONITOR, ul_debugobj(mn, "create top-level monitor fd"));
	mn->fd = epoll_create1(EPOLL_CLOEXEC);
	if (mn->fd < 0)
		return -errno;

	mnt_reset_iter(&itr, MNT_ITER_FORWARD);

	DBG(MONITOR, ul_debugobj(mn, "adding monitor entries to epoll (fd=%d)", mn->fd));
	while (monitor_next_entry(mn, &itr, &me) == 0) {
		if (!me->enabled)
			continue;
		rc = monitor_modify_epoll(mn, me, TRUE);
		if (rc)
			goto err;
	}

	DBG(MONITOR, ul_debugobj(mn, "successfully created monitor"));
	return mn->fd;
err:
	rc = errno ? -errno : -EINVAL;
	close(mn->fd);
	mn->fd = -1;
	DBG(MONITOR, ul_debugobj(mn, "failed to create monitor [rc=%d]", rc));
	return rc;
}

/* Returns: <0 on error, 0 on success, 1 nothing (timeout) */
static int read_epoll_events(struct libmnt_monitor *mn, int timeout, struct monitor_entry **act)
{
	int rc;
	struct monitor_entry *me = NULL;
	struct epoll_event events[1];

	if (act)
		*act = NULL;

	do {
		DBG(MONITOR, ul_debugobj(mn, "calling epoll_wait(), timeout=%d", timeout));
		rc = epoll_wait(mn->fd, events, 1, timeout);
		if (rc < 0) {
			rc = -errno;
			goto failed;
		}
		if (rc == 0)
			goto nothing;		/* timeout */

		me = (struct monitor_entry *) events[0].data.ptr;
		if (!me) {
			rc = -EINVAL;
			goto failed;
		}

		/* rc: <0 error; 0 accepted; 1 nothing */
		rc = me->opers->op_process_event(mn, me);
		if (rc < 0)
			goto failed;
		if (rc == 0)
			break;
		/* TODO" recalculate timeout */
	} while (1);

	if (me)
		me->active = 1;
	if (act)
		*act = me;
	return 0;

nothing:
	DBG(MONITOR, ul_debugobj(mn, " *** nothing"));
	return 1;
failed:
	DBG(MONITOR, ul_debugobj(mn, " *** error"));
	return rc;
}

/**
 * mnt_monitor_wait:
 * @mn: monitor
 * @timeout: number of milliseconds, -1 block indefinitely, 0 return immediately
 *
 * Waits for the next change, after the event it's recommended to use
 * mnt_monitor_next_change() to get more details about the change and to
 * avoid false positive events.
 *
 * Returns: 1 success (something changed), 0 timeout, <0 error.
 */
int mnt_monitor_wait(struct libmnt_monitor *mn, int timeout)
{
	int rc;

	if (!mn)
		return -EINVAL;

	if (mn->fd < 0) {
		rc = mnt_monitor_get_fd(mn);
		if (rc < 0)
			return rc;
	}

	rc = read_epoll_events(mn, timeout, NULL);

	return	rc == 0	? 1 :	/* success */
		rc == 1 ? 0 :	/* timeout (aka nothing) */
		rc;		/* error */
}


static struct monitor_entry *get_active(struct libmnt_monitor *mn)
{
	struct libmnt_iter itr;
	struct monitor_entry *me;

	mnt_reset_iter(&itr, MNT_ITER_FORWARD);
	while (monitor_next_entry(mn, &itr, &me) == 0) {
		if (me->active)
			return me;
	}
	return NULL;
}

/**
 * mnt_monitor_next_change:
 * @mn: monitor
 * @filename: returns changed file (optional argument)
 * @type: returns MNT_MONITOR_TYPE_* (optional argument)
 *
 * The function does not wait and is designed to provide details about changes.
 * It is always recommended to use this function to avoid false positives.
 *
 * This function iterates over a list of unprocessed events. When an event is returned
 * by this function, it is marked as processed. If you need details about the last
 * processed event, use the mnt_monitor_event_* functions.
 *
 * Returns: 0 on success, 1 no change, <0 on error
 */
int mnt_monitor_next_change(struct libmnt_monitor *mn,
			     const char **filename,
			     int *type)
{
	int rc = 0;
	struct monitor_entry *me = NULL;

	if (!mn || mn->fd < 0)
		return -EINVAL;

	mn->last = NULL;

	me = get_active(mn);
	if (!me) {
		rc = read_epoll_events(mn, 0, &me);	/* try without timeout */
		if (rc != 0)
			return rc;
	}

	me->active = 0;
	mn->last = me;

	if (filename)
		*filename = me->path;
	if (type)
		*type = me->type;

	DBG(MONITOR, ul_debugobj(mn, " *** success [changed: %s, type=%d]", me->path, me->type));
	return 0;
}

/**
 * mnt_monitor_event_cleanup:
 * @mn: monitor
 *
 * This function cleanups (drain) internal buffers. It's necessary to call
 * this function after event if you do not call mnt_monitor_next_change().
 *
 * Returns: 0 on success, <0 on error
 */
int mnt_monitor_event_cleanup(struct libmnt_monitor *mn)
{
	int rc;

	if (!mn || mn->fd < 0)
		return -EINVAL;

	while ((rc = mnt_monitor_next_change(mn, NULL, NULL)) == 0);
	return rc < 0 ? rc : 0;
}

/**
 * mnt_monitor_event_next_fs:
 * @mn: monitor
 * @fs: filesystem
 *
 * Fill in details about the next filesystem associated with the last event
 * (as returned by mnt_monitor_next_change()). If the event does not provide
 * details, it returns -ENOTSUP.
 *
 * <informalexample>
 *  <programlisting>
 *   while (mnt_monitor_next_change(mn, NULL, &type) == 0) {
 *     if (type == MNT_MONITOR_TYPE_FANOTIFY) {
 *       while (mnt_monitor_event_next_fs(mn, fs) == 0) {
 *         printf("ID=%ju\n", mnt_fs_get_uniq_id(fs));
 *       }
 *     }
 *   }
 *  </programlisting>
 * </informalexample>
 *
 * Returns: 0 on success, 1 no more data, <0 on error
 *
 * Since: 2.42
 */
int mnt_monitor_event_next_fs(struct libmnt_monitor *mn, struct libmnt_fs *fs)
{
	if (!mn || !fs)
		return -EINVAL;
	if (!mn->last)
		return 1;
	if (!mn->last->opers->op_next_fs)
		return -ENOTSUP;

	return mn->last->opers->op_next_fs(mn, mn->last, fs);
}

#ifdef TEST_PROGRAM

static struct libmnt_monitor *create_test_monitor(int argc, char *argv[])
{
	struct libmnt_monitor *mn;
	int i;

	mn = mnt_new_monitor();
	if (!mn) {
		warn("failed to allocate monitor");
		goto err;
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "userspace") == 0) {
			if (mnt_monitor_enable_userspace(mn, TRUE, NULL)) {
				warn("failed to initialize userspace monitor");
				goto err;
			}

		} else if (strcmp(argv[i], "mountinfo") == 0) {
			if (mnt_monitor_enable_mountinfo(mn, TRUE)) {
				warn("failed to initialize kernel monitor");
				goto err;
			}
		} else if (strcmp(argv[i], "fanotify") == 0) {
			if (mnt_monitor_enable_fanotify(mn, TRUE, -1)) {
				warn("failed to initialize fanotify monitor");
				goto err;
			}
		} else if (strcmp(argv[i], "veil") == 0) {
			mnt_monitor_veil_kernel(mn, 1);
		}
	}
	if (i == 1) {
		warnx("No monitor type specified");
		goto err;
	}

	return mn;
err:
	mnt_unref_monitor(mn);
	return NULL;
}

/*
 * create a monitor and add the monitor fd to epoll
 */
static int __test_epoll(struct libmnt_test *ts __attribute__((unused)),
			int argc, char *argv[], int cleanup)
{
	int fd, efd = -1, rc = -1;
	struct epoll_event ev;
	struct libmnt_monitor *mn = create_test_monitor(argc, argv);
	struct libmnt_fs *fs = NULL;

	if (!mn)
		return -1;

	fd = mnt_monitor_get_fd(mn);
	if (fd < 0) {
		warn("failed to initialize monitor fd");
		goto done;
	}

	efd = epoll_create1(EPOLL_CLOEXEC);
	if (efd < 0) {
		warn("failed to create epoll");
		goto done;
	}

	ev.events = EPOLLIN;
	ev.data.fd = fd;

	rc = epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
	if (rc < 0) {
		warn("failed to add fd to epoll");
		goto done;
	}

	do {
		const char *filename = NULL;
		struct epoll_event events[1];
		int n;

		printf("waiting for changes...\n");

		n = epoll_wait(efd, events, 1, -1);
		if (n < 0) {
			rc = -errno;
			warn("polling error");
			goto done;
		}
		if (n == 0 || events[0].data.fd != fd)
			continue;

		printf(" top-level FD active\n");
		if (cleanup)
			mnt_monitor_event_cleanup(mn);
		else {
			int type = 0;

			while (mnt_monitor_next_change(mn, &filename, &type) == 0) {
				printf("  %s: change detected\n", filename);

				if (type == MNT_MONITOR_TYPE_FANOTIFY) {
					if (!fs)
						fs = mnt_new_fs();
					while (mnt_monitor_event_next_fs(mn, fs) == 0) {
						mnt_fs_fetch_statmount(fs, 0);
						printf("ID=%ju (%s %s)\n",
							mnt_fs_get_uniq_id(fs),
							mnt_fs_get_target(fs),
							mnt_fs_is_attached(fs) ? "ATTACHED" :
							mnt_fs_is_detached(fs) ? "DETACHED" :
							mnt_fs_is_moved(fs) ? "MOVED" : "???");
					}
				}
			}
		}
	} while (1);

	rc = 0;
done:
	if (efd >= 0)
		close(efd);
	mnt_unref_monitor(mn);
	mnt_unref_fs(fs);
	return rc;
}

/*
 * create a monitor and add the monitor fd to epoll
 */
static int test_epoll(struct libmnt_test *ts, int argc, char *argv[])
{
	return __test_epoll(ts, argc, argv, 0);
}

static int test_epoll_cleanup(struct libmnt_test *ts, int argc, char *argv[])
{
	return __test_epoll(ts, argc, argv, 1);
}

/*
 * create a monitor and wait for a change
 */
static int test_wait(struct libmnt_test *ts __attribute__((unused)),
		     int argc, char *argv[])
{
	const char *filename;
	struct libmnt_monitor *mn = create_test_monitor(argc, argv);

	if (!mn)
		return -1;

	printf("waiting for changes...\n");
	while (mnt_monitor_wait(mn, -1) > 0) {
		printf("notification detected\n");

		while (mnt_monitor_next_change(mn, &filename, NULL) == 0)
			printf(" %s: change detected\n", filename);

		printf("waiting for changes...\n");
	}
	mnt_unref_monitor(mn);
	return 0;
}

int main(int argc, char *argv[])
{
	struct libmnt_test tss[] = {
		{ "--epoll", test_epoll, "<userspace mountinfo fanotify veil ...>  monitor in epoll" },
		{ "--epoll-clean", test_epoll_cleanup, "<userspace mountinfo fanotify veil ...>  monitor in epoll and clean events" },
		{ "--wait",  test_wait,  "<userspace mountinfo fanotify veil ...>  monitor wait function" },
		{ NULL }
	};

	return mnt_run_test(tss, argc, argv);
}

#endif /* TEST_PROGRAM */
