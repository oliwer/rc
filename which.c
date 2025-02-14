/* which.c: check to see if a file is executable.

   This function was originally written with Maarten Litmaath's which.c as
   a template, but was changed in order to accommodate the possibility of
   rc's running setuid or the possibility of executing files not in the
   primary group. Much of this file has been re-vamped by Paul Haahr.
   I re-re-vamped the functions that Paul supplied to correct minor bugs
   and to strip out unneeded functionality.
*/

#include "rc.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#include "getgroups.h"

#define X_USR 0100
#define X_GRP 0010
#define X_OTH 0001
#define X_ALL (X_USR|X_GRP|X_OTH)

static bool initialized = FALSE;
static uid_t uid;
static gid_t gid;

#if HAVE_GETGROUPS
static int ngroups;
static GETGROUPS_T *gidset;

/* determine whether gid lies in gidset */

static int ingidset(gid_t g) {
	int i;
	for (i = 0; i < ngroups; ++i)
		if (g == gidset[i])
			return 1;
	return 0;
}
#else
#define ingidset(g) (FALSE)
#endif

/*
   A home-grown access/stat. Does the right thing for group-executable files.
   Returns a bool instead of this -1 nonsense.
*/

bool rc_access(char *path, bool verbose, struct stat *stp) {
	int mask;
	if (stat(path, stp) != 0) {
		if (verbose) /* verbose flag only set for absolute pathname */
			uerror(path);
		return FALSE;
	}
	if (uid == 0)
		mask = X_ALL;
	else if (uid == stp->st_uid)
		mask = X_USR;
	else if (gid == stp->st_gid || ingidset(stp->st_gid))
		mask = X_GRP;
	else
		mask = X_OTH;
	if (((stp->st_mode & S_IFMT) == S_IFREG) && (stp->st_mode & mask))
		return TRUE;
	errno = EACCES;
	if (verbose)
		uerror(path);
	return FALSE;
}

/* replace non-printing characters with question marks in a freshly
 * allocated string */
static char *protect(char *in) {
	int l = strlen(in);
	char *out = ealloc(l + 1);
	int i;

	for (i = 0; i < l; ++i)
		out[i] = isprint(in[i]) ? in[i] : '?';
	out[i] = '\0';
	return out;
}

static char *join(char *path, char *cmd) {
	static char *buf = NULL;
	static size_t cap = 0;
	size_t need, end, pathlen, cmdlen;
	
	pathlen = strlen(path), cmdlen = strlen(cmd);
	need = pathlen + cmdlen + 2; /* one for null terminator, one for the '/' */
	if (cap < need) {
		if (need < 32) need = 32;
		buf = erealloc(buf, (cap = need));
	}
	if (*path == '\0') {
		memcpy(buf, cmd, cmdlen+1);
	} else {
		memcpy(buf, path, pathlen+1);
		end = pathlen - 1;
		if (buf[end] != '/' ) /* "//" is special to POSIX */
			buf[++end] = '/';
		memcpy(buf+end+1, cmd, cmdlen+1);
	}

	return buf;
}

/* return a full pathname by searching $path, and by checking the status of the file */

extern char *which(char *name, bool verbose) {
	List *path;
	char *cached, *full;
	struct stat st;

	if (name == NULL)	/* no filename? can happen with "> foo" as a command */
		return NULL;
	if (!initialized) {
		initialized = TRUE;
		uid = geteuid();
		gid = getegid();
#if HAVE_GETGROUPS
#if HAVE_POSIX_GETGROUPS
		ngroups = getgroups(0, (GETGROUPS_T *)0);
		if (ngroups < 0) {
			uerror("getgroups");
			rc_exit(1);
		}
#else
		ngroups = NGROUPS;
#endif
		if (ngroups) {	
			gidset = ecalloc((size_t)ngroups, sizeof(GETGROUPS_T));
			getgroups(ngroups, gidset);
		}
#endif
	}
	if (isabsolute(name)) /* absolute pathname? */
		return rc_access(name, verbose, &st) ? name : NULL;
	if ((cached = lookup_cmd(name)) != NULL) /* command has already been cached? */
		return join(cached, name);
	for (path = varlookup("path"); path != NULL; path = path->n) {
		full = join(path->w, name);
		if (rc_access(full, FALSE, &st)) {
			set_cmd_path(name, path->w); /* cache the path for this command */
			return full;
		}
	}
	if (verbose) {
		char *n = protect(name);
		fprint(2, RC "cannot find `%s'\n", n);
		efree(n);
	}
	return NULL;
}

/* Remove a command from the cache if it is no longer executable. */
extern void verify_cmd(char *fullpath) {
	struct stat st;
	char *cmd;

	if (rc_access(fullpath, FALSE, &st))
		return;
	
	cmd = strrchr(fullpath, '/');
	if (cmd != NULL && *++cmd != '\0')
		delete_cmd(cmd);
}
