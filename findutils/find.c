/* vi: set sw=4 ts=4: */
/*
 * Mini find implementation for busybox
 *
 * Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 *
 * Reworked by David Douthitt <n9ubh@callsign.net> and
 *  Matt Kraai <kraai@alumni.carnegiemellon.edu>.
 *
 * Licensed under the GPL version 2, see the file LICENSE in this tarball.
 */

/* findutils-4.1.20:
 *
 * # find file.txt -exec 'echo {}' '{}  {}' ';'
 * find: echo file.txt: No such file or directory
 * # find file.txt -exec 'echo' '{}  {}' '; '
 * find: missing argument to `-exec'
 * # find file.txt -exec 'echo {}' '{}  {}' ';' junk
 * find: paths must precede expression
 * # find file.txt -exec 'echo {}' '{}  {}' ';' junk ';'
 * find: paths must precede expression
 * # find file.txt -exec 'echo' '{}  {}' ';'
 * file.txt  file.txt
 * (strace: execve("/bin/echo", ["echo", "file.txt  file.txt"], [ 30 vars ]))
 * # find file.txt -exec 'echo' '{}  {}' ';' -print -exec pwd ';'
 * file.txt  file.txt
 * file.txt
 * /tmp
 * # find -name '*.c' -o -name '*.h'
 * [shows files, *.c and *.h intermixed]
 * # find file.txt -name '*f*' -o -name '*t*'
 * file.txt
 * # find file.txt -name '*z*' -o -name '*t*'
 * file.txt
 * # find file.txt -name '*f*' -o -name '*z*'
 * file.txt
 *
 * # find t z -name '*t*' -print -o -name '*z*'
 * t
 * # find t z t z -name '*t*' -o -name '*z*' -print
 * z
 * z
 * # find t z t z '(' -name '*t*' -o -name '*z*' ')' -o -print
 * (no output)
 */

#include <fnmatch.h>
#include "busybox.h"

USE_FEATURE_FIND_XDEV(static dev_t *xdev_dev;)
USE_FEATURE_FIND_XDEV(static int xdev_count;)

typedef int (*action_fp)(const char *fileName, struct stat *statbuf, void *);

typedef struct {
	action_fp f;
#if ENABLE_FEATURE_FIND_NOT
	bool invert;
#endif
} action;
#define ACTS(name, arg...) typedef struct { action a; arg; } action_##name;
#define ACTF(name)         static int func_##name(const char *fileName, struct stat *statbuf, action_##name* ap)
                        ACTS(print)
                        ACTS(name,  const char *pattern;)
USE_FEATURE_FIND_PRINT0(ACTS(print0))
USE_FEATURE_FIND_TYPE(  ACTS(type,  int type_mask;))
USE_FEATURE_FIND_PERM(  ACTS(perm,  char perm_char; mode_t perm_mask;))
USE_FEATURE_FIND_MTIME( ACTS(mtime, char mtime_char; unsigned mtime_days;))
USE_FEATURE_FIND_MMIN(  ACTS(mmin,  char mmin_char; unsigned mmin_mins;))
USE_FEATURE_FIND_NEWER( ACTS(newer, time_t newer_mtime;))
USE_FEATURE_FIND_INUM(  ACTS(inum,  ino_t inode_num;))
USE_FEATURE_FIND_EXEC(  ACTS(exec,  char **exec_argv; unsigned int *subst_count; int exec_argc;))
USE_FEATURE_FIND_USER(  ACTS(user,  int uid;))
USE_DESKTOP(            ACTS(paren, action ***subexpr;))
USE_DESKTOP(            ACTS(size,  off_t size;))
USE_DESKTOP(            ACTS(prune))

static action ***actions;
static bool need_print = 1;


#if ENABLE_FEATURE_FIND_EXEC
static unsigned int count_subst(const char *str)
{
	unsigned int count = 0;
	while ((str = strstr(str, "{}"))) {
		count++;
		str++;
	}
	return count;
}


static char* subst(const char *src, unsigned int count, const char* filename)
{
	char *buf, *dst, *end;
	size_t flen = strlen(filename);
	/* we replace each '{}' with filename: growth by strlen-2 */
	buf = dst = xmalloc(strlen(src) + count*(flen-2) + 1);
	while ((end = strstr(src, "{}"))) {
		memcpy(dst, src, end - src);
		dst += end - src;
		src = end + 2;
		memcpy(dst, filename, flen);
		dst += flen;
	}
	strcpy(dst, src);
	return buf;
}
#endif


static int exec_actions(action ***appp, const char *fileName, struct stat *statbuf)
{
	int cur_group;
	int cur_action;
	int rc = TRUE;
	action **app, *ap;

	cur_group = -1;
	while ((app = appp[++cur_group])) {
		cur_action = -1;
		while (1) {
			ap = app[++cur_action];
			if (!ap) {
				/* all actions in group were successful */
				return rc;
			}
			rc = ap->f(fileName, statbuf, ap);
#if ENABLE_FEATURE_FIND_NOT
			if (ap->invert) rc = !rc;
#endif
			if (!rc) {
				/* current group failed, try next */
				break;
			}
		}
	}
	return rc;
}


ACTF(name)
{
	const char *tmp = strrchr(fileName, '/');
	if (tmp == NULL)
		tmp = fileName;
	else
		tmp++;
	return fnmatch(ap->pattern, tmp, FNM_PERIOD) == 0;
}
#if ENABLE_FEATURE_FIND_TYPE
ACTF(type)
{
	return ((statbuf->st_mode & S_IFMT) == ap->type_mask);
}
#endif
#if ENABLE_FEATURE_FIND_PERM
ACTF(perm)
{
	/* -perm +mode: at least one of perm_mask bits are set */
	if (ap->perm_char == '+')
		return (statbuf->st_mode & ap->perm_mask) != 0;
	/* -perm -mode: all of perm_mask are set */
	if (ap->perm_char == '-')
		return (statbuf->st_mode & ap->perm_mask) == ap->perm_mask;
	/* -perm mode: file mode must match perm_mask */
	return (statbuf->st_mode & 07777) == ap->perm_mask;
}
#endif
#if ENABLE_FEATURE_FIND_MTIME
ACTF(mtime)
{
	time_t file_age = time(NULL) - statbuf->st_mtime;
	time_t mtime_secs = ap->mtime_days * 24*60*60;
	if (ap->mtime_char == '+')
		return file_age >= mtime_secs + 24*60*60;
	if (ap->mtime_char == '-')
		return file_age < mtime_secs;
	/* just numeric mtime */
	return file_age >= mtime_secs && file_age < (mtime_secs + 24*60*60);
}
#endif
#if ENABLE_FEATURE_FIND_MMIN
ACTF(mmin)
{
	time_t file_age = time(NULL) - statbuf->st_mtime;
	time_t mmin_secs = ap->mmin_mins * 60;
	if (ap->mmin_char == '+')
		return file_age >= mmin_secs + 60;
	if (ap->mmin_char == '-')
		return file_age < mmin_secs;
	/* just numeric mmin */
	return file_age >= mmin_secs && file_age < (mmin_secs + 60);
}
#endif
#if ENABLE_FEATURE_FIND_NEWER
ACTF(newer)
{
	return (ap->newer_mtime < statbuf->st_mtime);
}
#endif
#if ENABLE_FEATURE_FIND_INUM
ACTF(inum)
{
	return (statbuf->st_ino == ap->inode_num);
}
#endif
#if ENABLE_FEATURE_FIND_EXEC
ACTF(exec)
{
	int i, rc;
	char *argv[ap->exec_argc+1];
	for (i = 0; i < ap->exec_argc; i++)
		argv[i] = subst(ap->exec_argv[i], ap->subst_count[i], fileName);
	argv[i] = NULL; /* terminate the list */
	rc = wait4pid(spawn(argv));
	if (rc)
		bb_perror_msg("%s", argv[0]);
	for (i = 0; i < ap->exec_argc; i++)
		free(argv[i]);
	return rc == 0; /* return 1 if success */
}
#endif

#if ENABLE_FEATURE_FIND_USER
ACTF(user)
{
	return (statbuf->st_uid == ap->uid);
}
#endif

#if ENABLE_FEATURE_FIND_PRINT0
ACTF(print0)
{
	printf("%s%c", fileName, '\0');
	return TRUE;
}
#endif

ACTF(print)
{
	puts(fileName);
	return TRUE;
}

#if ENABLE_DESKTOP
ACTF(paren)
{
	return exec_actions(ap->subexpr, fileName, statbuf);
}

/*
 * -prune: if -depth is not given, return true and do not descend
 * current dir; if -depth is given, return false with no effect.
 * Example:
 * find dir -name 'asm-*' -prune -o -name '*.[chS]' -print
 */
ACTF(prune)
{
	return SKIP;
}

ACTF(size)
{
	return statbuf->st_size == ap->size;
}
#endif


static int fileAction(const char *fileName, struct stat *statbuf, void* junk, int depth)
{
	int i;
#ifdef CONFIG_FEATURE_FIND_XDEV
	if (S_ISDIR(statbuf->st_mode) && xdev_count) {
		for (i = 0; i < xdev_count; i++) {
			if (xdev_dev[i] != statbuf->st_dev)
				return SKIP;
		}
	}
#endif
	i = exec_actions(actions, fileName, statbuf);
	/* Had no explicit -print[0] or -exec? then print */
	if (i && need_print)
		puts(fileName);
	/* Cannot return 0: our caller, recursive_action(),
	 * will perror() and skip dirs (if called on dir) */
	return i == 0 ? TRUE : i;
}


#if ENABLE_FEATURE_FIND_TYPE
static int find_type(const char *type)
{
	int mask = 0;

	if (*type == 'b')
		mask = S_IFBLK;
	else if (*type == 'c')
		mask = S_IFCHR;
	else if (*type == 'd')
		mask = S_IFDIR;
	else if (*type == 'p')
		mask = S_IFIFO;
	else if (*type == 'f')
		mask = S_IFREG;
	else if (*type == 'l')
		mask = S_IFLNK;
	else if (*type == 's')
		mask = S_IFSOCK;

	if (mask == 0 || *(type + 1) != '\0')
		bb_error_msg_and_die(bb_msg_invalid_arg, type, "-type");

	return mask;
}
#endif

#if ENABLE_FEATURE_FIND_PERM || ENABLE_FEATURE_FIND_MTIME \
 || ENABLE_FEATURE_FIND_MMIN
static const char* plus_minus_num(const char* str)
{
	if (*str == '-' || *str == '+')
		str++;
	return str;
}
#endif

#define PARM_a 0
#define PARM_o 1
#define PARM_char_not (PARM_o + ENABLE_FEATURE_FIND_NOT)
#define PARM_print (PARM_char_not + 1)
#define PARM_print0 (PARM_print + ENABLE_FEATURE_FIND_PRINT0)
#define PARM_name (PARM_print0 + 1)
#define PARM_type (PARM_name + ENABLE_FEATURE_FIND_TYPE)
#define PARM_perm (PARM_type + ENABLE_FEATURE_FIND_PERM)
#define PARM_mtime (PARM_perm + ENABLE_FEATURE_FIND_MTIME)
#define PARM_mmin (PARM_mtime + ENABLE_FEATURE_FIND_MMIN)
#define PARM_newer (PARM_mmin + ENABLE_FEATURE_FIND_NEWER)
#define PARM_inum (PARM_newer + ENABLE_FEATURE_FIND_INUM)
#define PARM_exec (PARM_inum + ENABLE_FEATURE_FIND_EXEC)
#define PARM_user (PARM_exec + ENABLE_FEATURE_FIND_USER)
#if ENABLE_DESKTOP
#define PARM_and (PARM_user + 1)
#define PARM_or (PARM_and + 1)
#define PARM_not (PARM_or + ENABLE_FEATURE_FIND_NOT)
#define PARM_char_brace (PARM_not + 1)
#define PARM_prune (PARM_char_brace + 1)
#define PARM_size (PARM_prune + 1)
#endif
static action*** parse_params(char **argv)
{
	action*** appp;
	unsigned cur_group = 0;
	unsigned cur_action = 0;
	USE_FEATURE_FIND_NOT( bool invert_flag = 0; )
	const char * const params[] = {
		"-a",
		"-o",
#if ENABLE_FEATURE_FIND_NOT
		"!",
#endif
		"-print",
#if ENABLE_FEATURE_FIND_PRINT0
		"-print0",
#endif
		"-name",
#if ENABLE_FEATURE_FIND_TYPE
		"-type",
#endif
#if ENABLE_FEATURE_FIND_PERM
		"-perm",
#endif
#if ENABLE_FEATURE_FIND_MTIME
		"-mtime",
#endif
#if ENABLE_FEATURE_FIND_MMIN
		"-mmin",
#endif
#if ENABLE_FEATURE_FIND_NEWER
		"-newer",
#endif
#if ENABLE_FEATURE_FIND_INUM
		"-inum",
#endif
#if ENABLE_FEATURE_FIND_EXEC
		"-exec",
#endif
#if ENABLE_FEATURE_FIND_USER
		"-user",
#endif
#if ENABLE_DESKTOP
		"-and",
		"-or",
#	if ENABLE_FEATURE_FIND_NOT
		"-not",
#	endif
		"(",
		"-prune",
		"-size",
#endif
		NULL
	};
	action* alloc_action(int sizeof_struct, action_fp f)
	{
		action *ap;
		appp[cur_group] = xrealloc(appp[cur_group], (cur_action+2) * sizeof(*appp));
		appp[cur_group][cur_action++] = ap = xmalloc(sizeof_struct);
		appp[cur_group][cur_action] = NULL;
		ap->f = f;
		USE_FEATURE_FIND_NOT( ap->invert = invert_flag; )
		USE_FEATURE_FIND_NOT( invert_flag = 0; )
		return ap;
	}
#define ALLOC_ACTION(name) (action_##name*)alloc_action(sizeof(action_##name), (action_fp) func_##name)

	appp = xzalloc(2 * sizeof(appp[0])); /* appp[0],[1] == NULL */

/* Actions have side effects and return a true or false value
 * We implement: -print, -print0, -exec
 *
 * The rest are tests.
 *
 * Tests and actions are grouped by operators
 * ( expr )              Force precedence
 * ! expr                True if expr is false
 * -not expr             Same as ! expr
 * expr1 [-a[nd]] expr2  And; expr2 is not evaluated if expr1 is false
 * expr1 -o[r] expr2     Or; expr2 is not evaluated if expr1 is true
 * expr1 , expr2         List; both expr1 and expr2 are always evaluated
 * We implement: (), -a, -o
 */
	while (*argv) {
		const char *arg = argv[0];
		const char *arg1 = argv[1];
		int parm = index_in_str_array(params, arg);
	/* --- Operators --- */
		if (parm == PARM_a USE_DESKTOP(|| parm == PARM_and))
		{
			/* no further special handling required */
		}
		else if (parm == PARM_o USE_DESKTOP(|| parm == PARM_or))
		{
			/* start new OR group */
			cur_group++;
			appp = xrealloc(appp, (cur_group+2) * sizeof(*appp));
			/*appp[cur_group] = NULL; - already NULL */
			appp[cur_group+1] = NULL;
			cur_action = 0;
		}
#if ENABLE_FEATURE_FIND_NOT
		else if (parm == PARM_char_not USE_DESKTOP(|| parm == PARM_not))
		{
			/* also handles "find ! ! -name 'foo*'" */
			invert_flag ^= 1;
		}
#endif

	/* --- Tests and actions --- */
		else if (parm == PARM_print)
		{
			need_print = 0;
			/* GNU find ignores '!' here: "find ! -print" */
			USE_FEATURE_FIND_NOT( invert_flag = 0; )
			(void) ALLOC_ACTION(print);
		}
#if ENABLE_FEATURE_FIND_PRINT0
		else if (parm == PARM_print0)
		{
			need_print = 0;
			USE_FEATURE_FIND_NOT( invert_flag = 0; )
			(void) ALLOC_ACTION(print0);
		}
#endif
		else if (parm == PARM_name)
		{
			action_name *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(name);
			ap->pattern = arg1;
		}
#if ENABLE_FEATURE_FIND_TYPE
		else if (parm == PARM_type)
		{
			action_type *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(type);
			ap->type_mask = find_type(arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_PERM
/* TODO:
 * -perm mode   File's permission bits are exactly mode (octal or symbolic).
 *              Symbolic modes use mode 0 as a point of departure.
 * -perm -mode  All of the permission bits mode are set for the file.
 * -perm +mode  Any of the permission bits mode are set for the file.
 */
		else if (parm == PARM_perm)
		{
			action_perm *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(perm);
			ap->perm_char = arg1[0];
			arg1 = plus_minus_num(arg1);
			ap->perm_mask = 0;
			if (!bb_parse_mode(arg1, &ap->perm_mask))
				bb_error_msg_and_die("invalid mode: %s", arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_MTIME
		else if (parm == PARM_mtime)
		{
			action_mtime *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(mtime);
			ap->mtime_char = arg1[0];
			ap->mtime_days = xatoul(plus_minus_num(arg1));
		}
#endif
#if ENABLE_FEATURE_FIND_MMIN
		else if (parm == PARM_mmin)
		{
			action_mmin *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(mmin);
			ap->mmin_char = arg1[0];
			ap->mmin_mins = xatoul(plus_minus_num(arg1));
		}
#endif
#if ENABLE_FEATURE_FIND_NEWER
		else if (parm == PARM_newer)
		{
			action_newer *ap;
			struct stat stat_newer;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			xstat(arg1, &stat_newer);
			ap = ALLOC_ACTION(newer);
			ap->newer_mtime = stat_newer.st_mtime;
		}
#endif
#if ENABLE_FEATURE_FIND_INUM
		else if (parm == PARM_inum)
		{
			action_inum *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(inum);
			ap->inode_num = xatoul(arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_EXEC
		else if (parm == PARM_exec)
		{
			int i;
			action_exec *ap;
			need_print = 0;
			USE_FEATURE_FIND_NOT( invert_flag = 0; )
			ap = ALLOC_ACTION(exec);
			ap->exec_argv = ++argv; /* first arg after -exec */
			ap->exec_argc = 0;
			while (1) {
				if (!*argv) /* did not see ';' util end */
					bb_error_msg_and_die(bb_msg_requires_arg, arg);
				if (LONE_CHAR(argv[0], ';'))
					break;
				argv++;
				ap->exec_argc++;
			}
			if (ap->exec_argc == 0)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap->subst_count = xmalloc(ap->exec_argc * sizeof(int));
			i = ap->exec_argc;
			while (i--)
				ap->subst_count[i] = count_subst(ap->exec_argv[i]);
		}
#endif
#if ENABLE_FEATURE_FIND_USER
		else if (parm == PARM_user)
		{
			action_user *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(user);
			ap->uid = bb_strtou(arg1, NULL, 10);
			if (errno)
				ap->uid = xuname2uid(arg1);
		}
#endif
#if ENABLE_DESKTOP
		else if (parm == PARM_char_brace)
		{
			action_paren *ap;
			char **endarg;
			unsigned nested = 1;

			endarg = argv;
			while (1) {
				if (!*++endarg)
					bb_error_msg_and_die("unpaired '('");
				if (LONE_CHAR(*endarg, '('))
					nested++;
				else if (LONE_CHAR(*endarg, ')') && !--nested) {
					*endarg = NULL;
					break;
				}
			}
			ap = ALLOC_ACTION(paren);
			ap->subexpr = parse_params(argv + 1);
			*endarg = (char*) ")"; /* restore NULLed parameter */
			argv = endarg;
		}
		else if (parm == PARM_prune)
		{
			USE_FEATURE_FIND_NOT( invert_flag = 0; )
			(void) ALLOC_ACTION(prune);
		}
		else if (parm == PARM_size)
		{
			action_size *ap;
			if (!*++argv)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap = ALLOC_ACTION(size);
			ap->size = XATOOFF(arg1);
		}
#endif
		else
			bb_show_usage();
		argv++;
	}
	return appp;
#undef ALLOC_ACTION
}


int find_main(int argc, char **argv);
int find_main(int argc, char **argv)
{
	bool dereference = FALSE;
	char *arg;
	char **argp;
	int i, firstopt, status = EXIT_SUCCESS;
	const char * const options[] = {
		"-follow",
USE_FEATURE_FIND_XDEV( "-xdev", )
		NULL
	};

	for (firstopt = 1; firstopt < argc; firstopt++) {
		if (argv[firstopt][0] == '-')
			break;
		if (ENABLE_FEATURE_FIND_NOT && LONE_CHAR(argv[firstopt], '!'))
			break;
#if ENABLE_DESKTOP
		if (LONE_CHAR(argv[firstopt], '('))
			break;
#endif
	}
	if (firstopt == 1) {
		argv[0] = (char*)".";
		argv--;
		firstopt++;
	}

/* All options always return true. They always take effect
 * rather than being processed only when their place in the
 * expression is reached.
 * We implement: -follow, -xdev
 */
	/* Process options, and replace then with -a */
	/* (-a will be ignored by recursive parser later) */
	argp = &argv[firstopt];
	while ((arg = argp[0])) {
		i = index_in_str_array(options, arg);
		if (i == 0) { /* -follow */
			dereference = TRUE;
			argp[0] = (char*)"-a";
		}
#if ENABLE_FEATURE_FIND_XDEV
		else if (i == 1) { /* -xdev */
			struct stat stbuf;
			if (!xdev_count) {
				xdev_count = firstopt - 1;
				xdev_dev = xmalloc(xdev_count * sizeof(dev_t));
				for (i = 1; i < firstopt; i++) {
					/* not xstat(): shouldn't bomb out on
					 * "find not_exist exist -xdev" */
					if (stat(argv[i], &stbuf))
						stbuf.st_dev = -1L;
					xdev_dev[i-1] = stbuf.st_dev;
				}
			}
			argp[0] = (char*)"-a";
		}
#endif
		argp++;
	}

	actions = parse_params(&argv[firstopt]);

	for (i = 1; i < firstopt; i++) {
		if (!recursive_action(argv[i],
				action_recurse|(1<<dereference), /* flags */
				fileAction,     /* file action */
				fileAction,     /* dir action */
				NULL,           /* user data */
				0))             /* depth */
			status = EXIT_FAILURE;
	}
	return status;
}
