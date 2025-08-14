/*
 * No copyright is claimed.  This code is in the public domain; do with
 * it what you wish.
 *
 * Evaluting a list of configuration filenames which have to be parsed.
 * The order of this file list has been defined by 
 * https://github.com/uapi-group/specifications/blob/main/specs/configuration_files_specification.md
 */

#ifndef UTIL_LINUX_CONFIG_FILES_H
#define UTIL_LINUX_CONFIG_FILES_H

#include "list.h"

struct file_element {
	struct list_head file_list;
	char *filename;
};

/**
 * config_file_list - Evaluting a list of configuration filenames which have to be parsed.
 *
 * @file_list: List of filenames which have to be parsed in that order
 * @project: name of the project used as subdirectory, can be NULL
 * @etc_subdir: absolute directory path for user changed configuration files, can be NULL (default "/etc").
 * @usr_subdir: absolute directory path of vendor defined settings (often "/usr/lib").
 * @config_name: basename of the configuration file. If it is NULL, drop-ins without a main configuration file will be parsed only. 
 * @config_suffix: suffix of the configuration file. Can also be NULL.
 *
 * Returns the length of the file_list.
 *
 * Example:
 * size_t count = 0;
 * count = config_file_list (key_file,
 *                           "foo",
 *                           "/etc",
 *                           "/usr/lib",
 *                           "example",
 *                           "conf");
 *
 * Reading content in different cases in following order:
 *
 * /etc/foo/example.conf OR /etc/foo/example exist:
 *
 *   /etc/foo/example[.conf]
 *   /usr/lib/foo/example[.conf].d/ *.conf
 *   /run/foo/example[.conf].d/ *.conf
 *   /etc/foo/example[.conf].d/ *.conf
 *
 * /etc/foo/example.conf AND /etc/foo/example.conf NOT exist:
 *
 *    /run/foo/example.conf OR /run/foo/example exist:
 *
 *      /run/foo/example[.conf]
 *      /usr/lib/foo/example[.conf].d/ *.conf
 *      /run/foo/example[.conf].d/ *.conf
 *      /etc/foo/example[.conf].d/ *.conf
 *
 *   /run/foo/example.conf AND /run/foo/example NOT exist:
 *
 *      /usr/lib/foo/example[.conf]
 *      /usr/lib/foo/example.conf.d/ *.conf
 *      /run/foo/example.conf.d/ *.conf
 *      /etc/foo/example.conf.d/ *.conf
 *
 * Each file in /etc/ overrides those from /usr/lib and /run for the same basename.
 * I.e. Existing /etc/foo/example.d/foobar.conf overrides /usr/lib/foo/example.d/foobar.conf
 */

size_t config_file_list( struct list_head *file_list,
                       const char *project,
		       const char *etc_subdir,
		       const char *usr_subdir,
		       const char *config_name,
		       const char *config_suffix);
#endif
