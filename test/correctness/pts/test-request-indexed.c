/*
 * (C) 2002 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* 
 * test-request_indexed: 
 * Author: Michael Speth
 * Date: 8/14/2003
 * Last Updated: 8/14/2003
 */

#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

#include <pint-request.h>
#include <pvfs-distribution.h>
#include <simple-stripe.h>
#include "client.h"
#include "mpi.h"
#include "pts.h"
#include "pvfs-helper.h"
#include "pvfs2-util.h"
#define SEGMAX 16
#define BYTEMAX (4*1024*1024)
extern pvfs_helper_t pvfs_helper;

int test_request(void){
    int i;
    PINT_Request *r1;
    PINT_Request *r2;
    PINT_Request_state *rs1;
    PINT_Request_state *rs2;
    PINT_Request_file_data rf1;
    PINT_Request_file_data rf2;
    PINT_Request_result seg1;
    PINT_Request_result seg2;
                                                                                
                                                                                
    /* PVFS_Process_request arguments */
    int retval;
                                                                                
    int32_t blocklength = 10*1024*1024; /* 10M */

    int32_t tmpSize;
    int32_t tmpOff;
    int32_t segSize;
                                                                                
    /* set up two requests */
    PVFS_size displacement1 = 0;  /* first at offset zero */
    PVFS_size displacement2 = 10*1024*1024;  /* next at 10M offset */
    PVFS_Request_indexed(1, &blocklength, &displacement1, PVFS_BYTE, &r1);
                                                                                
    PVFS_Request_indexed(1, &blocklength, &displacement2, PVFS_BYTE, &r2);
    /* set up two request states */
    rs1 = PINT_New_request_state(r1);
    rs2 = PINT_New_request_state(r2);
                                                                                
    /* set up file data for first request */
    rf1.iod_num = 0;
    rf1.iod_count = 1;
    rf1.fsize = 0;
    rf1.dist = PVFS_Dist_create("simple_stripe");
    rf1.extend_flag = 1;
    PINT_Dist_lookup(rf1.dist);
                                                                                
    /* file data for second request is the same, except the file
     * will have grown by 10M
     */
    rf2.iod_num = 0;
    rf2.iod_count = 1;
    rf2.fsize = 10*1024*1024;
    rf2.dist = PVFS_Dist_create("simple_stripe");
    rf2.extend_flag = 1;
    PINT_Dist_lookup(rf2.dist);
                                                                                 
    /* set up result structures */
    seg1.offset_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
    seg1.size_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
    seg1.segmax = SEGMAX;
    seg1.bytemax = BYTEMAX;
    seg1.segs = 0;
    seg1.bytes = 0;
                                                                                 
    seg2.offset_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
    seg2.size_array = (int64_t *)malloc(SEGMAX * sizeof(int64_t));
    seg2.segmax = SEGMAX;
    seg2.bytemax = BYTEMAX;
    seg2.segs = 0;
    seg2.bytes = 0;
                                                                                 
    /* Turn on debugging */
    /* gossip_enable_stderr();
    gossip_set_debug_mask(1,REQUEST_DEBUG); */
                                                                                 
//    printf("\n************************************\n");
    tmpSize = blocklength;
    tmpOff = displacement1;
    segSize = BYTEMAX;
    
    do
    {
	seg1.bytes = 0;
	seg1.segs = 0;
	/* process request */
	retval = PINT_Process_request(rs1, NULL, &rf1, &seg1, PINT_SERVER);
                                                                                
	if(retval >= 0)
	{
//	    printf("results of PINT_Process_request():\n");
//	    printf("%d segments with %lld bytes\n", seg1.segs, seg1.bytes);
	    for(i = 0; i < seg1.segs; i++)
	    {
//		printf("  segment %d: offset: %d size: %d\n",
 //              i, (int)seg1.offset_array[i], (int)seg1.size_array[i]);
		if(tmpOff != ((int)seg1.offset_array[i])){
		    printf("Error:  segment %d offset is %d but should be %d\n",i,(int)seg1.offset_array[i],tmpOff);
		    return -1;
		}
		if(segSize != ((int)seg1.size_array[i])){
		    printf("Error:  segment %d size is %d but should be %d\n",i,(int)seg1.size_array[i],segSize);
		    return -1;
		}

		if( (tmpSize - BYTEMAX) < BYTEMAX){
		    segSize = tmpSize - BYTEMAX;
		}
		else{   
		    segSize = BYTEMAX;
		}
		tmpSize -= BYTEMAX;
		tmpOff += BYTEMAX;
	    }
	}
    } while(!PINT_REQUEST_DONE(rs1) && retval >= 0);
    if(retval < 0)
    {
      fprintf(stderr, "Error: PINT_Process_request() failure.\n");      return(-1);
   }
//   printf("final file size %lld\n", rf1.fsize);
   if(PINT_REQUEST_DONE(rs1))
   {
//      printf("**** first request done.\n");
   }
 //  printf("\n************************************\n");
    tmpOff = displacement2;
    tmpSize = blocklength;
    segSize = BYTEMAX;
   do
   {
      seg2.bytes = 0;
      seg2.segs = 0;
      /* process request */
      retval = PINT_Process_request(rs2, NULL, &rf2, &seg2, PINT_SERVER);
                                                                                
      if(retval >= 0)
      {
    //     printf("results of PINT_Process_request():\n");
//         printf("%d segments with %lld bytes\n", seg2.segs, seg2.bytes);
         for(i=0; i < seg2.segs; i++)
         {
//            printf("  segment %d: offset: %d size: %d\n",
 //              i, (int)seg2.offset_array[i], (int)seg2.size_array[i]);

		if(tmpOff != ((int)seg2.offset_array[i])){
		    printf("Error:  segment %d offset is %d but should be %d\n",i,(int)seg2.offset_array[i],tmpOff);
		    return -1;
		}
		if(segSize != ((int)seg2.size_array[i])){
		    printf("Error:  segment %d size is %d but should be %d\n",i,(int)seg2.size_array[i],segSize);
		    return -1;
		}

		if( (tmpSize - BYTEMAX) < BYTEMAX){
		    segSize = tmpSize - BYTEMAX;
		}
		else{   
		    segSize = BYTEMAX;
		}
		tmpSize -= BYTEMAX;
		tmpOff += BYTEMAX;
         }
      }
                                                                                
   } while(!PINT_REQUEST_DONE(rs2) && retval >= 0);
                                                                                
   if(retval < 0)
   {
      fprintf(stderr, "Error: PINT_Process_request() failure.\n");
      return(-1);
   }
//   printf("final file size %lld\n", rf2.fsize);
   if(PINT_REQUEST_DONE(rs2))
   {
 //     printf("**** second request done.\n");
   }
                                                                                
   return 0;
}

/* Preconditions:
 * Parameters: comm - special pts communicator, rank - the rank of the process,
 * buf - not used
 * Postconditions: 0 if no errors and nonzero otherwise
 */
int test_request_indexed(MPI_Comm * comm,
		     int rank,
		     char *buf,
		     void *rawparams)
{
    int ret = -1;

    /* right now, the system interface isn't threadsafe, so we just want to run with one process. */
    if (rank == 0)
    {
	ret = test_request();
    }
    return ret;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
