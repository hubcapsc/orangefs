/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>

#include "pvfs2.h"

#define DEFAULT_TAB "/etc/pvfs2tab"

/* optional parameters, filled in by parse_args() */
struct options
{
    int buf_size;
    char* srcfile;
    char* destfile;
};

static struct options* parse_args(int argc, char* argv[]);
static void usage(int argc, char** argv);
static double Wtime(void);

int main(int argc, char **argv)
{
    int ret = -1;
    char str_buf[PVFS_NAME_MAX] = {0};
    char pvfs_path[PVFS_NAME_MAX] = {0};
    PVFS_fs_id cur_fs;
    pvfs_mntlist mnt = {0,NULL};
    PVFS_sysresp_init resp_init;
    PVFS_sysresp_lookup resp_lookup;
    PVFS_sysresp_io resp_io;
    struct options* user_opts = NULL;
    int i = 0;
    int mnt_index = -1;
    int dest_fd = -1;
    int current_size = 0;
    void* buffer = NULL;
    int64_t total_written = 0;
    double time1, time2;
    int32_t blocklength = 0;
    PVFS_size displacement = 0;
    PVFS_fs_id lk_fs_id;
    char* lk_name;
    PVFS_credentials credentials;
    PVFS_pinode_reference pinode_refn;
    PVFS_Request io_req;
    int buffer_size;

    /* look at command line arguments */
    user_opts = parse_args(argc, argv);
    if(!user_opts)
    {
	fprintf(stderr, "Error: failed to parse command line arguments.\n");
	return(-1);
    }

    /* make sure we can access the dest file before we go to any
     * trouble to contact pvfs servers
     */
    dest_fd = open(user_opts->destfile, O_CREAT|O_WRONLY|O_TRUNC,
	0777);
    if(dest_fd < 0)
    {
	perror("open()");
	fprintf(stderr, "Error: could not access dest file: %s\n",
	    user_opts->destfile);
	return(-1);
    }

    /* look at pvfstab */
    if(PVFS_util_parse_pvfstab(DEFAULT_TAB, &mnt))
    {
        fprintf(stderr, "Error: failed to parse pvfstab %s.\n", DEFAULT_TAB);
	close(dest_fd);
        return(-1);
    }

    /* see if the destination resides on any of the file systems
     * listed in the pvfstab; find the pvfs fs relative path
     */
    for(i=0; i<mnt.nr_entry; i++)
    {
	ret = PINT_remove_dir_prefix(user_opts->srcfile,
	    mnt.ptab_p[i].local_mnt_dir, pvfs_path, PVFS_NAME_MAX);
	if(ret == 0)
	{
	    mnt_index = i;
	    break;
	}
    }

    if(mnt_index == -1)
    {
	fprintf(stderr, "Error: could not find filesystem for %s in pvfstab %s\n", 
	    user_opts->srcfile, DEFAULT_TAB);
	close(dest_fd);
	return(-1);

    }

    memset(&resp_init, 0, sizeof(resp_init));
    ret = PVFS_sys_initialize(mnt, 0, &resp_init);
    if(ret < 0)
    {
	PVFS_perror("PVFS_sys_initialize", ret);
	close(dest_fd);
	return(-1);
    }

    /* get the absolute path on the pvfs2 file system */
    if (PINT_remove_base_dir(pvfs_path,str_buf,PVFS_NAME_MAX))
    {
        if (pvfs_path[0] != '/')
        {
            fprintf(stderr, "Error: poorly formatted path.\n");
        }
        fprintf(stderr, "Error: cannot retrieve entry name for creation on %s\n",
               pvfs_path);
	ret = -1;
	goto main_out;
    }

    memset(&resp_lookup, 0, sizeof(PVFS_sysresp_lookup));

    cur_fs = resp_init.fsid_list[mnt_index];

    credentials.uid = getuid();
    credentials.gid = getgid();
    lk_fs_id = cur_fs;

    /* TODO: this is awkward- the remove_base_dir() function
     * doesn't leave an opening slash on the path (because it was
     * first written to help with sys_create() calls).  However,
     * in the sys_lookup() case, we need the opening slash.
     */
    lk_name = (char*)malloc(strlen(str_buf) + 2);
    if(!lk_name)
    {
	perror("malloc()");
	ret = -1;
	goto main_out;
    }
    lk_name[0] = '/';
    strcpy(&(lk_name[1]), str_buf);

    ret = PVFS_sys_lookup(lk_fs_id, lk_name, credentials, &resp_lookup);
    if(ret < 0)
    {
	PVFS_perror("PVFS_sys_lookup", ret);
	ret = -1;
	goto main_out;
    }

    /* start moving data */
    buffer = malloc(user_opts->buf_size);
    if(!buffer)
    {
	PVFS_sys_finalize();
	ret = -1;
	goto main_out;
    }

    pinode_refn = resp_lookup.pinode_refn;
    buffer_size = user_opts->buf_size;
    blocklength = user_opts->buf_size;
    displacement = 0;
    ret = PVFS_Request_indexed(1, &blocklength, &displacement,
	PVFS_BYTE, &io_req);
    if(ret < 0)
    {
	fprintf(stderr, "Error: PVFS_Request_indexed failure.\n");
	ret = -1;
	goto main_out;
    }

    time1 = Wtime();
    while((ret = PVFS_sys_read(pinode_refn, io_req, 0,
                buffer, buffer_size, credentials, &resp_io)) == 0 &&
	resp_io.total_completed > 0)
    {
	current_size = resp_io.total_completed;

	/* write out the data */
	ret = write(dest_fd, buffer, current_size);
	if(ret < 0)
	{
	    perror("write()");
	    ret = -1;
	    goto main_out;
	}

	total_written += current_size;

	/* TODO: need to free the old request description */
	
	/* setup I/O description */
	displacement = total_written;
	ret = PVFS_Request_indexed(1, &blocklength,
	    &displacement, PVFS_BYTE, &io_req);
	if(ret < 0)
	{
	    fprintf(stderr, "Error: PVFS_Request_indexed failure.\n");
	    ret = -1;
	    goto main_out;
	}
    };
    time2 = Wtime();

    if(ret < 0)
    {
	PVFS_perror("PVFS_sys_read()", ret);
	ret = -1;
	goto main_out;
    }

    /* print some statistics */
    printf("PVFS2 Import Statistics:\n");
    printf("********************************************************\n");
    printf("Source path (local): %s\n", user_opts->srcfile);
    printf("Source path (PVFS2 file system): %s\n", pvfs_path);
    printf("File system name: %s\n", mnt.ptab_p[mnt_index].service_name);
    printf("Initial config server: %s\n", mnt.ptab_p[mnt_index].meta_addr);
    printf("********************************************************\n");
    printf("Bytes written: %ld\n", (long)total_written);
    printf("Elapsed time: %f seconds\n", (time2-time1));
    printf("Bandwidth: %f MB/second\n",
	(((double)total_written)/((double)(1024*1024))/(time2-time1)));
    printf("********************************************************\n");

    ret = 0;

main_out:

    PVFS_sys_finalize();

    if(dest_fd > 0)
	close(dest_fd);
    if(buffer)
	free(buffer);

    return(ret);
}


/* parse_args()
 *
 * parses command line arguments
 *
 * returns pointer to options structure on success, NULL on failure
 */
static struct options* parse_args(int argc, char* argv[])
{
    /* getopt stuff */
    extern char* optarg;
    extern int optind, opterr, optopt;
    char flags[] = "b:";
    char one_opt = ' ';

    struct options* tmp_opts = NULL;
    int ret = -1;

    /* create storage for the command line options */
    tmp_opts = (struct options*)malloc(sizeof(struct options));
    if(!tmp_opts){
	return(NULL);
    }
    memset(tmp_opts, 0, sizeof(struct options));

    /* fill in defaults (except for hostid) */
    tmp_opts->buf_size = 10*1024*1024;

    /* look at command line arguments */
    while((one_opt = getopt(argc, argv, flags)) != EOF){
	switch(one_opt){
	    case('b'):
		ret = sscanf(optarg, "%d", &tmp_opts->buf_size);
		if(ret < 1){
		    free(tmp_opts);
		    return(NULL);
		}
		break;
	    case('?'):
		usage(argc, argv);
		exit(EXIT_FAILURE);
	}
    }

    if(optind != (argc - 2))
    {
	usage(argc, argv);
	exit(EXIT_FAILURE);
    }

    /* TODO: should probably malloc and copy instead */
    tmp_opts->srcfile = argv[argc-2];
    tmp_opts->destfile = argv[argc-1];

    return(tmp_opts);
}


static void usage(int argc, char** argv)
{
    fprintf(stderr, 
	"Usage: %s [-b buffer_size] pvfs2_src_file unix_dest_file\n", 
	argv[0]); 
    fprintf(stderr, "\n      Note: this utility reads /etc/pvfs2tab for file system configuration.\n");
    return;
}

static double Wtime(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return((double)t.tv_sec + (double)(t.tv_usec) / 1000000);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */

