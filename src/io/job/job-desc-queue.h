/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* functions for handling queues of jobs that the job interface is
 * managing
 */

#ifndef __JOB_DESC_QUEUE_H
#define __JOB_DESC_QUEUE_H

#include <quicklist.h>
#include <job.h>
#include <pvfs2-types.h>
#include <trove-types.h>
#include <request-scheduler.h>

/* describes BMI operations */
struct bmi_desc
{
	bmi_op_id_t id;
	bmi_error_code_t error_code;
	bmi_size_t actual_size;
};

/* describes trove operations */
struct trove_desc
{
	PVFS_ds_id id;
	PVFS_size actual_size;
	PVFS_vtag_s* vtag;
	PVFS_fs_id fsid;
	PVFS_ds_state state;
	PVFS_handle handle;
	PVFS_ds_position position;
	PVFS_ds_attributes_s attr;
	int count;
};

/* describes unexpected BMI operations */
struct bmi_unexp_desc
{
	struct BMI_unexpected_info* info;
};

/* describes flows */
struct flow_desc
{
	flow_descriptor* flow_d;
};

/* describes request scheduler elements */
struct req_sched_desc
{
	req_sched_id id;
	int post_flag;
	int error_code;
};

enum job_type
{
	JOB_BMI = 1,
	JOB_BMI_UNEXP,
	JOB_TROVE,
	JOB_FLOW,
	JOB_REQ_SCHED
};

/* describes a job, which may be one of several types */
struct job_desc
{
	enum job_type type;                           /* type of job */
	job_id_t job_id;							/* job interface identifier */
	void* job_user_ptr;                 /* user pointer */

	/* union of information for lower level interfaces */
	union                               
	{
		struct bmi_desc bmi;
		struct trove_desc trove;
		struct bmi_unexp_desc bmi_unexp;
		struct flow_desc flow;
		struct req_sched_desc req_sched;
	}u;

	struct qlist_head job_desc_q_link;  /* queue link */
};

typedef struct qlist_head* job_desc_q_p;

struct job_desc* alloc_job_desc(int type);
void dealloc_job_desc(struct job_desc* jd);
job_desc_q_p job_desc_q_new(void);
void job_desc_q_cleanup(job_desc_q_p jdqp);
void job_desc_q_add(job_desc_q_p jdqp, struct job_desc* desc);
void job_desc_q_remove(struct job_desc* desc);
int job_desc_q_empty(job_desc_q_p jdqp);
struct job_desc* job_desc_q_shownext(job_desc_q_p jdqp);
struct job_desc* job_desc_q_search(job_desc_q_p jdqp, job_id_t id);
int job_desc_q_search_multi(job_desc_q_p jdqp, job_id_t* id_array, int*
	inout_count_p, int* index_array);

#endif /* __JOB_DESC_QUEUE_H */
