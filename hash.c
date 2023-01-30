/* hash.c: hash table support for functions and variables. */

/*
   Functions and variables are cached in both internal and external
   form for performance. Thus a variable which is never "dereferenced"
   with a $ is passed on to rc's children untouched. This is not so
   important for variables, but is a big win for functions, where a call
   to yyparse() is involved.
*/

#include "rc.h"
#include "sigmsgs.h"

static bool var_exportable(char *);
static bool fn_exportable(char *);
static int hash(char *, int);
static int find(char *, Htab *, int);
static void free_fn(rc_Function *);

Htab *fp; /* functions */
Htab *vp; /* variables */
Htab *cp; /* commands in $path */
static int fused, fsize, vused, vsize, cused, csize;
static char **env;
static int bozosize;
static int envsize;
static bool env_dirty = TRUE;
static char *dead = "";

#define HASHSIZE 64 /* rc was debugged with HASHSIZE == 2; 64 is about right for normal use */

extern void inithash() {
	fp = ecalloc(HASHSIZE, sizeof(Htab));
	vp = ecalloc(HASHSIZE, sizeof(Htab));
	cp = ecalloc(HASHSIZE, sizeof(Htab));
	fused = vused = cused = 0;
	fsize = vsize = csize = HASHSIZE;
}

#define ADV()   {if ((c = *s++) == '\0') break;}

/* hash function courtesy of paul haahr */

static int hash(char *s, int size) {
	int c, n = 0;
	while (1) {
		ADV();
		n += (c << 17) ^ (c << 11) ^ (c << 5) ^ (c >> 1);
		ADV();
		n ^= (c << 14) + (c << 7) + (c << 4) + c;
		ADV();
		n ^= (~c << 11) | ((c << 3) ^ (c >> 1));
		ADV();
		n -= (c << 16) | (c << 9) | (c << 2) | (c & 3);
	}
	if (n < 0)
		n = ~n;
	return n & (size - 1); /* need power of 2 size */
}

static bool rehash(Htab *ht) {
	int i, j, size;
	int newsize, newused;
	Htab *newhtab;
	if (ht == fp) {
		if (fsize > 2 * fused)
			return FALSE;
		size = fsize;
	} else if (ht == vp) {
		if (vsize > 2 * vused)
			return FALSE;
		size = vsize;
	} else {
		if (csize > 2 * cused)
			return FALSE;
		size = csize;
	}
	newsize = 2 * size;
	newhtab = ecalloc(newsize, sizeof(Htab));
	for (i = 0; i < newsize; i++)
		newhtab[i].name = NULL;
	for (i = newused = 0; i < size; i++)
		if (ht[i].name != NULL && ht[i].name != dead) {
			newused++;
			j = hash(ht[i].name, newsize);
			while (newhtab[j].name != NULL) {
				j++;
				j &= (newsize - 1);
			}
			newhtab[j].name = ht[i].name;
			newhtab[j].p = ht[i].p;
		}
	if (ht == fp) {
		fused = newused;
		fp = newhtab;
		fsize = newsize;
	} else if (ht == vp) {
		vused = newused;
		vp = newhtab;
		vsize = newsize;
	} else {
		cused = newused;
		cp = newhtab;
		csize = newsize;
	}
	efree(ht);
	return TRUE;
}

#define varfind(s) find(s, vp, vsize)
#define fnfind(s) find(s, fp, fsize)
#define cmdfind(s) find(s, cp, csize)

static int find(char *s, Htab *ht, int size) {
	int h = hash(s, size);
	while (ht[h].name != NULL && !streq(ht[h].name, s)) {
		h++;
		h &= size - 1;
	}
	return h;
}

extern void *lookup(char *s, Htab *ht) {
	int h = find(s, ht, ht == fp ? fsize : ht == vp ? vsize : csize);
	return (ht[h].name == NULL) ? NULL : ht[h].p;
}

extern rc_Function *get_fn_place(char *s) {
	int h = fnfind(s);
	env_dirty = TRUE;
	if (fp[h].name == NULL) {
		if (rehash(fp))
			h = fnfind(s);
		fused++;
		fp[h].name = ecpy(s);
		fp[h].p = enew(rc_Function);
	} else
		free_fn(fp[h].p);
	return fp[h].p;
}

extern Variable *get_var_place(char *s, bool stack) {
	Variable *new;
	int h = varfind(s);

	env_dirty = TRUE;

	if (vp[h].name == NULL) {
		if (rehash(vp))
			h = varfind(s);
		vused++;
		vp[h].name = ecpy(s);
		vp[h].p = enew(Variable);
		((Variable *)vp[h].p)->n = NULL;
		return vp[h].p;
	} else {
		if (stack) {	/* increase the stack by 1 */
			new = enew(Variable);
			new->n = vp[h].p;
			return vp[h].p = new;
		} else {	/* trample the top of the stack */
			new = vp[h].p;
			efree(new->extdef);
			listfree(new->def);
			return new;
		}
	}
}

/* Upsert the path associated to a command. We do not make a copy of
   the path string, but simply copy its pointer. Because of this,
   the whole table must be reset when $path is modified. */
extern void set_cmd_path(char *cmd, char *path) {
	int h = cmdfind(cmd);
	if (fp[h].name == NULL) {
		if (rehash(cp))
			h = cmdfind(cmd);
		cused++;
		cp[h].name = ecpy(cmd);
	}
	cp[h].p = path;
}

extern void delete_fn(char *s) {
	int h = fnfind(s);
	if (fp[h].name == NULL)
		return; /* not found */
	env_dirty = TRUE;
	free_fn(fp[h].p);
	efree(fp[h].p);
	efree(fp[h].name);
	if (fp[(h+1)&(fsize-1)].name == NULL) {
		--fused;
		fp[h].name = NULL;
	} else {
		fp[h].name = dead;
	}
}

extern void delete_var(char *s, bool stack) {
	int h = varfind(s);
	Variable *v;
	if (vp[h].name == NULL)
		return; /* not found */
	env_dirty = TRUE;
	v = vp[h].p;
	efree(v->extdef);
	listfree(v->def);
	if (v->n != NULL) { /* This is the top of a stack */
		if (stack) { /* pop */
			vp[h].p = v->n;
			efree(v);
		} else { /* else just empty */
			v->extdef = NULL;
			v->def = NULL;
		}
	} else { /* needs to be removed from the hash table */
		efree(v);
		vp[h].p = NULL;
		efree(vp[h].name);
		if (vp[(h+1)&(vsize-1)].name == NULL) {
			--vused;
			vp[h].name = NULL;
		} else {
			vp[h].name = dead;
		}
	}
}

extern void delete_cmd(char *s) {
	int h = cmdfind(s);
	if (cp[h].name == NULL)
		return; /* not found */
	efree(cp[h].p);
	if (cp[(h+1)&(csize-1)].name == NULL) {
		--fused;
		cp[h].name = NULL;
	} else {
		cp[h].name = dead;
	}
}

extern void reset_cmdtab() {
	Htab *cpp;
	int i;

	if (cused == 0)
		return;

	for (cpp = cp, i = 0; i < csize; i++, cpp++) {
		efree(cpp->name);
		cpp->name = NULL;
	}
	
	cused = 0;
}

static void free_fn(rc_Function *f) {
	treefree(f->def);
	efree(f->extdef);
}

extern void initenv(char **envp) {
	int n;
	for (n = 0; envp[n] != NULL; n++)
		;
	n++; /* one for the null terminator */
	if (n < HASHSIZE)
		n = HASHSIZE;
	envsize = 2 * n;
	env = ecalloc(envsize, sizeof(char *));
	for (; *envp != NULL; envp++)
		if (strncmp(*envp, "fn_", conststrlen("fn_")) == 0) {
			if (!dashpee)
				fnassign_string(*envp);
		} else {
			if (!varassign_string(*envp)) /* add to bozo env */
				env[bozosize++] = *envp;
		}
}

/* for a few variables that have default values, we export them only
if they've been explicitly set; maybeexport[n].flag is TRUE if this
has occurred. */
struct nameflag {
	char *name;
	bool flag;
};
static struct nameflag maybeexport[] = {
	{ "prompt", FALSE },
	{ "version", FALSE }
};

void set_exportable(char *s, bool b) {
	int i;
	for (i = 0; i < arraysize(maybeexport); ++i)
		if (maybeexport[i].flag != b && streq(s, maybeexport[i].name))
			maybeexport[i].flag = b;
}

static bool var_exportable(char *s) {
	int i;
	List *noex;
	for (i = 0; i < arraysize(maybeexport); i++)
		if (maybeexport[i].flag == FALSE && streq(s, maybeexport[i].name))
			return FALSE;
	for (noex = varlookup("noexport"); noex != NULL; noex = noex->n)
		if (streq(s, noex->w))
			return FALSE;
	return TRUE;
}

static bool fn_exportable(char *s) {
	int i;
	if (strncmp(s, "sig", conststrlen("sig")) == 0) { /* small speed hack */
		for (i = 0; i < NUMOFSIGNALS; i++)
			if (streq(s, signals[i].name))
				return FALSE;
		if (streq(s, "sigexit"))
			return FALSE;
	}
	return TRUE;
}

extern char **makeenv() {
	int ep, i;
	char *v;
	if (!env_dirty)
		return env;
	env_dirty = FALSE;
	ep = bozosize;
	if (vsize + fsize + 1 + bozosize > envsize) {
		envsize = 2 * (bozosize + vsize + fsize + 1);
		env = erealloc(env, envsize * sizeof(char *));
	}
	for (i = 0; i < vsize; i++) {
		if (vp[i].name == NULL || vp[i].name == dead || !var_exportable(vp[i].name))
			continue;
		v = varlookup_string(vp[i].name);
		if (v != NULL)
			env[ep++] = v;
	}
	for (i = 0; i < fsize; i++) {
		if (fp[i].name == NULL || fp[i].name == dead || !fn_exportable(fp[i].name))
			continue;
		env[ep++] = fnlookup_string(fp[i].name);
	}
	env[ep] = NULL;
	qsort(env, (size_t) ep, sizeof(char *), starstrcmp);
	return env;
}

extern void whatare_all_vars(bool showfn, bool showvar) {
	int i;
	List *s;
	if (showvar)
		for (i = 0; i < vsize; i++)
			if (vp[i].name != NULL && (s = varlookup(vp[i].name)) != NULL)
				prettyprint_var(1, vp[i].name, s);
	if (showfn)
		for (i = 0; i < fsize; i++)
			if (fp[i].name != NULL && fp[i].name != dead)
				prettyprint_fn(1, fp[i].name, fnlookup(fp[i].name));
}

extern char *compl_name(const char *text, int state, char **p, size_t count, ssize_t inc) {
	static char **n;
	static size_t i, len;
	char *name;

	if (!state) {
		n = p;
		i = 0;
		len = strlen(text);
	}
	for (name = NULL; name == NULL && i < count; i++, n += inc)
		if (*n != NULL && strncmp(*n, text, len) == 0)
			name = strdup(*n);
	return name;
}

extern char *compl_fn(const char *text, int state) {
	return compl_name(text, state, &fp[0].name, fsize, &fp[1].name - &fp[0].name);
}

extern char *compl_var(const char *text, int state) {
	return compl_name(text, state, &vp[0].name, vsize, &vp[1].name - &vp[0].name);
}
