
/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef _MNTMONITOR_PRIVATE_H
#define _MNTMONITOR_PRIVATE_H

#include "c.h"
#include <stdbool.h>

struct monitor_opers;

struct monitor_entry {
	int			fd;		/* private entry file descriptor */
	int			id;		/* external identifier (-1 for undefined) */
	char			*path;		/* path to the monitored file */
	int			type;		/* MNT_MONITOR_TYPE_* */
	uint32_t		events;		/* wanted epoll events */

	const struct monitor_opers *opers;
	void			*data;		/* private type-specific data */

	bool			enabled,	/* monitoring fd */
				active;		/* ready for mnt_monitor_next_change() */

	struct list_head	ents;
};

struct libmnt_monitor {
	int			refcount;
	int			fd;		/* public monitor file descriptor */

	struct list_head	ents;
	struct monitor_entry	*last;		/* last active returned by mnt_monitor_next_change() */

	bool			kernel_veiled;
};

struct monitor_opers {
	int (*op_get_fd)(struct libmnt_monitor *, struct monitor_entry *);
	int (*op_close_fd)(struct libmnt_monitor *, struct monitor_entry *);
	int (*op_free_data)(struct monitor_entry *);
	int (*op_process_event)(struct libmnt_monitor *, struct monitor_entry *);
	int (*op_next_fs)(struct libmnt_monitor *, struct monitor_entry *, struct libmnt_fs *);
};

int monitor_modify_epoll(struct libmnt_monitor *mn, struct monitor_entry *me, int enable);
struct monitor_entry *monitor_get_entry(struct libmnt_monitor *mn, int type, int id);
struct monitor_entry *monitor_new_entry(struct libmnt_monitor *mn);
void free_monitor_entry(struct monitor_entry *me);

#endif /* _MNTMONITOR_PRIVATE_H */
