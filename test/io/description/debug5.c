/*
 * (C) 2002 Clemson University.
 *
 * See COPYING in top-level directory.
 */       

#include <stdlib.h>
#include <stdio.h>
#include <pvfs2-types.h>
#include <gossip.h>
#include <pvfs2-debug.h>

#include <pvfs-distribution.h>
#include <pvfs-request.h>
#include <pint-request.h>

#include <simple-stripe.h>

#define SEGMAX 16
#define BYTEMAX (4*1024*1024)

int main(int argc, char **argv)
{
	int i;
	PINT_Request *r1;
	PINT_Request *r2;
	PINT_Request_state *rs1;
	PINT_Request_state *rs2;
	PINT_Request_file_data rf1;
	PINT_Request_file_data rf2;

	int retval;
	PVFS_count32 segmax;
	PVFS_offset *offset_array;
	PVFS_size *size_array;
	PVFS_offset offset;
	PVFS_size bytemax;
	PVFS_boolean eof_flag;

	PVFS_count32 blocklength = 10*1024*1024;
	PVFS_size displacement = 20*1024*1024;  

	PVFS_Request_indexed(1, &blocklength, &displacement, PVFS_BYTE, &r1);
	PVFS_Request_indexed(1, &blocklength, &displacement, PVFS_BYTE, &r2);

	rs1 = PINT_New_request_state(r1);
	rs2 = PINT_New_request_state(r2);

	rf1.iod_num = 0;
	rf1.iod_count = 3;
	rf1.fsize = 8454144;
	rf1.dist = PVFS_Dist_create("simple_stripe");
	rf1.extend_flag = 0;
	PINT_Dist_lookup(rf1.dist);

	rf2.iod_num = 1;
	rf2.iod_count = 3;
	rf2.fsize = 8454144;
	rf2.dist = PVFS_Dist_create("simple_stripe");
	rf2.extend_flag = 0;
	PINT_Dist_lookup(rf2.dist);

   /* Turn on debugging */
	//gossip_enable_stderr(); 
	//gossip_set_debug_mask(1,REQUEST_DEBUG);

	offset_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
	size_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));

// server stuff below this line
/****************************************************************/

	offset = 0;
	fprintf(stderr, "\n************************************\n");
	eof_flag = 0;
	segmax = SEGMAX;
	bytemax = BYTEMAX;

	/* process request */
	retval = PINT_Process_request(rs1, &rf1, &segmax,
		offset_array, size_array, &offset, &bytemax, 
		&eof_flag, PINT_SERVER);

	if(retval >= 0)
	{
		fprintf(stderr, "results of PINT_Process_request(PINT_SERVER):\n");
		fprintf(stderr, "req proc offset: %d\n", (int)offset);
		fprintf(stderr, "total size: %d\n", (int)bytemax);
		for(i=0; i<segmax; i++)
		{
			fprintf(stderr, "  segment %d: offset: %d size: %d\n",
				i, (int)offset_array[i], (int)size_array[i]);
		}
	}

	
	if(retval < 0)
	{
		fprintf(stderr, "Error: PINT_Process_request() failure.\n");
		return(-1);
	}
	if(offset == -1)
	{
		fprintf(stderr, "**** server done.\n");
	}

// client stuff below this line
/*******************************************************************/

	offset = 0;
	fprintf(stderr, "\n************************************\n");
	eof_flag = 0;
	segmax = SEGMAX;
	bytemax = BYTEMAX;

	/* process request */
	retval = PINT_Process_request(rs2, &rf2, &segmax,
		offset_array, size_array, &offset, &bytemax, 
		&eof_flag, PINT_CLIENT);

	if(retval >= 0)
	{
		fprintf(stderr, "results of PINT_Process_request(PINT_CLIENT):\n");
		fprintf(stderr, "req proc offset: %d\n", (int)offset);
		fprintf(stderr, "total size: %d\n", (int)bytemax);
		for(i=0; i<segmax; i++)
		{
			fprintf(stderr, "  segment %d: offset: %d size: %d\n",
				i, (int)offset_array[i], (int)size_array[i]);
		}
	}

	
	if(retval < 0)
	{
		fprintf(stderr, "Error: PINT_Process_request() failure.\n");
		return(-1);
	}
	if(offset == -1)
	{
		fprintf(stderr, "**** client request done.\n");
	}
	fprintf(stderr, "\n************************************\n");
	eof_flag = 0;
	segmax = SEGMAX;
	bytemax = 393216;

	/* process request */
	// gossip_enable_stderr(); 
	// gossip_set_debug_mask(1,REQUEST_DEBUG);
	
	retval = PINT_Process_request(rs2, &rf2, &segmax,
		offset_array, size_array, &offset, &bytemax, 
		&eof_flag, PINT_CLIENT);

	if(retval >= 0)
	{
		fprintf(stderr, "results of PINT_Process_request(PINT_CLIENT):\n");
		fprintf(stderr, "req proc offset: %d\n", (int)offset);
		fprintf(stderr, "total size: %d\n", (int)bytemax);
		for(i=0; i<segmax; i++)
		{
			fprintf(stderr, "  segment %d: offset: %d size: %d\n",
				i, (int)offset_array[i], (int)size_array[i]);
		}
	}

	
	if(retval < 0)
	{
		fprintf(stderr, "Error: PINT_Process_request() failure.\n");
		return(-1);
	}
	if(offset != -1)
	{
		fprintf(stderr, "\nAIEEEeee!  Why doesn't the client side set req processing offset to -1?.\n");
		fprintf(stderr, "... the server stopped correctly after this many bytes\n");
	}
	return 0;
}
