/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <time.h>
#include <client.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include "pvfs2-util.h"
#include "pvfs2-mgmt.h"

#define DEFAULT_IO_SIZE 8*1024*1024

int main(int argc,char **argv)
{
	PVFS_sysresp_init resp_init;
	PVFS_sysresp_lookup resp_lk;
	PVFS_sysresp_create resp_cr;
	PVFS_sysresp_io resp_io;
	char *filename;
	int name_sz;
	int ret = -1;
	pvfs_mntlist mnt = {0,NULL};
	int io_size = DEFAULT_IO_SIZE;
	int* io_buffer = NULL;
	int i;
	int errors;
	PVFS_fs_id fs_id;
	char* name;
	PVFS_credentials credentials;
	char* entry_name;
	PVFS_pinode_reference parent_refn;
	PVFS_sys_attr attr;
	PVFS_pinode_reference pinode_refn;
	PVFS_Request file_req;
	PVFS_Request mem_req;
	void* buffer;
	int buffer_size;
	PVFS_sysresp_getattr resp_getattr;
	PVFS_handle* dfile_array = NULL;

	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s <file name>\n", argv[0]);
		return(-1);
	}

	if(index(argv[1], '/'))
	{
		fprintf(stderr, "Error: please use simple file names, no path.\n");
		return(-1);
	}
		
	/* create a buffer for running I/O on */
	io_buffer = (int*)malloc(io_size*sizeof(int));
	if(!io_buffer)
	{
		return(-1);
	}

	/* put some data in the buffer so we can verify */
	for (i=0; i<io_size; i++) {
		io_buffer[i]=i;
	}

	/* build the full path name (work out of the root dir for this test) */

	name_sz = strlen(argv[1]) + 2; /* include null terminator and slash */
	filename = malloc(name_sz);
	if(!filename)
	{
		perror("malloc");
		return(-1);
	}
	filename[0] = '/';

	memcpy(&(filename[1]), argv[1], (name_sz-1));

	/* parse pvfstab */
	ret = PVFS_util_parse_pvfstab(NULL, &mnt);
	if (ret < 0)
	{
		fprintf(stderr, "Error: parse_pvfstab() failure.\n");
		return(-1);
	}
	/* init the system interface */
	ret = PVFS_sys_initialize(mnt, GOSSIP_NO_DEBUG, &resp_init);
	if(ret < 0)
	{
		fprintf(stderr, "Error: PVFS_sys_initialize() failure. = %d\n", ret);
		return(ret);
	}

	/*************************************************************
	 * try to look up the target file 
	 */
	
	name = filename;
	fs_id = resp_init.fsid_list[0];
	credentials.uid = getuid();
	credentials.gid = getgid();

	ret = PVFS_sys_lookup(fs_id, name, credentials,
                              &resp_lk, PVFS2_LOOKUP_LINK_FOLLOW);
	/* TODO: really we probably want to look for a specific error code,
	 * like maybe ENOENT?
	 */
	if (ret < 0)
	{
		printf("IO-TEST: lookup failed; creating new file.\n");

		/* get root handle */
		name = "/";
		fs_id = resp_init.fsid_list[0];
		credentials.uid = getuid();
		credentials.gid = getgid();

		ret = PVFS_sys_lookup(fs_id, name, credentials,
                                      &resp_lk, PVFS2_LOOKUP_LINK_NO_FOLLOW);
		if(ret < 0)
		{
			fprintf(stderr, "Error: PVFS_sys_lookup() failed to find root handle.\n");
			return(-1);
		}

		/* create new file */

		attr.owner = getuid();
		attr.group = getgid();
		attr.perms = PVFS_U_WRITE|PVFS_U_READ;
		attr.atime = attr.ctime = attr.mtime = 
		    time(NULL);
		attr.mask = PVFS_ATTR_SYS_ALL_SETABLE;
		parent_refn.handle = resp_lk.pinode_refn.handle;
		parent_refn.fs_id = fs_id;
		entry_name = &(filename[1]); /* leave off slash */
		credentials.uid = getuid();
		credentials.gid = getgid();

		ret = PVFS_sys_create(entry_name, parent_refn, attr, 
					credentials, &resp_cr);
		if(ret < 0)
		{
			fprintf(stderr, "Error: PVFS_sys_create() failure.\n");
			return(-1);
		}

		pinode_refn.fs_id = fs_id;
		pinode_refn.handle = resp_cr.pinode_refn.handle;
	}
	else
	{
		printf("IO-TEST: lookup succeeded; performing I/O on existing file.\n");

		pinode_refn.fs_id = fs_id;
		pinode_refn.handle = resp_lk.pinode_refn.handle;
	}

	/**************************************************************
	 * carry out I/O operation
	 */

	printf("IO-TEST: performing write on handle: %ld, fs: %d\n",
		(long)pinode_refn.handle, (int)pinode_refn.fs_id);

	credentials.uid = getuid();
	credentials.gid = getgid();
	buffer = io_buffer;
	buffer_size = io_size*sizeof(int);

	/* file datatype is tiled, so we can get away with a trivial type here */
	file_req = PVFS_BYTE;

	ret = PVFS_Request_contiguous(io_size*sizeof(int), PVFS_BYTE, &(mem_req));
	if(ret < 0)
	{
		fprintf(stderr, "Error: PVFS_Request_contiguous() failure.\n");
		return(-1);
	}

	ret = PVFS_sys_write(pinode_refn, file_req, 0, buffer, mem_req, 
				credentials, &resp_io);
	if(ret < 0)
	{
		fprintf(stderr, "Error: PVFS_sys_write() failure.\n");
		return(-1);
	}

	printf("IO-TEST: wrote %d bytes.\n", (int)resp_io.total_completed);

	/* uncomment and try out the readback-and-verify stuff that follows
	 * once reading back actually works */
	memset(io_buffer, 0, io_size*sizeof(int));

	/* verify */
	printf("IO-TEST: performing read on handle: %ld, fs: %d\n",
		(long)pinode_refn.handle, (int)pinode_refn.fs_id);

	ret = PVFS_sys_read(pinode_refn, file_req, 0, buffer, mem_req, 
				credentials, &resp_io);
	if(ret < 0)
	{
		fprintf(stderr, "Error: PVFS_sys_read() failure.\n");
		return(-1);
	}
	printf("IO-TEST: read %d bytes.\n", (int)resp_io.total_completed);
	if((io_size*sizeof(int)) != resp_io.total_completed)
	{
		fprintf(stderr, "Error: SHORT READ! skipping verification...\n");
	}
	else
	{
		errors = 0;
		for(i=0; i<io_size; i++) {
			if (i != io_buffer[i] )
			{
				fprintf(stderr, "error: element %d differs: should be %d, is %d\n", i, i, io_buffer[i]); 
				errors++;
			}
		}
		if (errors != 0 )
		{
			fprintf(stderr, "ERROR: found %d errors\n", errors);
		}
		else
		{
			printf("IO-TEST: no errors found.\n");
		}
	}

	/* test out some of the mgmt functionality */
	ret = PVFS_sys_getattr(pinode_refn, PVFS_ATTR_SYS_ALL_NOSIZE,
	    credentials, &resp_getattr);
	if(ret < 0)
	{
	    PVFS_perror("PVFS_sys_getattr", ret);
	    return(-1);
	}

	printf("Target file had %d datafiles:\n", 
	    resp_getattr.attr.dfile_count);

	dfile_array = (PVFS_handle*)malloc(resp_getattr.attr.dfile_count
	    * sizeof(PVFS_handle));
	if(!dfile_array)
	{
	    perror("malloc");
	    return(-1);
	}

	ret = PVFS_mgmt_get_dfile_array(pinode_refn, credentials,
	    dfile_array, resp_getattr.attr.dfile_count);
	if(ret < 0)
	{
	    PVFS_perror("PVFS_mgmt_get_dfile_array", ret);
	    return(-1);
	}
	for(i=0; i<resp_getattr.attr.dfile_count; i++)
	{
	    printf("0x%08Lx\n", Lu(dfile_array[i]));
	}

	/**************************************************************
	 * shut down pending interfaces
	 */

	ret = PVFS_sys_finalize();
	if (ret < 0)
	{
		fprintf(stderr, "Error: PVFS_sys_finalize() failed with errcode = %d\n", ret);
		return (-1);
	}

	free(filename);
	free(io_buffer);
	return(0);
}
