/* 
 * (C) 2001 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

/*

SMS:  1. Very simple machine.  errors can be reported directly to client.
      2. Request scheduler used, but not sure how good this is.
      3. Documented
				      
SFS:  1. Almost all pre/post
      2. Some assertions
		      
TS:   1. Exists but not thorough.

My TODO list for this SM:

 This state machine is pretty simple and for the most part is hammered out.
 it might need a little more documentation, but that is it.

*/

#include <state-machine.h>
#include <server-config.h>
#include <pvfs2-server.h>
#include <string.h>
#include <pvfs2-attr.h>
#include <assert.h>
#include <gossip.h>

static int create_init(PINT_server_op *s_op, job_status_s *ret);
static int create_cleanup(PINT_server_op *s_op, job_status_s *ret);
static int create_create(PINT_server_op *s_op, job_status_s *ret);
static int create_release_posted_job(PINT_server_op *s_op, job_status_s *ret);
static int create_send_bmi(PINT_server_op *s_op, job_status_s *ret);
void create_init_state_machine(void);

#if 0
PINT_state_machine_s create_req_s = 
{
	NULL,
	"create",
	create_init_state_machine
};
#endif

%%

machine create(init, create2, send, release, cleanup)
{
	state init
	{
		run create_init;
		default => create2;
	}

	state create2
	{
		run create_create;
		default => send;
	}

	state send
	{
		run create_send_bmi;
		default => release;
	}

	state release
	{
		run create_release_posted_job;
		default => cleanup;
	}

	state cleanup
	{
		run create_cleanup;
		default => init;
	}
}

%%

#if 0
/*
 * Function: create_init_state_machine
 *
 * Params:   void
 *
 * Pre:      None
 *
 * Post:     None
 *
 * Returns:  void
 *
 * Synopsis: Set up the state machine for create. 
 *           
 */

void create_init_state_machine(void)
{

    create_req_s.state_machine = create;

}
#endif

/*
 * Function: create_init
 *
 * Params:   server_op *s_op, 
 *           job_status_s *ret
 *
 * Pre:      Valid request
 *
 * Post:     Job Scheduled
 *
 * Returns:  int
 *
 * Synopsis: Create is a relatively easy server operation.  We just need to 
 *           schedule the creation of the new dataspace.
 *           
 */


static int create_init(PINT_server_op *s_op, job_status_s *ret)
{

    int job_post_ret;

    job_post_ret = job_req_sched_post(s_op->req,
	    s_op,
	    ret,
	    &(s_op->scheduled_id));

    return(job_post_ret);

}


/*
 * Function: create_create
 *
 * Params:   server_op *s_op, 
 *           job_status_s *ret
 *
 * Pre:      None
 *
 * Post:     None
 *
 * Returns:  int
 *
 * Synopsis: Create the new dataspace with the values provided in the response.
 *           
 */


static int create_create(PINT_server_op *s_op, job_status_s *ret)
{

    int job_post_ret;
    job_id_t i;

    job_post_ret = job_trove_dspace_create(s_op->req->u.create.fs_id,
	    s_op->req->u.create.bucket,
	    s_op->req->u.create.handle_mask,
	    s_op->req->u.create.object_type,
	    NULL,
	    s_op,
	    ret,
	    &i);

    return(job_post_ret);

}


/*
 * Function: create_bmi_send
 *
 * Params:   server_op *s_op, 
 *           job_status_s *ret
 *
 * Pre:      None
 *
 * Post:     None
 *
 * Returns:  int
 *
 * Synopsis: Send a message to the client.  If the dataspace was successfully
 *           created, send the new handle back.
 *           
 */


static int create_send_bmi(PINT_server_op *s_op, job_status_s *ret)
{

    int job_post_ret=0;
    job_id_t i;

    /* fill in response -- status field is the only generic one we should have to set */
    s_op->resp->status = ret->error_code;

    /* Set the handle IF it was created */
    if(ret->error_code == 0) 
    {
	gossip_err("Handle Created: %lld\n",ret->handle);
	s_op->resp->u.create.handle = ret->handle;

    }

    /* Encode the message */
    job_post_ret = PINT_encode(s_op->resp,
	    PINT_ENCODE_RESP,
	    &(s_op->encoded),
	    s_op->addr,
	    s_op->enc_type);
    assert(job_post_ret == 0);

#ifndef PVFS2_SERVER_DEBUG_BMI

    job_post_ret = job_bmi_send_list(
	    s_op->addr,
	    s_op->encoded.buffer_list,
	    s_op->encoded.size_list,
	    s_op->encoded.list_count,
	    s_op->encoded.total_size,
	    s_op->tag,
	    s_op->encoded.buffer_flag,
	    0,
	    s_op, 
	    ret, 
	    &i);

#else

    job_post_ret = job_bmi_send(
	    s_op->addr,
	    s_op->encoded.buffer_list[0],
	    s_op->encoded.total_size,
	    s_op->tag,
	    s_op->encoded.buffer_flag,
	    0,
	    s_op,
	    ret,
	    &i);

#endif


    return(job_post_ret);

}

/*
 * Function: create_release_posted_job
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Pre:      We are done!
 *
 * Post:     We need to let the next operation go.
 *
 * Returns:  int
 *
 * Synopsis: Free the job from the scheduler to allow next job to proceed.
 */

static int create_release_posted_job(PINT_server_op *s_op, job_status_s *ret)
{

    int job_post_ret=0;
    job_id_t i;

    job_post_ret = job_req_sched_release(s_op->scheduled_id,
	    s_op,
	    ret,
	    &i);
    return job_post_ret;
}


/*
 * Function: create_cleanup
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Pre:      None
 *
 * Post:     None
 *
 * Returns:  int
 *
 * Synopsis: free memory and return
 *           
 */


static int create_cleanup(PINT_server_op *s_op, job_status_s *ret)
{

    PINT_encode_release(&(s_op->encoded),PINT_ENCODE_RESP,0);

    PINT_decode_release(&(s_op->decoded),PINT_DECODE_REQ,0);

    if(s_op->resp)
    {
	free(s_op->resp);
    }

    /*
    BMI_memfree(
	    s_op->addr,
	    s_op->req,
	    s_op->unexp_bmi_buff->size,
	    BMI_RECV_BUFFER
	    );
    */

    free(s_op);

    return(0);

}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
