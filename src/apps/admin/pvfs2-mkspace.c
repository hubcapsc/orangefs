/*
 * (C) 2002 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include "pvfs2.h"
#include "mkspace.h"
#include "pvfs2-internal.h"

#ifndef PVFS2_VERSION
#define PVFS2_VERSION "Unknown"
#endif

typedef struct
{
    int use_defaults;
    int verbose;
    PVFS_fs_id coll_id;
    PVFS_handle root_handle;
    int collection_only;
    int delete_storage;
    char collection[PATH_MAX];
    char data_space[PATH_MAX];
    char meta_space[PATH_MAX];
    char config_space[PATH_MAX];
} options_t;

static int default_verbose = 0;
static PVFS_handle default_root_handle = PVFS_HANDLE_NULL_INIT;
static int default_collection_only = 0;
static char default_collection[PATH_MAX] = "pvfs2-fs";

static void print_help(char *progname, options_t *opts);

static int parse_args(int argc, char **argv, options_t *opts)
{
    int ret = 0, option_index = 0;
    const char *cur_option = NULL;
    static struct option long_opts[] =
    {
        {"help",0,0,0},
        {"version",0,0,0},
        {"verbose",0,0,0},
        {"defaults",0,0,0},
        {"data-space",1,0,0},
        {"meta-space",1,0,0},
        {"config-space",1,0,0},
        {"coll-id",1,0,0},
        {"coll-name",1,0,0},
        {"root-handle",1,0,0},
        {"delete-storage",0,0,0},
        {"add-coll",0,0,0},
        {0,0,0,0}
    };

    if (argc == 1)
    {
        print_help(argv[0], opts);
        exit(1);
    }

    while ((ret = getopt_long(argc, argv, "c:i:r:vVhadD",
                              long_opts, &option_index)) != -1)
    {
	switch (ret)
        {
            case 0:
                cur_option = long_opts[option_index].name;

                if (strcmp("help", cur_option) == 0)
                {
                    goto do_help;
                }
                else if (strcmp("version", cur_option) == 0)
                {
                    goto do_version;
                }
                else if (strcmp("verbose", cur_option) == 0)
                {
                    goto do_verbose;
                }
                else if (strcmp("data-space", cur_option) == 0)
                {
                    strncpy(opts->data_space, optarg, PATH_MAX - 1);
                    break;
                }
                else if (strcmp("meta-space", cur_option) == 0)
		{
                    strncpy(opts->meta_space, optarg, PATH_MAX - 1);
                    break;
                }
                else if (strcmp("config-space", cur_option) == 0)
		{
                    strncpy(opts->config_space, optarg, PATH_MAX - 1);
                    break;
                }
                else if (strcmp("coll-id", cur_option) == 0)
                {
                    goto do_collection_id;
                }
                else if (strcmp("coll-name", cur_option) == 0)
                {
                    goto do_collection_name;
                }
                else if (strcmp("root-handle", cur_option) == 0)
                {
                    goto do_root_handle;
                }
                else if (strcmp("add-coll", cur_option) == 0)
                {
                    goto do_add_collection;
                }
                else if (strcmp("defaults", cur_option) == 0)
                {
                    goto do_defaults;
                }
                else if (strcmp("delete-storage", cur_option) == 0)
                {
                    goto do_delete_storage;
                }
                else
                {
                    print_help(argv[0], opts);
                    exit(1);
                }
	    case 'a':
          do_add_collection:
		opts->collection_only = 1;
		break;
	    case 'c':
          do_collection_name:
		strncpy(opts->collection, optarg, PATH_MAX - 1);
		break;
            case 'd':
          do_defaults:
                opts->use_defaults = 1;
                break;
            case 'h':
          do_help:
                print_help(argv[0], opts);
                exit(0);
	    case 'i':
          do_collection_id:
#ifdef HAVE_STRTOULL
		opts->coll_id = (PVFS_fs_id)strtoull(optarg, NULL, 10);
#else
		opts->coll_id = (PVFS_fs_id)strtoul(optarg, NULL, 10);
#endif
		break;
	    case 'r':
          do_root_handle:
                PVFS_OID_str2bin(optarg, &opts->root_handle);
		break;
	    case 'v':
          do_verbose:
		opts->verbose = PVFS2_MKSPACE_STDERR_VERBOSE;
		break;
            case 'V':
          do_version:
                fprintf(stderr,"%s\n",PVFS2_VERSION);
                exit(0);
            case 'D':
          do_delete_storage:
                opts->delete_storage = 1;
                break;
	    case '?':
                fprintf(stderr, "%s: error: unrecognized "
                        "option '%c'.\n", argv[0], ret);
	    default:
                print_help(argv[0], opts);
		return -1;
	}
    }
    return 0;
}

static void print_options(options_t *opts)
{
    if (opts)
    {
        printf("\t   use all defaults    : %s\n",
               (opts->use_defaults ? "yes" : "no"));
        printf("\t   delete storage      : %s\n",
               (opts->delete_storage ? "yes" : "no"));
        printf("\t   verbose             : %s\n",
               (opts->verbose ? "ON" : "OFF"));
        printf("\t   root handle         : %s\n",
               PVFS_OID_str(&opts->root_handle));
        printf("\t   collection-only mode: %s\n",
               (opts->collection_only ? "ON" : "OFF"));
        printf("\t   collection id       : %d\n", opts->coll_id);
        printf("\t   collection name     : %s\n",
               (strlen(opts->collection) ?
                opts->collection : "None specified"));
        printf("\t   data storage space  : %s\n",
               (strlen(opts->data_space) ?
                opts->data_space : "None specified"));
        printf("\tmetadata storage space : %s\n",
	       	(strlen(opts->meta_space) ?
		opts->meta_space : "None specified"));
        printf("\tconfig storage space : %s\n",
	       	(strlen(opts->config_space) ?
		opts->config_space : "None specified"));
    }
}

static void print_help(char *progname, options_t *opts)
{
    fprintf(stderr,"usage: %s [OPTION]...\n", progname);
    fprintf(stderr,"This program is useful for creating a new pvfs2 "
            "storage space with a \nsingle collection, or adding a "
            "new collection to an existing storage space.\n\n");
    fprintf(stderr,"The following arguments can be used to "
           "customize your volume:\n");

    fprintf(stderr,"  -a, --add-coll                       "
            "used to add a collection only\n");
    fprintf(stderr,"  -c, --coll-name                      "
            "create collection with the speciifed name\n");
    fprintf(stderr,"  -d, --defaults                       "
            "use all default options (see below)\n");
    fprintf(stderr,"  -D, --delete-storage                 "
            "REMOVE the storage space (unrecoverable!)\n");
    fprintf(stderr,"  -h, --help                           "
            "show this help listing\n");
    fprintf(stderr,"  -i, --coll-id=ID                     "
            "create collection with the specified id\n");
    fprintf(stderr,"  -r, --root-handle=HANDLE             "
            "create collection with this root handle\n");
    fprintf(stderr,"       --data-space=PATH             "
            "create data storage space at this location\n");
    fprintf(stderr,"       --meta-space=PATH             "
            "create metadata storage space at this location\n");
    fprintf(stderr,"       --config-space=PATH             "
            "create config storage space at this location\n");
    fprintf(stderr,"  -v, --verbose                        "
            "operate in verbose mode\n");
    fprintf(stderr,"  -V, --version                        "
            "print version information and exit\n");

    fprintf(stderr,"\nIf the -d or --defaults option is used, the "
            "following options will be used:\n");
    opts->use_defaults = 1;
    print_options(opts);
}

int main(int argc, char **argv)
{
    int ret = -1;
    options_t opts;
    memset(&opts, 0, sizeof(options_t));

    if (parse_args(argc, argv, &opts))
    {
	fprintf(stderr,"%s: error: argument parsing failed; "
                "aborting!\n", argv[0]);
	return -1;
    }

    if (opts.use_defaults)
    {
        opts.verbose = default_verbose;
        opts.root_handle = default_root_handle;
        opts.collection_only = default_collection_only;
        strncpy(opts.collection, default_collection, PATH_MAX);
    }

    print_options(&opts);

    if (strlen(opts.data_space) == 0)
    {
        fprintf(stderr, "Error: You MUST specify a data storage space\n");
        return -1;
    }

    if (strlen(opts.meta_space) == 0)
    {
        fprintf(stderr, "Error: You MUST specify a metadata storage space\n");
        return -1;
    }

    if (strlen(opts.config_space) == 0)
    {
        fprintf(stderr, "Error: You MUST specify a config storage space\n");
        return -1;
    }

    if (opts.coll_id == PVFS_FS_ID_NULL)
    {
        fprintf(stderr, "Error: You MUST specify a collection ID\n");
        return -1;
    }

    if (opts.delete_storage)
    {
        ret = pvfs2_rmspace(opts.data_space,
                            opts.meta_space,
                            opts.config_space,
			    opts.collection,
                            opts.coll_id, 
			    opts.collection_only,
                            opts.verbose);
    }
    else
    {
        printf("opts.collection_only(%d).\n", opts.collection_only);
        ret = pvfs2_mkspace(opts.data_space,
                            opts.meta_space,
                            opts.config_space,
			    opts.collection,
                            opts.coll_id, 
                            PVFS_HANDLE_NULL,
                            PVFS_HANDLE_NULL,
                            NULL,
                            0,
                            opts.collection_only, 
			    opts.verbose);
    }
    return ret;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
