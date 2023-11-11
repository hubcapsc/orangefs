
/*
 * We collect stuff to send to the notification engine in a
 * buffer. Sometimes there's a pathname included. How big should
 * this buffer be? Maybe it should be dynamic?
 */
#define BUFFER_MAX (FILENAME_MAX * 2)

/*
 * Default directory to store lists in the filesystem. Currently
 * it is in the storage/data directory. That might be a bad
 * choice on a meta-only server :-) ...
 */
#define LIST_DIR "/d.lists"

/*
 * These sprintf format strings are too ugly to mix in with the code...
 * Once we've collected what we need for to describe an operation,
 * we can use whichever one of these lines is appropriate to
 * send the notification to the outside world.
 */
#define SETATTR_F "{\"op\": \"setattr\", \"obj_type\": \"%s\", \"handle\": \"%s\"}\n"
#define C_OBJ_F "{\"op\": \"create\", \"name\": \"%s\", \"obj_type\": \"%s\", \"handle\": \"%s\", \"parent_handle\": \"%s\", \"uid\": \"%s\"}\n"
#define C_SYMLINK_F "{\"op\": \"create\", \"name\": \"%s\", \"obj_type\": \"%s\", \"handle\": \"%s\", \"parent_handle\": \"%s\", \"target\": \"%s\", \"uid\": \"%s\"}\n"
#define RENAME_F "{\"op\": \"rename\", \"name\": \"%s\", \"obj_type\": \"%s\", \"handle\": \"%s\", \"parent_handle\": \"%s\", \"uid\": \"%s\"}\n"
#define RENAME_F2 "{\"op\": \"rename\", \"name\": \"%s\", \"handle\": \"%s\", \"old_handle\": \"%s\", \"uid\": \"%s\"}\n"
#define D_OBJ_F "{\"op\": \"delete\", \"name\": \"%s\", \"obj_type\": \"%s\", \"handle\": \"%s\", \"uid\": \"%s\"}\n"

