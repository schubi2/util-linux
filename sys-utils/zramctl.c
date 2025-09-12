/*
 * zramctl - control compressed block devices in RAM
 *
 * Copyright (c) 2014 Timofey Titovets <Nefelim4ag@gmail.com>
 * Copyright (C) 2014 Karel Zak <kzak@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://gnu.org/licenses/>.
 */

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/file.h>
#include <sys/types.h>
#include <dirent.h>

#include <libsmartcols.h>

#if HAVE_DECL_SD_DEVICE_NEW_FROM_SYSPATH
#include <systemd/sd-device.h>
#endif

#include "c.h"
#include "cctype.h"
#include "nls.h"
#include "closestream.h"
#include "strutils.h"
#include "xalloc.h"
#include "sysfs.h"
#include "optutils.h"
#include "ismounted.h"
#include "strv.h"
#include "path.h"
#include "pathnames.h"

/*#define CONFIG_ZRAM_DEBUG*/

#define _cleanup_(x) __attribute__((__cleanup__(x)))

#ifdef CONFIG_ZRAM_DEBUG
# define DBG(x)	 do { fputs("zram: ", stderr); x; fputc('\n', stderr); } while(0)
#else
# define DBG(x)
#endif

/* status output columns */
struct colinfo {
	const char *name;
	double whint;
	int flags;
	const char *help;
};

enum {
	COL_NAME = 0,
	COL_DISKSIZE,
	COL_ORIG_SIZE,
	COL_COMP_SIZE,
	COL_ALGORITHM,
	COL_STREAMS,
	COL_ZEROPAGES,
	COL_MEMTOTAL,
	COL_MEMLIMIT,
	COL_MEMUSED,
	COL_MIGRATED,
	COL_COMPRATIO,
	COL_MOUNTPOINT
};

static const struct colinfo infos[] = {
	[COL_NAME]      = { "NAME",      0.25, 0, N_("zram device name") },
	[COL_DISKSIZE]  = { "DISKSIZE",     5, SCOLS_FL_RIGHT, N_("limit on the uncompressed amount of data") },
	[COL_ORIG_SIZE] = { "DATA",         5, SCOLS_FL_RIGHT, N_("uncompressed size of stored data") },
	[COL_COMP_SIZE] = { "COMPR",        5, SCOLS_FL_RIGHT, N_("compressed size of stored data") },
	[COL_ALGORITHM] = { "ALGORITHM",    3, 0, N_("the selected compression algorithm") },
	[COL_STREAMS]   = { "STREAMS",      3, SCOLS_FL_RIGHT, N_("number of concurrent compress operations") },
	[COL_ZEROPAGES] = { "ZERO-PAGES",   3, SCOLS_FL_RIGHT, N_("empty pages with no allocated memory") },
	[COL_MEMTOTAL]  = { "TOTAL",        5, SCOLS_FL_RIGHT, N_("all memory including allocator fragmentation and metadata overhead") },
	[COL_MEMLIMIT]  = { "MEM-LIMIT",    5, SCOLS_FL_RIGHT, N_("memory limit used to store compressed data") },
	[COL_MEMUSED]   = { "MEM-USED",     5, SCOLS_FL_RIGHT, N_("peak memory usage to store compressed data") },
	[COL_MIGRATED]  = { "MIGRATED",     5, SCOLS_FL_RIGHT, N_("number of objects migrated by compaction") },
	[COL_COMPRATIO] = { "COMP-RATIO",   5, SCOLS_FL_RIGHT, N_("compression ratio: DATA/TOTAL") },
	[COL_MOUNTPOINT]= { "MOUNTPOINT",0.10, SCOLS_FL_TRUNC, N_("where the device is mounted") },
};

static int columns[ARRAY_SIZE(infos) * 2] = {-1};
static size_t ncolumns;

enum {
	MM_ORIG_DATA_SIZE = 0,
	MM_COMPR_DATA_SIZE,
	MM_MEM_USED_TOTAL,
	MM_MEM_LIMIT,
	MM_MEM_USED_MAX,
	MM_ZERO_PAGES,
	MM_NUM_MIGRATED
};

static const char *const mm_stat_names[] = {
	[MM_ORIG_DATA_SIZE]  = "orig_data_size",
	[MM_COMPR_DATA_SIZE] = "compr_data_size",
	[MM_MEM_USED_TOTAL]  = "mem_used_total",
	[MM_MEM_LIMIT]       = "mem_limit",
	[MM_MEM_USED_MAX]    = "mem_used_max",
	[MM_ZERO_PAGES]      = "zero_pages",
	[MM_NUM_MIGRATED]    = "num_migrated"
};

struct zram {
	char	devname[32];
	int	lock_fd;
	struct	path_cxt *sysfs;	/* device specific sysfs directory */
	char	**mm_stat;

#if HAVE_DECL_SD_DEVICE_NEW_FROM_SYSPATH
	sd_device	*device;
#endif

	unsigned int mm_stat_probed : 1,
		     control_probed : 1,
		     has_control : 1;	/* has /sys/class/zram-control/ */
};

static unsigned int raw, no_headings, inbytes;
static struct path_cxt *__control;

static int get_column_id(size_t num)
{
	assert(num < ncolumns);
	assert(columns[num] < (int) ARRAY_SIZE(infos));
	return columns[num];
}

static const struct colinfo *get_column_info(int num)
{
	return &infos[ get_column_id(num) ];
}

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(infos); i++) {
		const char *cn = infos[i].name;

		if (!c_strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);
	return -1;
}

#if HAVE_DECL_SD_DEVICE_NEW_FROM_SYSPATH
static int monitor_callback(sd_device_monitor *m, sd_device *device, void *userdata)
{
	struct zram *z = userdata;
	sd_device_action_t a;
	const char *s;

	assert(z);
	assert(device);

	if (sd_device_get_action(device, &a) < 0)
		return 0;

	if (a == SD_DEVICE_REMOVE)
		return 0;

	if (sd_device_get_devname(device, &s) < 0)
		return 0;

	if (strcmp(s, z->devname) != 0)
		return 0;

	if (sd_device_get_is_initialized(device) <= 0)
		return 0;

	assert(!z->device);
	z->device = sd_device_ref(device);

	return sd_event_exit(sd_device_monitor_get_event(m), 0);
}
#endif

static int zram_wait_initialized(struct zram *z)
{
#if HAVE_DECL_SD_DEVICE_NEW_FROM_SYSPATH
	_cleanup_(sd_device_unrefp) sd_device *dev = NULL;
	_cleanup_(sd_device_monitor_unrefp) sd_device_monitor *m = NULL;
	sd_event *event;
	int r;

	assert(z);

	z->device = sd_device_unref(z->device);

	/* prepare device monitor. */
	r = sd_device_monitor_new(&m);
	if (r < 0)
		return r;

	r = sd_device_monitor_filter_add_match_subsystem_devtype(m, "block", "disk");
	if (r < 0)
		return r;

	r = sd_device_monitor_start(m, monitor_callback, z);
	if (r < 0)
		return r;

	event = sd_device_monitor_get_event(m);

	/* Wait up to 3 seconds. */
	r = sd_event_add_time_relative(event, NULL, CLOCK_BOOTTIME, 3 * 1000 * 1000, 0, NULL, (void*) (intptr_t) (-ETIMEDOUT));
	if (r < 0)
	        return r;

	/* Check if the device is already initialized. */
#if HAVE_DECL_SD_DEVICE_OPEN
	/* sd_device_new_from_devname() and sd_device_open() are both since systemd-251 */
	r = sd_device_new_from_devname(&dev, z->devname);
#else
	{
		char syspath[PATH_MAX];
		const char *slash;

		slash = strrchr(z->devname, '/');
		if (!slash)
			return -EINVAL;
		snprintf(syspath, sizeof(syspath), "/sys/class/block/%s", slash+1);

		r = sd_device_new_from_syspath(&dev, syspath);
	}
#endif
	if (r < 0)
		return r;

	r = sd_device_get_is_initialized(dev);
	if (r < 0)
		return r;
	if (r > 0) {
		/* Already initialized. It is not necessary to wait with the monitor. */
		z->device = dev;
		dev = NULL;
		return 0;
	}

	return sd_event_loop(event);
#else
	assert(z);
	return 0;
#endif
}

static int zram_lock(struct zram *z, int operation)
{
	int fd, r;

	assert(z);
	assert((operation & ~LOCK_NB) == LOCK_SH ||
	       (operation & ~LOCK_NB) == LOCK_EX);

	if (z->lock_fd >= 0)
		return 0;

#if HAVE_DECL_SD_DEVICE_OPEN
	if (z->device) {
		fd = sd_device_open(z->device, O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
	        if (fd < 0)
			return fd;
	} else {
#endif
		fd = open(z->devname, O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
		if (fd < 0)
			return -errno;
#if HAVE_DECL_SD_DEVICE_OPEN
	}
#endif

	if (flock(fd, operation) < 0) {
		r = -errno;
		close(fd);
		return r;
	}

	z->lock_fd = fd;
	return 0;
}

static void zram_unlock(struct zram *z) {
	if (z && z->lock_fd >= 0) {
		close(z->lock_fd);
		z->lock_fd = -EBADF;
	}
}

static void zram_reset_stat(struct zram *z)
{
	if (z) {
		ul_strv_free(z->mm_stat);
		z->mm_stat = NULL;
		z->mm_stat_probed = 0;
	}
}

static void zram_set_devname(struct zram *z, const char *devname, size_t n)
{
	assert(z);

	if (!devname)
		snprintf(z->devname, sizeof(z->devname), "/dev/zram%zu", n);
	else
		xstrncpy(z->devname, devname, sizeof(z->devname));

	DBG(fprintf(stderr, "set devname: %s", z->devname));
	ul_unref_path(z->sysfs);
	z->sysfs = NULL;
	zram_reset_stat(z);
}

static int zram_get_devnum(struct zram *z)
{
	int n;

	assert(z);

	if (sscanf(z->devname, "/dev/zram%d", &n) == 1)
		return n;
	return -EINVAL;
}

static struct zram *new_zram(const char *devname)
{
	struct zram *z = xcalloc(1, sizeof(struct zram));

	DBG(fprintf(stderr, "new: %p", z));
	if (devname)
		zram_set_devname(z, devname, 0);

	z->lock_fd = -EBADF;
	return z;
}

static void free_zram(struct zram *z)
{
	if (!z)
		return;
	DBG(fprintf(stderr, "free: %p", z));
	ul_unref_path(z->sysfs);
	zram_reset_stat(z);
	zram_unlock(z);

#if HAVE_DECL_SD_DEVICE_NEW_FROM_SYSPATH
	sd_device_unref(z->device);
#endif

	free(z);
}

static struct path_cxt *zram_get_sysfs(struct zram *z)
{
	assert(z);

	if (!z->sysfs) {
		dev_t devno = sysfs_devname_to_devno(z->devname);
		if (!devno)
			return NULL;
		z->sysfs = ul_new_sysfs_path(devno, NULL, NULL);
		if (!z->sysfs)
			return NULL;
		if (*z->devname != '/')
			/* canonicalize the device name according to /sys */
			sysfs_blkdev_get_path(z->sysfs, z->devname, sizeof(z->devname));
	}

	return z->sysfs;
}

static inline int zram_exist(struct zram *z)
{
	assert(z);

	errno = 0;
	if (zram_get_sysfs(z) == NULL) {
		errno = ENODEV;
		return 0;
	}

	DBG(fprintf(stderr, "%s exists", z->devname));
	return 1;
}

static int zram_set_u64parm(struct zram *z, const char *attr, uint64_t num)
{
	struct path_cxt *sysfs = zram_get_sysfs(z);
	if (!sysfs)
		return -EINVAL;
	DBG(fprintf(stderr, "%s writing %ju to %s", z->devname, num, attr));
	return ul_path_write_u64(sysfs, num, attr);
}

static int zram_set_strparm(struct zram *z, const char *attr, const char *str)
{
	struct path_cxt *sysfs = zram_get_sysfs(z);
	if (!sysfs)
		return -EINVAL;
	DBG(fprintf(stderr, "%s writing %s to %s", z->devname, str, attr));
	return ul_path_write_string(sysfs, str, attr);
}


static int zram_used(struct zram *z)
{
	uint64_t size;
	struct path_cxt *sysfs = zram_get_sysfs(z);

	if (sysfs &&
	    ul_path_read_u64(sysfs, &size, "disksize") == 0 &&
	    size > 0) {

		DBG(fprintf(stderr, "%s used", z->devname));
		return 1;
	}
	DBG(fprintf(stderr, "%s unused", z->devname));
	return 0;
}

static int zram_has_control(struct zram *z)
{
	if (!z->control_probed) {
		z->has_control = access(_PATH_SYS_CLASS "/zram-control/", F_OK) == 0 ? 1 : 0;
		z->control_probed = 1;
		DBG(fprintf(stderr, "zram-control: %s", z->has_control ? "yes" : "no"));
	}

	return z->has_control;
}

static struct path_cxt *zram_get_control(void)
{
	if (!__control)
		__control = ul_new_path(_PATH_SYS_CLASS "/zram-control");
	return __control;
}

static int zram_control_add(struct zram *z)
{
	int n = 0;
	struct path_cxt *ctl;

	if (!zram_has_control(z) || !(ctl = zram_get_control()))
		return -ENOSYS;

	if (ul_path_read_s32(ctl, &n, "hot_add") != 0 || n < 0)
		return n;

	DBG(fprintf(stderr, "hot-add: %d", n));
	zram_set_devname(z, NULL, n);
	return 0;
}

static int zram_control_remove(struct zram *z)
{
	struct path_cxt *ctl;
	int n;

	if (!zram_has_control(z) || !(ctl = zram_get_control()))
		return -ENOSYS;

	n = zram_get_devnum(z);
	if (n < 0)
		return n;

	DBG(fprintf(stderr, "hot-remove: %d", n));
	return ul_path_write_u64(ctl, n, "hot_remove");
}

static struct zram *find_free_zram(void)
{
	struct zram *z = new_zram(NULL);
	size_t i;
	int isfree = 0;

	for (i = 0; isfree == 0; i++) {
		DBG(fprintf(stderr, "find free: checking zram%zu", i));
		zram_set_devname(z, NULL, i);
		if (!zram_exist(z) && zram_control_add(z) != 0)
			break;
		isfree = !zram_used(z);
	}
	if (!isfree) {
		free_zram(z);
		z = NULL;
	}
	return z;
}

static int get_mm_stat(struct zram *z,
			 size_t idx, int bytes,
			 char **re_str, uint64_t *re_num)
{
	struct path_cxt *sysfs;
	const char *name;
	uint64_t num;

	assert(idx < ARRAY_SIZE(mm_stat_names));
	assert(z);

	sysfs = zram_get_sysfs(z);
	if (!sysfs)
		return -ENOENT;

	/* Linux >= 4.1 uses /sys/block/zram<id>/mm_stat */
	if (!z->mm_stat && !z->mm_stat_probed) {
		char *str = NULL;
		if (ul_path_read_string(sysfs, &str, "mm_stat") > 0 && str) {
			z->mm_stat = ul_strv_split(str, " ");

			/* make sure kernel provides mm_stat as expected */
			if (ul_strv_length(z->mm_stat) < ARRAY_SIZE(mm_stat_names)) {
				ul_strv_free(z->mm_stat);
				z->mm_stat = NULL;
			}
		}
		z->mm_stat_probed = 1;
		free(str);
		str = NULL;
	}

	if (z->mm_stat) {
		if (re_str && bytes)
			*re_str = xstrdup(z->mm_stat[idx]);

		num = strtou64_or_err(z->mm_stat[idx], _("Failed to parse mm_stat"));
		if (re_num)
			*re_num = num;
		if (re_str && !bytes)
			*re_str = size_to_human_string(SIZE_SUFFIX_1LETTER, num);
		return 0;
	}

	/* Linux < 4.1 uses /sys/block/zram<id>/<attrname> */
	name = mm_stat_names[idx];
	if (re_str && bytes)
		ul_path_read_string(sysfs, re_str, name);

	if ((re_str && !bytes) || re_num) {
		int rc = ul_path_read_u64(sysfs, &num, name);
		if (rc != 0)
			return rc;

		if (re_str && !bytes)
			*re_str = size_to_human_string(SIZE_SUFFIX_1LETTER, num);
		if (re_num)
			*re_num = num;
	}

	return 0;
}

static char *get_mm_stat_string(struct zram *z, size_t idx, int bytes)
{
	char *str = NULL;

	get_mm_stat(z, idx, bytes, &str, NULL);
	return str;
}

static uint64_t get_mm_stat_number(struct zram *z, size_t idx)
{
	uint64_t num = 0;

	get_mm_stat(z, idx, 0, NULL, &num);
	return num;
}

static void fill_table_row(struct libscols_table *tb, struct zram *z)
{
	static struct libscols_line *ln;
	struct path_cxt *sysfs;
	size_t i;
	uint64_t num;

	assert(tb);
	assert(z);

	DBG(fprintf(stderr, "%s: filling status table", z->devname));

	sysfs = zram_get_sysfs(z);
	if (!sysfs)
		return;

	ln = scols_table_new_line(tb, NULL);
	if (!ln)
		err(EXIT_FAILURE, _("failed to allocate output line"));

	for (i = 0; i < ncolumns; i++) {
		char *str = NULL;

		switch (get_column_id(i)) {
		case COL_NAME:
			str = xstrdup(z->devname);
			break;
		case COL_DISKSIZE:
			if (inbytes)
				ul_path_read_string(sysfs, &str, "disksize");

			else if (ul_path_read_u64(sysfs, &num, "disksize") == 0)
				str = size_to_human_string(SIZE_SUFFIX_1LETTER, num);
			break;
		case COL_ALGORITHM:
		{
			char *alg = NULL;

			ul_path_read_string(sysfs, &alg, "comp_algorithm");
			if (alg) {
				char* lbr = strrchr(alg, '[');
				char* rbr = strrchr(alg, ']');

				if (lbr != NULL && rbr != NULL && rbr - lbr > 1)
					str = xstrndup(lbr + 1, rbr - lbr - 1);
				free(alg);
			}
			break;
		}
		case COL_MOUNTPOINT:
		{
			char path[PATH_MAX] = { '\0' };
			int fl;

			check_mount_point(z->devname, &fl, path, sizeof(path));
			if (*path)
				str = xstrdup(path);
			break;
		}
		case COL_COMPRATIO:
			xasprintf(&str, "%.4f",
				(double) get_mm_stat_number(z, MM_ORIG_DATA_SIZE)
				/ get_mm_stat_number(z, MM_MEM_USED_TOTAL));
			break;
		case COL_STREAMS:
			ul_path_read_string(sysfs, &str, "max_comp_streams");
			break;
		case COL_ZEROPAGES:
			str = get_mm_stat_string(z, MM_ZERO_PAGES, 1);
			break;
		case COL_ORIG_SIZE:
			str = get_mm_stat_string(z, MM_ORIG_DATA_SIZE, inbytes);
			break;
		case COL_COMP_SIZE:
			str = get_mm_stat_string(z, MM_COMPR_DATA_SIZE, inbytes);
			break;
		case COL_MEMTOTAL:
			str = get_mm_stat_string(z, MM_MEM_USED_TOTAL, inbytes);
			break;
		case COL_MEMLIMIT:
			str = get_mm_stat_string(z, MM_MEM_LIMIT, inbytes);
			break;
		case COL_MEMUSED:
			str = get_mm_stat_string(z, MM_MEM_USED_MAX, inbytes);
			break;
		case COL_MIGRATED:
			str = get_mm_stat_string(z, MM_NUM_MIGRATED, inbytes);
			break;
		}
		if (str && scols_line_refer_data(ln, i, str))
			err(EXIT_FAILURE, _("failed to add output data"));
	}
}

static void status(struct zram *z)
{
	struct libscols_table *tb;
	size_t i;
	DIR *dir;
	struct dirent *d;

	scols_init_debug(0);

	tb = scols_new_table();
	if (!tb)
		err(EXIT_FAILURE, _("failed to allocate output table"));

	scols_table_enable_raw(tb, raw);
	scols_table_enable_noheadings(tb, no_headings);

	for (i = 0; i < ncolumns; i++) {
		const struct colinfo *col = get_column_info(i);

		if (!scols_table_new_column(tb, col->name, col->whint, col->flags))
			err(EXIT_FAILURE, _("failed to initialize output column"));
	}

	if (z) {
		/* just one device specified */
		fill_table_row(tb, z);
		goto print_table;
	}

	/* list all used devices */
	z = new_zram(NULL);
	if (!(dir = opendir(_PATH_DEV)))
		err(EXIT_FAILURE, _("cannot open %s"), _PATH_DEV);

	while ((d = readdir(dir))) {
		int n;
		if (sscanf(d->d_name, "zram%d", &n) != 1)
			continue;
		zram_set_devname(z, NULL, n);
		if (zram_exist(z) && zram_used(z))
			fill_table_row(tb, z);
	}
	closedir(dir);
	free_zram(z);

print_table:
	scols_print_table(tb);
	scols_unref_table(tb);
}

static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;
	size_t i;

	fputs(USAGE_HEADER, out);
	fprintf(out, _(	" %1$s [options] <device>\n"
			" %1$s -r <device> [...]\n"
			" %1$s [options] -f | <device> -s <size>\n"),
			program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Set up and control zram devices.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -a, --algorithm <alg>     compression algorithm to use\n"), out);
	fputs(_(" -b, --bytes               print sizes in bytes, not in human-readable form\n"), out);
	fputs(_(" -f, --find                find a free device\n"), out);
	fputs(_(" -n, --noheadings          don't print headings\n"), out);
	fputs(_(" -o, --output <list>       columns to use for status output\n"), out);
	fputs(_("     --output-all          output all columns\n"), out);
	fputs(_(" -p, --algorithm-params <parameter>...\n"
		"                           parameters for the compression algorithm\n"), out);
	fputs(_(" -r, --reset <device>...   reset the specified zram devices\n"), out);
	fputs(_("     --raw                 use raw status output format\n"), out);
	fputs(_(" -s, --size <size>         device size\n"), out);
	fputs(_(" -t, --streams <number>    number of compression streams\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fprintf(out, USAGE_HELP_OPTIONS(27));

	fputs(USAGE_ARGUMENTS, out);
	fprintf(out, USAGE_ARG_SIZE(_("<size>")));

	fputs(_(" <alg> is the name of an algorithm; supported are:\n"), out);
	fputs(  "   lzo, lz4, lz4hc, deflate, 842, zstd\n", out);
	fputs(_("   (List may be inaccurate, consult man page.)\n"), out);

	fputs(USAGE_COLUMNS, out);
	for (i = 0; i < ARRAY_SIZE(infos); i++)
		fprintf(out, " %11s  %s\n", infos[i].name, _(infos[i].help));

	fprintf(out, USAGE_MAN_TAIL("zramctl(8)"));
	exit(EXIT_SUCCESS);
}

/* actions */
enum {
	A_NONE = 0,
	A_STATUS,
	A_CREATE,
	A_FINDONLY,
	A_RESET
};

int main(int argc, char **argv)
{
	uintmax_t size = 0, nstreams = 0;
	char *algorithm = NULL;
	char *algorithm_params = NULL;
	int rc = 0, c, find = 0, act = A_NONE;
	struct zram *zram = NULL;
	char *outarg = NULL;

	enum {
		OPT_RAW = CHAR_MAX + 1,
		OPT_LIST_TYPES
	};

	static const struct option longopts[] = {
		{ "algorithm",       required_argument, NULL, 'a' },
		{ "bytes",           no_argument, NULL, 'b' },
		{ "find",            no_argument, NULL, 'f' },
		{ "help",            no_argument, NULL, 'h' },
		{ "output",          required_argument, NULL, 'o' },
		{ "output-all",      no_argument, NULL, OPT_LIST_TYPES },
		{ "algorithm-params",required_argument, NULL, 'p' },
		{ "noheadings",      no_argument, NULL, 'n' },
		{ "reset",           no_argument, NULL, 'r' },
		{ "raw",             no_argument, NULL, OPT_RAW },
		{ "size",            required_argument, NULL, 's' },
		{ "streams",         required_argument, NULL, 't' },
		{ "version",         no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 }
	};

	static const ul_excl_t excl[] = {
		{ 'f', 'o', 'r' },
		{ 'o', 'r', 's' },
		{ 0 }
	};
	int excl_st[ARRAY_SIZE(excl)] = UL_EXCL_STATUS_INIT;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	while ((c = getopt_long(argc, argv, "a:bfho:p:nrs:t:V", longopts, NULL)) != -1) {

		err_exclusive_options(c, longopts, excl, excl_st);

		switch (c) {
		case 'a':
			algorithm = optarg;
			break;
		case 'b':
			inbytes = 1;
			break;
		case 'f':
			find = 1;
			break;
		case 'o':
			outarg = optarg;
			break;
		case OPT_LIST_TYPES:
			for (ncolumns = 0; ncolumns < ARRAY_SIZE(infos); ncolumns++)
				columns[ncolumns] = ncolumns;
			break;
		case 'p':
			algorithm_params = optarg;
			break;
		case 's':
			size = strtosize_or_err(optarg, _("failed to parse size"));
			act = A_CREATE;
			break;
		case 't':
			nstreams = strtou64_or_err(optarg, _("failed to parse streams"));
			break;
		case 'r':
			act = A_RESET;
			break;
		case OPT_RAW:
			raw = 1;
			break;
		case 'n':
			no_headings = 1;
			break;

		case 'V':
			print_version(EXIT_SUCCESS);
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (find && optind < argc)
		errx(EXIT_FAILURE, _("option --find is mutually exclusive "
				     "with <device>"));
	if (act == A_NONE)
		act = find ? A_FINDONLY : A_STATUS;

	if (act != A_RESET && optind + 1 < argc)
		errx(EXIT_FAILURE, _("only one <device> at a time is allowed"));

	if ((act == A_STATUS || act == A_FINDONLY) && (algorithm || algorithm_params || nstreams))
		errx(EXIT_FAILURE, _("options --algorithm, --algorithm-params, and --streams "
				     "must be combined with --size"));

	ul_path_init_debug();
	ul_sysfs_init_debug();

	switch (act) {
	case A_STATUS:
		if (!ncolumns) {		/* default columns */
			columns[ncolumns++] = COL_NAME;
			columns[ncolumns++] = COL_ALGORITHM;
			columns[ncolumns++] = COL_DISKSIZE;
			columns[ncolumns++] = COL_ORIG_SIZE;
			columns[ncolumns++] = COL_COMP_SIZE;
			columns[ncolumns++] = COL_MEMTOTAL;
			columns[ncolumns++] = COL_STREAMS;
			columns[ncolumns++] = COL_MOUNTPOINT;
		}

		if (outarg && string_add_to_idarray(outarg,
					columns, ARRAY_SIZE(columns),
					&ncolumns, column_name_to_id) < 0)
	                return EXIT_FAILURE;

		if (optind < argc) {
			zram = new_zram(argv[optind++]);
			if (!zram_exist(zram))
				err(EXIT_FAILURE, "%s", zram->devname);
		}
		status(zram);
		free_zram(zram);
		break;
	case A_RESET:
		if (optind == argc)
			errx(EXIT_FAILURE, _("no device specified"));
		while (optind < argc) {
			zram = new_zram(argv[optind]);
			if (!zram_exist(zram) ||
			    zram_wait_initialized(zram) ||
			    zram_lock(zram, LOCK_EX | LOCK_NB) ||
			    (zram_unlock(zram), zram_set_u64parm(zram, "reset", 1))) {
				warn(_("%s: failed to reset"), zram->devname);
				rc = 1;
			}
			zram_control_remove(zram);
			free_zram(zram);
			optind++;
		}
		break;
	case A_FINDONLY:
		zram = find_free_zram();
		if (!zram)
			errx(EXIT_FAILURE, _("no free zram device found"));
		printf("%s\n", zram->devname);
		free_zram(zram);
		break;
	case A_CREATE:
		if (find) {
			zram = find_free_zram();
			if (!zram)
				errx(EXIT_FAILURE, _("no free zram device found"));
		} else if (optind == argc)
			errx(EXIT_FAILURE, _("no device specified"));
		else {
			zram = new_zram(argv[optind]);
			if (!zram_exist(zram))
				err(EXIT_FAILURE, "%s", zram->devname);
		}

		/* Wait for udevd initialized the device. */
		if (zram_wait_initialized(zram))
			err(EXIT_FAILURE, _("%s: failed to wait for initialized"), zram->devname);

		/* Even if the device has been initialized by udevd, the device may be still opened and
		 * locked by udevd. Let's wait for the lock taken by udevd is released. */
		if (zram_lock(zram, LOCK_EX))
			err(EXIT_FAILURE, _("%s: failed to lock"), zram->devname);

		/* Writting 'reset' attribute is refused by the kernel when the device node is opened.
		 * Hence, we cannot keep the lock, unfortunately. */
		zram_unlock(zram);

		if (zram_set_u64parm(zram, "reset", 1))
			err(EXIT_FAILURE, _("%s: failed to reset"), zram->devname);

		if (nstreams &&
		    zram_set_u64parm(zram, "max_comp_streams", nstreams) &&
		    errno != ENOENT)
			err(EXIT_FAILURE, _("%s: failed to set number of streams"), zram->devname);

		if (algorithm &&
		    zram_set_strparm(zram, "comp_algorithm", algorithm))
			err(EXIT_FAILURE, _("%s: failed to set algorithm"), zram->devname);

		if (algorithm_params &&
		    zram_set_strparm(zram, "algorithm_params", algorithm_params))
			err(EXIT_FAILURE, _("%s: failed to set algorithm params"), zram->devname);

		if (zram_set_u64parm(zram, "disksize", size))
			err(EXIT_FAILURE, _("%s: failed to set disksize (%ju bytes)"),
				zram->devname, size);
		if (find)
			printf("%s\n", zram->devname);
		free_zram(zram);
		break;
	}

	ul_unref_path(__control);
	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
