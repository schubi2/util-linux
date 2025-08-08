/*
 * config_files.c instantiates functions defined and described in config_files.h
 */
#include <err.h>
#include <errno.h>
#include <sys/syslog.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "config-files.h"
#include "list.h"
#include "xalloc.h"

#define RUN_SUBDIR "/run"
#define DEFAULT_ETC_SUBDIR "/etc"

/* Checking for main configuration file 
 * 
 * Returning absolute path or NULL if not found
 * The return value has to be freed by the caller.
 */
static char *main_config_file(const char *root,
		       const char *project,
		       const char *config_name,
		       const char *config_suffix)
{
	bool found = false;
	char *path = NULL;
	struct stat st;
	
	if (config_suffix) {
		xasprintf(&path, "%s/%s/%s.%s", root, project, config_name, config_suffix);
		if (stat(path, &st) == 0) {
			found = true;
		} else {
			free(path);
			path = NULL;
		}
	}
	if (!found) {
		/* trying filename without suffix */
		xasprintf(&path, "%s/%s/%s", root, project, config_name);
		if (stat(path, &st) != 0) {
			/* not found */
			free(path);
			path = NULL;
		}
	}

	return path;
}

static struct file_element *new_list_entry(const char *filename)
{
	struct file_element *file_element = xcalloc(1, sizeof(*file_element));

	INIT_LIST_HEAD(&file_element->file_list);

	file_element->filename = xstrdup(filename);

	return file_element;
}

int  config_file_list( struct list_head *file_list,
                       const char *project,
		       const char *etc_subdir,
		       const char *usr_subdir,
		       const char *config_name,
		       const char *config_suffix)
{
	char *filename = NULL;

	INIT_LIST_HEAD(file_list);
	
	if (!config_name){
		warnx("config_name must be a valid value");
		return -1;
	}

	/* Default is /etc */
	if (!etc_subdir)
		etc_subdir = DEFAULT_ETC_SUBDIR;

	if (!usr_subdir)
		usr_subdir = "";

	if (!project)
		project = "";

        bool found = false;
	
	/* Evaluating first "main" file which has to be parsed */
	/* in the following order : /etc /run /usr             */
 	filename = main_config_file(etc_subdir, project, config_name, config_suffix);
	if (filename == NULL)
		filename = main_config_file(RUN_SUBDIR, project, config_name, config_suffix);
	if (filename == NULL)
		filename = main_config_file(usr_subdir, project, config_name, config_suffix);
	if (filename != NULL) {
		struct file_element *entry = new_list_entry(filename);
		list_add_tail(&entry->file_list, file_list);
	}
		
	
	return 0;
}

#if 0


gboolean
g_key_file_load_unix_configurations (GKeyFile       *key_file,
                                     const gchar    *project,
                                     const gchar    *etc_subdir,
                                     const gchar    *usr_subdir,
                                     const gchar    *config_name,
                                     const gchar    *config_suffix,
                                     GKeyFileFlags  flags,
                                     GError         **error)
{
  gchar *path = NULL;
  gchar *scan_dir = NULL;
  int fd = 0;
  int cmp_ret = 0;
  GDir *dir;
  gchar *filename = NULL;
  const gchar *file = NULL;
  gboolean ret = TRUE;
  GError *key_file_error = NULL;
  GList *parsing_list = NULL;
  GList *etc_list = NULL;
  GList *ptr_etc_list = NULL;
  GList *usr_list = NULL;
  GList *ptr_usr_list = NULL;
  GKeyFile *parsed_key_file = g_key_file_new();
  gchar** groups = NULL;
  gchar** groups_ptr = NULL;
  gchar** keys = NULL;
  gchar** keys_ptr = NULL;
  gchar*  value = NULL;
  if (!config_name)
    {
      g_set_error_literal (error, G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_PARSE,
                           _("config_name must be a valid value"));
      return FALSE;
    }
  if (!key_file)
    {
      g_set_error_literal (error, G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_PARSE,
                           _("key_file must be a valid value"));
      return FALSE;
    }

  /* Default is /etc */
  if (!etc_subdir)
    etc_subdir = "/etc";

  if (!usr_subdir)
    usr_subdir = "";

  if (config_suffix)
    filename = g_strdup_printf ("%s.%s", config_name, config_suffix);
  else
    filename = g_strdup (config_name);

  if (!project)
    project = "";

    /* Evaluating first "main" file which has to be parsed */
  path = g_build_filename (etc_subdir, project, filename, NULL);
  fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
  if (fd == -1)
    {
      g_free (path);
      path = g_build_filename ("/run", project, filename, NULL);
      fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    }
  if (fd == -1)
    {
      g_free (path);
      path = g_build_filename (usr_subdir, project, filename, NULL);
      fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    }
  if (fd != -1)
    parsing_list = g_list_append (parsing_list, path);
  else
    g_free (path);

    /* Evaluting all plugin files which has to be parsed and insert it into
     the list in the correct order */
  g_free(filename);
  if (config_suffix)
    filename = g_strdup_printf ("%s.%s.d", config_name, config_suffix);
  else
    filename = g_strdup_printf ("%s.d", config_name);

  scan_dir = g_build_filename (usr_subdir, project, filename, NULL);
  dir = g_dir_open(scan_dir, 0, &key_file_error);
  if (dir)
    {
      while ((file = g_dir_read_name(dir)) != NULL)
        {
          usr_list = g_list_insert_sorted (usr_list, g_strdup (file), cmp_str_function);
        }
      g_dir_close(dir);
    }
  if (key_file_error)
    {
      g_error_free (key_file_error);
      key_file_error = NULL;
    }
  g_free(scan_dir);
  scan_dir = g_build_filename (etc_subdir, project, filename, NULL);
  dir = g_dir_open(scan_dir, 0, &key_file_error);
  if (dir)
    {
      while ((file = g_dir_read_name(dir)) != NULL)
        {
          etc_list = g_list_insert_sorted (etc_list, g_strdup (file), cmp_str_function);
        }
      g_dir_close(dir);
    }
  if (key_file_error)
    {
      g_error_free (key_file_error);
      key_file_error = NULL;
    }
  g_free(scan_dir);

  ptr_usr_list = usr_list;
  ptr_etc_list = etc_list;

  while (ptr_etc_list != NULL)
    {
      while (ptr_usr_list != NULL)
        {
          cmp_ret = g_strcmp0 ((const gchar *)(ptr_usr_list->data),
                               (const gchar *)(ptr_etc_list->data));
          if (cmp_ret < 0)
            {
              parsing_list = g_list_append (parsing_list,
                                            g_build_filename (usr_subdir, project, filename, ptr_usr_list->data, NULL));
              ptr_usr_list = ptr_usr_list->next;
            }
          else
            {
              if (cmp_ret == 0)
                {
                  ptr_usr_list = ptr_usr_list->next;
                }
              break;
            }
        }
      parsing_list = g_list_append (parsing_list,
        g_build_filename (etc_subdir, project, filename, ptr_etc_list->data, NULL));
      ptr_etc_list = ptr_etc_list->next;
    }
  while (ptr_usr_list != NULL)
    {
      parsing_list = g_list_append (parsing_list,
                                    g_build_filename (usr_subdir, project, filename, ptr_usr_list->data, NULL));
      ptr_usr_list = ptr_usr_list->next;
    }
  g_free(filename);
  /* Parsing all configuration files in the correct order and merging the entries.*/
  for (GList *l = parsing_list; l != NULL; l = l->next)
    {
      fd = g_open ((const gchar *) (l->data), O_RDONLY | O_CLOEXEC, 0);
      if (fd != -1)
        {
          if (g_key_file_load_from_fd (parsed_key_file, fd, flags, &key_file_error))
            {
              if (key_file_error)
                {
                  g_propagate_error (error, key_file_error);
                  ret = FALSE;
                  g_error_free (key_file_error);
                  key_file_error = NULL;
                }

              groups = g_key_file_get_groups (parsed_key_file, NULL);
              groups_ptr = groups;
              while (*groups_ptr)
                {
                  keys_ptr = g_key_file_get_keys (parsed_key_file,
                                                  *groups_ptr,
                                                  NULL,
                                                  &key_file_error);
                  if (key_file_error)
                    {
                      g_propagate_error (error, key_file_error);
                      ret = FALSE;
                      g_error_free (key_file_error);
                      key_file_error = NULL;
                    }
                  while (*keys_ptr)
                    {
                      value = g_key_file_get_value (parsed_key_file,
                                                    *groups_ptr,
                                                    *keys_ptr,
                                                    &key_file_error);
                      if (key_file_error)
                        {
                          g_propagate_error (error, key_file_error);
                          ret = FALSE;
                          g_error_free (key_file_error);
                          key_file_error = NULL;
                        }
                      else
                        {
                          g_key_file_set_value (key_file,
                                                *groups_ptr,
                                                *keys_ptr,
                                                value);
                          if (key_file_error)
                            {
                              g_propagate_error (error, key_file_error);
                              ret = FALSE;
                              g_error_free (key_file_error);
                              key_file_error = NULL;
                            }
                        }
                      keys_ptr++;
                    }
                  g_strfreev (keys);
                  groups_ptr++;
                }
              g_strfreev (groups);
              g_key_file_free(parsed_key_file);
              parsed_key_file = g_key_file_new ();
            }
          close (fd);
        }
    }

  g_list_free_full(usr_list, g_free);
  g_list_free_full(etc_list, g_free);
  if (parsing_list == NULL)
    {
      g_set_error_literal (error, G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_NOT_FOUND,
                           _("Valid key file could not be "
                             "found in search dirs"));
      ret = FALSE;
    }
  else
    {
      g_list_free_full(parsing_list, g_free);
      g_key_file_free(parsed_key_file);
    }
  return ret;
}
#endif
