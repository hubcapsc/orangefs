/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <getopt.h>

#include "pvfs2.h"
#include "pvfs2-mgmt.h"
#include "pvfs2-internal.h"

#define HISTORY 5
#define FREQUENCY 3

#ifndef PVFS2_VERSION
#define PVFS2_VERSION "Unknown"
#endif

#define MAX_KEY_CNT 18
/* macros for accessing data returned from server */
#define VALID_FLAG(s,h) (perf_matrix[(s)][((h) * (key_cnt + 2)) + key_cnt] != 0.0)
#define ID(s,h) (perf_matrix[(s)][((h) * (key_cnt + 2)) + key_cnt])
#define START_TIME(s,h) (perf_matrix[(s)][((h) * (key_cnt + 2)) + key_cnt])
#define READ(s,h) (perf_matrix[(s)][((h) * (key_cnt + 2)) + 0])
#define WRITE(s,h) (perf_matrix[(s)][((h) * (key_cnt + 2)) + 1])
#define METADATA_READ(s,h) (perf_matrix[(s)][((h) * (key_cnt + 2)) + 2])
#define METADATA_WRITE(s,h) (perf_matrix[(s)][((h) * (key_cnt + 2)) + 3])

int key_cnt; /* holds the Number of keys */


struct options
{
    char* mnt_point;
    int mnt_point_set;
};

static struct options* parse_args(int argc, char* argv[]);
static void usage(int argc, char** argv);

int main(int argc, char **argv)
{
    int ret = -1;
    PVFS_fs_id cur_fs;
    struct options* user_opts = NULL;
    char pvfs_path[PVFS_NAME_MAX] = {0};
    int i,j;
    PVFS_credential cred;
    int io_server_count;
    int64_t** perf_matrix;
    uint64_t* end_time_ms_array;
    uint32_t* next_id_array;
    PVFS_BMI_addr_t *addr_array;
    int tmp_type;
    uint64_t next_time;
    float bw;

    /* look at command line arguments */
    user_opts = parse_args(argc, argv);
    if(!user_opts)
    {
	fprintf(stderr, "Error: failed to parse command line arguments.\n");
	usage(argc, argv);
	return(-1);
    }

    ret = PVFS_util_init_defaults();
    if(ret < 0)
    {
	PVFS_perror("PVFS_util_init_defaults", ret);
	return(-1);
    }

    /* translate local path into pvfs2 relative path */
    ret = PVFS_util_resolve(user_opts->mnt_point,
                            &cur_fs,
                            pvfs_path,
                            PVFS_NAME_MAX);
    if(ret < 0)
    {
	PVFS_perror("PVFS_util_resolve", ret);
	return(-1);
    }

    ret = PVFS_util_gen_credential_defaults(&cred);
    if (ret < 0)
    {
        PVFS_perror("PVFS_util_gen_credential", ret);
        return(-1);
    }

    /* count how many I/O servers we have */
    ret = PVFS_mgmt_count_servers(cur_fs,
                                  PVFS_MGMT_IO_SERVER,
	                          &io_server_count);
    if(ret < 0)
    {
	PVFS_perror("PVFS_mgmt_count_servers", ret);
	return(-1);
    }

    /* allocate a 2 dimensional array for statistics */
    perf_matrix = (int64_t **)malloc(io_server_count * sizeof(int64_t *));
    if(!perf_matrix)
    {
	perror("malloc");
	return(-1);
    }
    for(i = 0; i < io_server_count; i++)
    {
	perf_matrix[i] = (int64_t *)malloc(MAX_KEY_CNT * 
                                           HISTORY *
                                           sizeof(int64_t));
	if (perf_matrix[i] == NULL)
	{
	    perror("malloc");
	    return -1;
	}
    }

    /* allocate an array to keep up with what iteration of statistics
     * we need from each server 
     */
    next_id_array = (uint32_t *) malloc(io_server_count * sizeof(uint32_t));
    if (next_id_array == NULL)
    {
	perror("malloc");
	return -1;
    }
    memset(next_id_array, 0, io_server_count * sizeof(uint32_t));

    /* allocate an array to keep up with end times from each server */
    end_time_ms_array = (uint64_t *)malloc(io_server_count * sizeof(uint64_t));
    if (end_time_ms_array == NULL)
    {
	perror("malloc");
	return -1;
    }

    /* build a list of servers to talk to */
    addr_array = (PVFS_BMI_addr_t *)malloc(io_server_count *
                 sizeof(PVFS_BMI_addr_t));
    if (addr_array == NULL)
    {
	perror("malloc");
	return -1;
    }
    ret = PVFS_mgmt_get_server_array(cur_fs,
				     PVFS_MGMT_IO_SERVER,
				     addr_array,
				     &io_server_count);
    if (ret < 0)
    {
	PVFS_perror("PVFS_mgmt_get_server_array", ret);
	return -1;
    }

    /* loop for ever, grabbing stats at regular intervals */
    while (1)
    {
        PVFS_util_refresh_credential(&cred);
        key_cnt = MAX_KEY_CNT;
	ret = PVFS_mgmt_perf_mon_list(cur_fs,
				      &cred,
				      perf_matrix, 
				      end_time_ms_array,
				      addr_array,
				      next_id_array,
				      io_server_count, 
                                      &key_cnt,
				      HISTORY,
				      NULL, NULL);
	if (ret < 0)
	{
	    PVFS_perror("PVFS_mgmt_perf_mon_list", ret);
	    return -1;
	}

	printf("\nPVFS2 I/O server bandwith statistics (MB/sec):\n");
	printf("==================================================\n");
	for (i = 0; i < io_server_count; i++)
	{
	    printf("\nread:  %-30s ",
		   PVFS_mgmt_map_addr(cur_fs, addr_array[i], &tmp_type));
	    for (j = 0; j < HISTORY; j++)
	    {
		/* only print valid measurements */
		if(!VALID_FLAG(i, j))
		    break;

		/* shortcut if measurement is zero */
		if(READ(i, j) == 0)
		{
		    printf("\t0.0");
		    continue;
		}

		/* figure out what time interval to use */
		if (j == (HISTORY - 1) || !VALID_FLAG(i, j+1))
		    next_time = end_time_ms_array[i];
		else
		    next_time = START_TIME(i, j + 1);

		/* bw calculation */
		bw = ((float)READ(i, j) * 1000.0) / 
		    (float)(next_time - START_TIME(i, j));
		bw = bw / (float)(1024.0 * 1024.0);
		printf("\t%10f", bw);
	    }

	    printf("\nwrite: %-30s ",
		   PVFS_mgmt_map_addr(cur_fs, addr_array[i], &tmp_type));

	    for (j = 0; j < HISTORY; j++)
	    {
		/* only print valid measurements */
		if (!VALID_FLAG(i, j))
		    break;

		/* shortcut if measurement is zero */
		if (WRITE(i,j) == 0)
		{
		    printf("\t0.0");
		    continue;
		}

		/* figure out what time interval to use */
		if (j == (HISTORY - 1) || !VALID_FLAG(i, j + 1))
		    next_time = end_time_ms_array[i];
		else
		    next_time = START_TIME(i, j + 1);

		/* bw calculation */
		bw = ((float)WRITE(i, j) * 1000.0) / 
		    (float)(next_time - START_TIME(i, j));
		bw = bw / (float)(1024.0*1024.0);
		printf("\t%10f", bw);
	    }

            printf("\n\nPVFS2 metadata op statistics (# of operations):\n");
            printf("==================================================");
            printf("\nread:  %-30s ",
                   PVFS_mgmt_map_addr(cur_fs, addr_array[i], &tmp_type));

	    for(j = 0; j < HISTORY; j++)
	    {
		if (!VALID_FLAG(i,j))
                {
		    break;
                }
		printf("\t%llu", llu(METADATA_READ(i, j)));
	    }

            printf("\nwrite:  %-30s ",
                   PVFS_mgmt_map_addr(cur_fs, addr_array[i], &tmp_type));

	    for(j = 0; j < HISTORY; j++)
	    {
		if (!VALID_FLAG(i,j))
                {
		    break;
                }
		printf("\t%llu", llu(METADATA_WRITE(i, j)));
	    }

	    printf("\ntimestep:\t\t\t");
	    for(j = 0; j < HISTORY; j++)
	    {
		if(!VALID_FLAG(i,j))
		    break;

		printf("\t%u", (unsigned)ID(i, j));
	    }
	    printf("\n");
	}
	fflush(stdout);
	sleep(FREQUENCY);
    }

    PVFS_sys_finalize();

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
    char flags[] = "vm:";
    int one_opt = 0;
    int len = 0;

    struct options *tmp_opts = NULL;
    int ret = -1;

    /* create storage for the command line options */
    tmp_opts = (struct options *) malloc(sizeof(struct options));
    if(tmp_opts == NULL)
    {
	return(NULL);
    }
    memset(tmp_opts, 0, sizeof(struct options));

    /* look at command line arguments */
    while((one_opt = getopt(argc, argv, flags)) != EOF)
    {
	switch(one_opt)
        {
            case('v'):
                printf("%s\n", PVFS2_VERSION);
                exit(0);
	    case('m'):
		len = strlen(optarg)+1;
		tmp_opts->mnt_point = (char*)malloc(len+1);
		if(!tmp_opts->mnt_point)
		{
		    free(tmp_opts);
		    return(NULL);
		}
		memset(tmp_opts->mnt_point, 0, len+1);
		ret = sscanf(optarg, "%s", tmp_opts->mnt_point);
		if(ret < 1){
		    free(tmp_opts);
		    return(NULL);
		}
		/* TODO: dirty hack... fix later.  The remove_dir_prefix()
		 * function expects some trailing segments or at least
		 * a slash off of the mount point
		 */
		strcat(tmp_opts->mnt_point, "/");
		tmp_opts->mnt_point_set = 1;
		break;
	    case('?'):
		usage(argc, argv);
		exit(EXIT_FAILURE);
	}
    }

    if (!tmp_opts->mnt_point_set)
    {
	free(tmp_opts);
	return(NULL);
    }

    return(tmp_opts);
}


static void usage(int argc, char **argv)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage  : %s [-m fs_mount_point]\n", argv[0]);
    fprintf(stderr, "Example: %s -m /mnt/pvfs2\n", argv[0]);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
