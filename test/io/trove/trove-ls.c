/*
 * (C) 2002 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <trove.h>
#include <trove-test.h>

char storage_space[SSPACE_SIZE] = "/tmp/storage-space-foo";
char file_system[FS_SIZE] = "fs-foo";
char path_to_dir[PATH_SIZE] = "/";
TROVE_handle requested_file_handle = 4095;

extern char *optarg;

int parse_args(int argc, char **argv);
int path_lookup(TROVE_coll_id coll_id, char *path, TROVE_handle *out_handle_p);

#define KEYVAL_ARRAY_LEN 10

int main(int argc, char **argv)
{
    int ret, num_processed, count, i,j;
    TROVE_op_id op_id;
    TROVE_coll_id coll_id;
    TROVE_handle handle;
    TROVE_ds_state state;
    TROVE_keyval_s key[KEYVAL_ARRAY_LEN], val[KEYVAL_ARRAY_LEN];
    TROVE_ds_position pos = TROVE_ITERATE_START;
    char *method_name;
    char path_name[PATH_SIZE];

    TROVE_handle ls_handle[KEYVAL_ARRAY_LEN];
    char ls_name[KEYVAL_ARRAY_LEN][PATH_SIZE];

    ret = parse_args(argc, argv);
    if (ret < 0) {
	fprintf(stderr, "argument parsing failed.\n");
	return -1;
    }

    ret = trove_initialize(storage_space, 0, &method_name, 0);
    if (ret < 0) {
	fprintf(stderr, "initialize failed.\n");
	return -1;
    }

    /* try to look up collection used to store file system */
    ret = trove_collection_lookup(file_system, &coll_id, NULL, &op_id);
    if (ret < 0) {
	fprintf(stderr, "collection lookup failed.\n");
	return -1;
    }

    strcpy(path_name, path_to_dir);
    printf("path is %s\n", path_name);

    /* find the directory handle */
    ret = path_lookup(coll_id, path_name, &handle);
    if (ret < 0) {
	return -1;
    }

    /* TODO: verify that this is in fact a directory! */

    /* iterate through keyvals in directory */

    /* trove_keyval_iterate will let the caller know how much progress was made
     * through the 'count' parameter.  The caller should check 'count' after
     * calling trove_keval_iterate: if it is different, that means EOF reached
     */

    for (j=0; j< KEYVAL_ARRAY_LEN; j++ ) {
	    key[j].buffer = ls_name[j];
	    key[j].buffer_sz = PATH_SIZE;
	    val[j].buffer = &ls_handle[j]; 
	    val[j].buffer_sz = sizeof(ls_handle);
    }

    for (;;) {
	num_processed = KEYVAL_ARRAY_LEN;
	ret = trove_keyval_iterate(coll_id,
				   handle,
				   &pos,
				   key,
				   val,
				   &num_processed,
				   0,
				   NULL,
				   NULL, 
				   &op_id);
	if (ret == -1) return -1;

	while (ret == 0) ret=trove_dspace_test(coll_id, op_id, &count, NULL, NULL, &state);
	if (ret < 0) return -1; 
	
	if (num_processed == 0) return 0;
	
	for(i = 0; i < num_processed; i++ ) {
	    printf("%s (%Ld)\n", (char *) key[i].buffer, *(TROVE_handle *) val[i].buffer);
	}
    }
    
    return 0;
}

int path_lookup(TROVE_coll_id coll_id, char *path, TROVE_handle *out_handle_p)
{
    int ret, count;
    TROVE_ds_state state;
    TROVE_keyval_s key, val;
    TROVE_op_id op_id;
    TROVE_handle handle;

    char root_handle_string[] = ROOT_HANDLE_STRING;

    /* get root */
    key.buffer = root_handle_string;
    key.buffer_sz = strlen(root_handle_string) + 1;
    val.buffer = &handle;
    val.buffer_sz = sizeof(handle);
    ret = trove_collection_geteattr(coll_id, &key, &val, 0, NULL, &op_id);
    while (ret == 0) trove_dspace_test(coll_id, op_id, &count, NULL, NULL, &state);
    if (ret < 0) {
	fprintf(stderr, "collection geteattr (for root handle) failed.\n");
	return -1;
    }

    /* TODO: handle more than just a root handle! */

    *out_handle_p = handle;
    return 0;
}

int parse_args(int argc, char **argv)
{
    int c;

    while ((c = getopt(argc, argv, "s:c:p:h:")) != EOF) {
	switch (c) {
	    case 's':
		strncpy(storage_space, optarg, SSPACE_SIZE);
		break;
	    case 'c': /* collection */
		strncpy(file_system, optarg, FS_SIZE);
		break;
	    case 'p':
		strncpy(path_to_dir, optarg, PATH_SIZE);
	    case 'h':
		requested_file_handle = atoll(optarg);
		break;
	    case '?':
	    default:
		return -1;
	}
    }
    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sw=4 noexpandtab
 */
