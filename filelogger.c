/* filelogger v1.0 */

/* Copyright 2013 Yahoo! Inc.
 * This file is free software; you can redistribute it and/or modify it under 
 * the terms of the GNU General Public License (GPL), version 2 or later. This 
 * library is distributed WITHOUT ANY WARRANTY, whether express or implied. 
 * See the GNU GPL for more details (http://www.gnu.org/licenses/gpl.html)
 */
 
 /* Authors: Michael Martinez and ohers, see below for the list */
 
 /* COPYRIGHT NOTICE FROM TAIL.C used in this code below:
 *  Copyright (C) 1989-1991, 1995-2006, 2008-2011 Free Software Foundation, Inc.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.  
 *
 * COPYRIGHT NOTICE FROM LOGGER.C used in this code below:
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


/*To do:
 * get rid of the compile warnings
 * depending on how a file is truncated or rotated, it does not capture the text
 * or at least the first line that was written as part of the
 * truncation, eg echo test > file. it also does not detect
 * every truncation
 *
/* This combines logger.c from util-linux-2.20.1
 * and tail.c from core-utils-8.9
 * into a program that continuously tails one or 
 * more files and logs each line to local or 
 * remote syslog.
 */

/* tail: all original command-line options removed
 * except q, v, and s. tail -F is assumed.
 * stdin is not allowed. must specify a file
 * by name.
 */

/* logger: an additional command line option 
 * --add which allows additional text to be 
 *  inserted at the beginning of the line before
 *  sent to the syslog server. 
 *  The previous hardcoded 400 character limit 
 *  for the log message has been increased to 8096.
 */


#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <getopt.h>

#include "c.h"
#include "nls.h"
#include "strutils.h"

#define	SYSLOG_NAMES
#include <syslog.h>

/* from tail.c */
#include "xstrtol.h"
#include "xstrtod.h"
#include "c-strtod.h"
#include "binary-io.h"
#include "isapipe.h"
#include "quotearg.h"
#include "quote.h"
#include <stddef.h>
#include "stat-time.h"
#include "safe-read.h"
#include <assert.h>
#include "xnanosleep.h"
#include "time.h"

#if HAVE_INOTIFY
# include "hash.h"
# include <sys/inotify.h>
/* `select' is used by tail_forever_inotify.  */
# include <sys/select.h>

/* inotify needs to know if a file is local.  */
# include "fs.h"
# if HAVE_SYS_STATFS_H
#  include <sys/statfs.h>
# elif HAVE_SYS_VFS_H
#  include <sys/vfs.h>
# endif
#endif

/* The official name of this program (e.g., no `g' prefix).  */
#define PROGRAM_NAME "filelogger"

#define AUTHORS \
  proper_name ("Paul Rubin"), \
  proper_name ("David MacKenzie"), \
  proper_name ("Ian Lance Taylor"), \
  proper_name ("Jim Meyering"), \
  proper_name ("Michael Martinez")

/* Number of items to tail.  */
#define DEFAULT_N_LINES 0
#define STREQ(s1, s2) (strcmp (s1, s2) == 0)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define ATTRIBUTE_NORETURN __attribute__ ((__noreturn__))
# define xalloc_oversized(n, s) \
    ((size_t) (sizeof (ptrdiff_t) <= sizeof (size_t) ? -1 : -2) / (s) < (n))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* True if the arithmetic type T is signed.  */
# define TYPE_SIGNED(t) (! ((t) 0 < (t) -1))

# if __GNUC__ >= 2
#  define signed_type_or_expr__(t) TYPE_SIGNED (__typeof__ (t))
# else
#  define signed_type_or_expr__(t) 1
# endif

/* Bound on length of the string representing an unsigned integer
 *    value representable in B bits.  log10 (2.0) < 146/485.  The
 *       smallest value of B where this bound is not tight is 2621.  */
# define INT_BITS_STRLEN_BOUND(b) (((b) * 146 + 484) / 485)

/* Bound on length of the string representing an integer type or expression T.
 *    Subtract 1 for the sign bit if T is signed, and then add 1 more for
 *       a minus sign if needed.  */
# define INT_STRLEN_BOUND(t) \
  (INT_BITS_STRLEN_BOUND (sizeof (t) * CHAR_BIT - signed_type_or_expr__ (t)) \
   + signed_type_or_expr__ (t))

/* Bound on buffer size needed to represent an integer type or expression T,
 *    including the terminating null.  */
# define INT_BUFSIZE_BOUND(t) (INT_STRLEN_BOUND (t) + 1)


extern void xalloc_die (void) ATTRIBUTE_NORETURN;

static inline void * xnmalloc (size_t n, size_t s)
{
  if (xalloc_oversized (n, s))
    xalloc_die ();
  return xmalloc (n * s);
}

static inline int
timespec_cmp (struct timespec a, struct timespec b)
{
  return (a.tv_sec < b.tv_sec ? -1
          : a.tv_sec > b.tv_sec ? 1
          : (int) (a.tv_nsec - b.tv_nsec));
}


/* Special values for dump_remainder's N_BYTES parameter.  */
#define COPY_TO_EOF UINTMAX_MAX
#define COPY_A_BUFFER (UINTMAX_MAX - 1)

/* FIXME: make Follow_name the default?  */
#define DEFAULT_FOLLOW_MODE Follow_descriptor

enum Follow_mode
{
  /* Follow the name of each file: if the file is renamed, try to reopen
     that name and track the end of the new file if/when it's recreated.
     This is useful for tracking logs that are occasionally rotated.  */
  Follow_name = 1,

  /* Follow each descriptor obtained upon opening a file.
     That means we'll continue to follow the end of a file even after
     it has been renamed or unlinked.  */
  Follow_descriptor = 2
};

/* The types of files for which tail works.  */
#define IS_TAILABLE_FILE_TYPE(Mode) \
  (S_ISREG (Mode) || S_ISFIFO (Mode) || S_ISSOCK (Mode) || S_ISCHR (Mode))

static char const *const follow_mode_string[] =
{
  "descriptor", "name", NULL
};

static enum Follow_mode const follow_mode_map[] =
{
  Follow_descriptor, Follow_name,
};

struct File_spec
{
  /* The actual file name, or "-" for stdin.  */
  char *name;

  /* Attributes of the file the last time we checked.  */
  off_t size;
  struct timespec mtime;
  dev_t dev;
  ino_t ino;
  mode_t mode;

  /* The specified name initially referred to a directory or some other
     type for which tail isn't meaningful.  Unlike for a permission problem
     (tailable, below) once this is set, the name is not checked ever again.  */
  bool ignore;

  /* See the description of fremote.  */
  bool remote;

  /* A file is tailable if it exists, is readable, and is of type
     IS_TAILABLE_FILE_TYPE.  */
  bool tailable;

  /* File descriptor on which the file is open; -1 if it's not open.  */
  int fd;

  /* The value of errno seen last time we checked this file.  */
  int errnum;

  /* 1 if O_NONBLOCK is clear, 0 if set, -1 if not known.  */
  int blocking;

#if HAVE_INOTIFY
  /* The watch descriptor used by inotify.  */
  int wd;

  /* The parent directory watch descriptor.  It is used only
   * when Follow_name is used.  */
  int parent_wd;

  /* Offset in NAME of the basename part.  */
  size_t basename_start;
#endif

  /* See description of DEFAULT_MAX_N_... below.  */
  uintmax_t n_unchanged_stats;
};

#if HAVE_INOTIFY
/* The events mask used with inotify on files.  This mask is not used on
   directories.  */
const uint32_t inotify_wd_mask = (IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF
                                  | IN_MOVE_SELF);
#endif

static bool reopen_inaccessible_files = true;

/* Always assume reading via lines */
static bool count_lines;

/* Whether we follow the name of each file or the file descriptor
   that is initially associated with each name.  */
static enum Follow_mode follow_mode = Follow_descriptor;

/* If true, read from the ends of all specified files until killed.  */
static bool forever;

/* If true, print filename headers.  */
static bool print_headers;

/* When to print the filename banners.  */
enum header_mode
{
  multiple_files, always, never
};

/* When tailing a file by name, if there have been this many consecutive
   iterations for which the file has not changed, then open/fstat
  parse_options (argc, argv, &n_units, &header_mode, &sleep_interval);
   the file to determine if that file name is still associated with the
   same device/inode-number pair as before.  This option is meaningful only
   when following by name.  --max-unchanged-stats=N  */
#define DEFAULT_MAX_N_UNCHANGED_STATS_BETWEEN_OPENS 5
static uintmax_t max_n_unchanged_stats_between_opens =
  DEFAULT_MAX_N_UNCHANGED_STATS_BETWEEN_OPENS;

/* The process ID of the process (presumably on the current host)
   that is writing to all followed files.  */
static pid_t pid;

/* True if we have ever read standard input.  */
static bool have_read_stdin;

/* If nonzero then don't use inotify even if available.  */
static bool disable_inotify;

/* ** END from tail.c ** */

int	decode __P((char *, CODE *));
int	pencode __P((char *));

static int optd = 0;
static int udpport = 514;
static int LogSock = -1;
static int logflags = 0;
static int pri = LOG_NOTICE;
static char *tag = NULL;
static char *add  = NULL;
static char *usock = NULL;
static char logthis[4096];
static int logthis_offset = 0; // where to begin writing in logthis[]

static int
myopenlog(const char *sock) {
       int fd;
       static struct sockaddr_un s_addr; /* AF_UNIX address of local logger */

       if (strlen(sock) >= sizeof(s_addr.sun_path))
	       errx(EXIT_FAILURE, _("openlog %s: pathname too long"), sock);

       s_addr.sun_family = AF_UNIX;
       (void)strcpy(s_addr.sun_path, sock);

       if ((fd = socket(AF_UNIX, optd ? SOCK_DGRAM : SOCK_STREAM, 0)) == -1)
	       err(EXIT_FAILURE, _("socket %s"), sock);

       if (connect(fd, (struct sockaddr *) &s_addr, sizeof(s_addr)) == -1)
	       err(EXIT_FAILURE, _("connect %s"), sock);

       return fd;
}

static int
udpopenlog(const char *servername,int port) {
	int fd;
	struct sockaddr_in s_addr;
	struct hostent *serverhost;

	if ((serverhost = gethostbyname(servername)) == NULL )
		errx(EXIT_FAILURE, _("unable to resolve '%s'"), servername);

	if ((fd = socket(AF_INET, SOCK_DGRAM , 0)) == -1)
		err(EXIT_FAILURE, _("socket"));

	bcopy(serverhost->h_addr,&s_addr.sin_addr,serverhost->h_length);
        s_addr.sin_family=AF_INET;
        s_addr.sin_port=htons(port);

        if (connect(fd, (struct sockaddr *) &s_addr, sizeof(s_addr)) == -1)
		err(EXIT_FAILURE, _("connect"));

	return fd;
}

/* these used to be passed as paramters: LogSock, logflags, pri, tag, add, logthis
 * but now are global variables
 */

/* write add + logthis to Logsock with logflags, pri, tag 
 * and then memset logthis and its counter */
static void
mysyslog() {
       char buf[4096], pid[30], *cp, *pc, *tp;
       time_t now;

/* to do: check and print file header if needed. 
 * this is probably not the appropriate function to do it in */

       if (LogSock > -1) {
               if (logflags & LOG_PID)
                       snprintf (pid, sizeof(pid), "[%d]", getpid());
	       else
		       pid[0] = 0;
               if (tag)
		       cp = tag;
	       else {
		       cp = getlogin();
		       if (!cp)
			       cp = "<someone>";
	       }
               (void)time(&now);
	       tp = ctime(&now)+4;

               if (add)
		       pc = add;
	       else {
		       pc = "";
	       }
               snprintf(buf, sizeof(buf), "<%d>%.15s %.200s%s: %.30s %.4096s",
			pri, tp, cp, pid, pc, logthis);

               if (write(LogSock, buf, strlen(buf)+1) < 0) 
		{
			
		  error (errno, errno, _("mysyslog(): error writing to socket. \
			premature exit.\n"));
		  exit (errno); /* to do: terminate program or just return ? */
		} else {
		  memset(logthis,'\0',4096);
		  logthis_offset = 0;
		}

       }
}

/* TAIL routines */

/* Call lseek with the specified arguments, where file descriptor FD
   corresponds to the file, FILENAME.
   Give a diagnostic and exit nonzero if lseek fails.
   Otherwise, return the resulting offset.  */

static off_t
xlseek (int fd, off_t offset, int whence, char const *filename)
{
  off_t new_offset = lseek (fd, offset, whence);
  char buf[INT_BUFSIZE_BOUND (offset)];
  char *s;

  if (0 <= new_offset)
    return new_offset;

  s = offtostr (offset, buf);
  switch (whence)
    {
    case SEEK_SET:
      error (0, errno, _("%s: cannot seek to offset %s"),
             filename, s);
      break;
    case SEEK_CUR:
      error (0, errno, _("%s: cannot seek to relative offset %s"),
             filename, s);
      break;
    case SEEK_END:
      error (0, errno, _("%s: cannot seek to end-relative offset %s"),
             filename, s);
      break;
    default:
      abort ();
    }

  exit (EXIT_FAILURE);
}

/* Record a file F with descriptor FD, size SIZE, status ST, and
   blocking status BLOCKING.  */

static void
record_open_fd (struct File_spec *f, int fd,
                off_t size, struct stat const *st,
                int blocking)
{
  f->fd = fd;
  f->size = size;
  f->mtime = get_stat_mtime (st);
  f->dev = st->st_dev;
  f->ino = st->st_ino;
  f->mode = st->st_mode;
  f->blocking = blocking;
  f->n_unchanged_stats = 0;
  f->ignore = false;
}

/* Close the file with descriptor FD and name FILENAME.  */

static void
close_fd (int fd, const char *filename)
{
  if (fd != -1 && fd != STDIN_FILENO && close (fd))
    {
      error (0, errno, _("closing %s (fd=%d)"), filename, fd);
    }
}

static bool
valid_file_spec (struct File_spec const *f)
{
  /* Exactly one of the following subexpressions must be true. */
  return ((f->fd == -1) ^ (f->errnum == 0));
}

static char const *
pretty_name (struct File_spec const *f)
{
  return ( (strcmp (f->name, "-") == 0) ? _("standard input") : f->name);
//  return (STREQ (f->name, "-") ? _("standard input") : f->name);
}

static void
xwrite_logger (char const *buffer, size_t n_bytes)
{
  int i;

  for (i = 1; i <= n_bytes; i++)  
  {
    if (logthis_offset < 4096)  {
	logthis[logthis_offset] = *buffer++;
	logthis_offset++;
    }

    if (logthis[logthis_offset - 1] == '\0')
    {
	logthis[logthis_offset - 1] = '\n';
    }

    if (logthis[logthis_offset - 1] == '\n' || logthis_offset >= 4096) {

       if (!usock) {
          syslog(pri, "%s", logthis);
       } else {
	mysyslog();
       }
    }

  } //end for

}

#if HAVE_INOTIFY
/* Without inotify support, always return false.  Otherwise, return false
   when FD is open on a file known to reside on a local file system.
   If fstatfs fails, give a diagnostic and return true.
   If fstatfs cannot be called, return true.  */
static bool
fremote (int fd, const char *name)
{
  bool remote = true;           /* be conservative (poll by default).  */

# if HAVE_FSTATFS && HAVE_STRUCT_STATFS_F_TYPE && defined __linux__
  struct statfs buf;
  int err = fstatfs (fd, &buf);
  if (err != 0)
    {
      error (0, errno, _("cannot determine location of %s. "
                         "reverting to polling"), quote (name));
    }
  else
    {
      switch (buf.f_type)
        {
        case S_MAGIC_AFS:
        case S_MAGIC_CIFS:
        case S_MAGIC_CODA:
        case S_MAGIC_FUSEBLK:
        case S_MAGIC_FUSECTL:
        case S_MAGIC_GFS:
        case S_MAGIC_KAFS:
        case S_MAGIC_LUSTRE:
        case S_MAGIC_NCP:
        case S_MAGIC_NFS:
        case S_MAGIC_NFSD:
        case S_MAGIC_OCFS2:
        case S_MAGIC_SMB:
          break;
        default:
          remote = false;
        }
    }
# endif

  return remote;
}
#else
/* Without inotify support, whether a file is remote is irrelevant.
   Always return "false" in that case.  */
# define fremote(fd, name) false
#endif

/* Print the last N_LINES lines from the end of file FD.
   Go backward through the file, reading `BUFSIZ' bytes at a time (except
   probably the first), until we hit the start of the file or have
   read NUMBER newlines.
   START_POS is the starting position of the read pointer for the file
   associated with FD (may be nonzero).
   END_POS is the file offset of EOF (one larger than offset of last byte).
   Return true if successful.  */

static bool
file_lines (const char *pretty_filename, int fd, uintmax_t n_lines,
            off_t start_pos, off_t end_pos, uintmax_t *read_pos)
{
  char buffer[BUFSIZ];
  size_t bytes_read;
  off_t pos = end_pos;

/* for purpose of this program n_lines is always 0 */
    return true;
}

/* Write the last N_LINES lines of file FILENAME open for reading in FD.
   Return true if successful.  */

static bool
tail_lines (const char *pretty_filename, int fd, uintmax_t n_lines,
            uintmax_t *read_pos)
{
  struct stat stats;

  if (fstat (fd, &stats))
   {
      error (0, errno, _("cannot fstat %s"), quote (pretty_filename));
      return false;
   }

  off_t start_pos = -1;
  off_t end_pos;

      /* Use file_lines only if FD refers to a regular file for
 *          which lseek (... SEEK_END) works.  */
      if ( S_ISREG (stats.st_mode)
           && (start_pos = lseek (fd, 0, SEEK_CUR)) != -1
           && start_pos < (end_pos = lseek (fd, 0, SEEK_END)))
        {
          *read_pos = end_pos;
          if (end_pos != 0
              && ! file_lines (pretty_filename, fd, n_lines,
                               start_pos, end_pos, read_pos))
            return false;
        }
      else
        {
	  error (errno, errno, _("tail_lines(): unable to lseek. potential pipe\n"));
          exit (errno); /* to do: terminate program or just return ? */
        }

  return true;
}

static bool
tail (const char *filename, int fd, uintmax_t n_units,
      uintmax_t *read_pos)
{
  *read_pos = 0;
   return tail_lines (filename, fd, n_units, read_pos);
}

/* Write the last N_UNITS units of the file described by F.
   Return true if successful.  */

static bool
tail_file (struct File_spec *f, uintmax_t n_units)
{
  int fd;
  bool ok;

/* Exit if file is stdin */
  bool is_stdin = (STREQ (f->name, "-"));
  if (is_stdin) {
    error (EXIT_FAILURE, errno, _("Please specify a real file, not stdin."));
    exit(EXIT_FAILURE);
  }

  fd = open (f->name, O_RDONLY | O_BINARY);

  f->tailable = !(reopen_inaccessible_files && fd == -1);

  if (fd == -1)
    {
      if (forever)
        {
          f->fd = -1;
          f->errnum = errno;
          f->ignore = false;
          f->ino = 0;
          f->dev = 0;
        }
      error (0, errno, _("cannot open %s for reading"),
             quote (pretty_name (f)));
      ok = false;
    }
  else
    {
      uintmax_t read_pos;
	ok = tail (pretty_name (f), fd, n_units, &read_pos);
      if (forever)
        {
          struct stat stats;

#if TEST_RACE_BETWEEN_FINAL_READ_AND_INITIAL_FSTAT
          xnanosleep (1);
#endif
          f->errnum = ok - 1;
          if (fstat (fd, &stats) < 0)
            {
              ok = false;
              f->errnum = errno;
              error (0, errno, _("error reading %s"), quote (pretty_name (f)));
            }
          else if (!IS_TAILABLE_FILE_TYPE (stats.st_mode))
            {
              error (0, 0, _("%s: cannot follow end of this type of file;\
 giving up on this name"),
                     pretty_name (f));
              ok = false;
              f->errnum = -1;
              f->ignore = true;
            }

          if (!ok)
            {
              close_fd (fd, pretty_name (f));
              f->fd = -1;
            }
          else
            {
              record_open_fd (f, fd, read_pos, &stats, (is_stdin ? -1 : 1));
              f->remote = fremote (fd, pretty_name (f));
            }
        }
      else
        {
          if (!is_stdin && close (fd))
            {
              error (0, errno, _("error reading %s"), quote (pretty_name (f)));
              ok = false;
            }
        }
    }

  return ok;
}

/* Mark as '.ignore'd each member of F that corresponds to a
   pipe or fifo, and return the number of non-ignored members.  */
static size_t
ignore_fifo_and_pipe (struct File_spec *f, size_t n_files)
{
  /* When there is no FILE operand and stdin is a pipe or FIFO
     POSIX requires that tail ignore the -f option.
     Since we allow multiple FILE operands, we extend that to say: with -f,
     ignore any "-" operand that corresponds to a pipe or FIFO.  */
  size_t n_viable = 0;

  size_t i;
  for (i = 0; i < n_files; i++)
    {
      bool is_a_fifo_or_pipe =
        (STREQ (f[i].name, "-")
         && !f[i].ignore
         && 0 <= f[i].fd
         && (S_ISFIFO (f[i].mode)
             || (HAVE_FIFO_PIPES != 1 && isapipe (f[i].fd))));
      if (is_a_fifo_or_pipe)
        f[i].ignore = true;
      else
        ++n_viable;
    }

  return n_viable;
}

static void
recheck (struct File_spec *f, bool blocking)
{
  /* open/fstat the file and announce if dev/ino have changed */
  struct stat new_stats;
  bool ok = true;
  bool is_stdin = (STREQ (f->name, "-"));
  bool was_tailable = f->tailable;
  int prev_errnum = f->errnum;
  bool new_file;
  int fd = (is_stdin
            ? STDIN_FILENO
            : open (f->name, O_RDONLY | (blocking ? 0 : O_NONBLOCK)));

  assert (valid_file_spec (f));

  /* If the open fails because the file doesn't exist,
     then mark the file as not tailable.  */
  f->tailable = !(reopen_inaccessible_files && fd == -1);

  if (fd == -1 || fstat (fd, &new_stats) < 0)
    {
      ok = false;
      f->errnum = errno;
      if (!f->tailable)
        {
          if (was_tailable)
            {
              /* FIXME-maybe: detect the case in which the file first becomes
                 unreadable (perms), and later becomes readable again and can
                 be seen to be the same file (dev/ino).  Otherwise, tail prints
                 the entire contents of the file when it becomes readable.  */
              error (0, f->errnum, _("%s has become inaccessible"),
                     quote (pretty_name (f)));
            }
          else
            {
              /* say nothing... it's still not tailable */
            }
        }
      else if (prev_errnum != errno)
        {
          error (0, errno, "%s", pretty_name (f));
        }
    }
  else if (!IS_TAILABLE_FILE_TYPE (new_stats.st_mode))
    {
      ok = false;
      f->errnum = -1;
      error (0, 0, _("%s has been replaced with an untailable file;\
 giving up on this name"),
             quote (pretty_name (f)));
      f->ignore = true;
    }
  else if (!disable_inotify && fremote (fd, pretty_name (f)))
    {
      ok = false;
      f->errnum = -1;
      error (0, 0, _("%s has been replaced with a remote file. "
                     "giving up on this name"), quote (pretty_name (f)));
      f->ignore = true;
      f->remote = true;
    }
  else
    {
      f->errnum = 0;
    }

  new_file = false;
  if (!ok)
    {
      close_fd (fd, pretty_name (f));
      close_fd (f->fd, pretty_name (f));
      f->fd = -1;
    }
  else if (prev_errnum && prev_errnum != ENOENT)
    {
      new_file = true;
      assert (f->fd == -1);
      error (0, 0, _("%s has become accessible"), quote (pretty_name (f)));
    }
  else if (f->ino != new_stats.st_ino || f->dev != new_stats.st_dev)
    {
      new_file = true;
      if (f->fd == -1)
        {
          error (0, 0,
                 _("%s has appeared;  following end of new file"),
                 quote (pretty_name (f)));
        }
      else
        {
          /* Close the old one.  */
          close_fd (f->fd, pretty_name (f));

          /* File has been replaced (e.g., via log rotation) --
             tail the new one.  */
          error (0, 0,
                 _("%s has been replaced;  following end of new file"),
                 quote (pretty_name (f)));
        }
    }
  else
    {
      if (f->fd == -1)
        {
          /* This happens when one iteration finds the file missing,
             then the preceding <dev,inode> pair is reused as the
             file is recreated.  */
          new_file = true;
        }
      else
        {
          close_fd (fd, pretty_name (f));
        }
    }

  if (new_file)
    {
      /* Start at the beginning of the file.  */
      record_open_fd (f, fd, 0, &new_stats, (is_stdin ? -1 : blocking));
      xlseek (fd, 0, SEEK_SET, pretty_name (f));
    }
}

/* Return true if any of the N_FILES files in F are live, i.e., have
   open file descriptors.  */

static bool
any_live_files (const struct File_spec *f, size_t n_files)
{
  size_t i;

  for (i = 0; i < n_files; i++)
    if (0 <= f[i].fd)
      return true;
  return false;
}

/* Read and output N_BYTES of file PRETTY_FILENAME starting at the current
   position in FD.  If N_BYTES is COPY_TO_EOF, then copy until end of file.
   If N_BYTES is COPY_A_BUFFER, then copy at most one buffer's worth.
   Return the number of bytes read from the file.  */

static uintmax_t
dump_remainder (const char *pretty_filename, int fd, uintmax_t n_bytes)
{
  uintmax_t n_written;
  uintmax_t n_remaining = n_bytes;

  n_written = 0;
  while (1)
    {
      char buffer[BUFSIZ];
      size_t n = MIN (n_remaining, BUFSIZ);
      size_t bytes_read = safe_read (fd, buffer, n);
      if (bytes_read == SAFE_READ_ERROR)
        {
          if (errno != EAGAIN)
            error (EXIT_FAILURE, errno, _("error reading %s"),
                   quote (pretty_filename));
          break;
        }
      if (bytes_read == 0)
        break;
      xwrite_logger (buffer, bytes_read);
      n_written += bytes_read;
      if (n_bytes != COPY_TO_EOF)
        {
          n_remaining -= bytes_read;
          if (n_remaining == 0 || n_bytes == COPY_A_BUFFER)
            break;
        }
    }

  return n_written;
}

/* Tail N_FILES files forever, or until killed.
   The pertinent information for each file is stored in an entry of F.
   Loop over each of them, doing an fstat to see if they have changed size,
   and an occasional open/fstat to see if any dev/ino pair has changed.
   If none of them have changed size in one iteration, sleep for a
   while and try again.  Continue until the user interrupts us.  */

static void
tail_forever (struct File_spec *f, size_t n_files, double sleep_interval)
{
  /* Use blocking I/O as an optimization, when it's easy.  */
  bool blocking = (pid == 0 && follow_mode == Follow_descriptor
                   && n_files == 1 && ! S_ISREG (f[0].mode));
  size_t last;
  bool writer_is_dead = false;

  last = n_files - 1;

  while (1)
    {
      size_t i;
      bool any_input = false;

      for (i = 0; i < n_files; i++)
        {
          int fd;
          char const *name;
          mode_t mode;
          struct stat stats;
          uintmax_t bytes_read;

          if (f[i].ignore)
            continue;

          if (f[i].fd < 0)
            {
              recheck (&f[i], blocking);
              continue;
            }

          fd = f[i].fd;
          name = pretty_name (&f[i]);
          mode = f[i].mode;

          if (f[i].blocking != blocking)
            {
              int old_flags = fcntl (fd, F_GETFL);
              int new_flags = old_flags | (blocking ? 0 : O_NONBLOCK);
              if (old_flags < 0
                  || (new_flags != old_flags
                      && fcntl (fd, F_SETFL, new_flags) == -1))
                {
                  /* Don't update f[i].blocking if fcntl fails.  */
                  if (S_ISREG (f[i].mode) && errno == EPERM)
                    {
                      /* This happens when using tail -f on a file with
                         the append-only attribute.  */
                    }
                  else
                    error (EXIT_FAILURE, errno,
                           _("%s: cannot change nonblocking mode"), name);
                }
              else
                f[i].blocking = blocking;
            }

          if (!f[i].blocking)
            {
              if (fstat (fd, &stats) != 0)
                {
                  f[i].fd = -1;
                  f[i].errnum = errno;
                  error (0, errno, "%s", name);
                  continue;
                }

              if (f[i].mode == stats.st_mode
                  && (! S_ISREG (stats.st_mode) || f[i].size == stats.st_size)
                  && timespec_cmp (f[i].mtime, get_stat_mtime (&stats)) == 0)
                {
                  if ((max_n_unchanged_stats_between_opens
                       <= f[i].n_unchanged_stats++)
                      && follow_mode == Follow_name)
                    {
                      recheck (&f[i], f[i].blocking);
                      f[i].n_unchanged_stats = 0;
                    }
                  continue;
                }

              /* This file has changed.  Print out what we can, and
                 then keep looping.  */

              f[i].mtime = get_stat_mtime (&stats);
              f[i].mode = stats.st_mode;

              /* reset counter */
              f[i].n_unchanged_stats = 0;

              if (S_ISREG (mode) && stats.st_size < f[i].size)
                {
                  error (0, 0, _("%s: file truncated"), name);
                  last = i;
                  xlseek (fd, stats.st_size, SEEK_SET, name);
                  f[i].size = stats.st_size;
                  continue;
                }

              if (i != last)
                {
                  last = i;
                }
            }

          bytes_read = dump_remainder (name, fd,
                                       (f[i].blocking
                                        ? COPY_A_BUFFER : COPY_TO_EOF));
          any_input |= (bytes_read != 0);
          f[i].size += bytes_read;
        }

      if (! any_live_files (f, n_files) && ! reopen_inaccessible_files)
        {
          error (0, 0, _("no files remaining"));
          break;
        }

      /* If nothing was read, sleep and/or check for dead writers.  */
      if (!any_input)
        {
          if (writer_is_dead)
            break;

          /* Once the writer is dead, read the files once more to
             avoid a race condition.  */
          writer_is_dead = (pid != 0
                            && kill (pid, 0) != 0
                            /* Handle the case in which you cannot send a
                               signal to the writer, so kill fails and sets
                               errno to EPERM.  */
                            && errno != EPERM);

          if (!writer_is_dead && xnanosleep (sleep_interval))
            error (EXIT_FAILURE, errno, _("cannot read realtime clock"));

        }
    }
}

#if HAVE_INOTIFY

/* Return true if any of the N_FILES files in F is remote, i.e., has
   an open file descriptor and is on a network file system.  */

static bool
any_remote_file (const struct File_spec *f, size_t n_files)
{
  size_t i;

  for (i = 0; i < n_files; i++)
    if (0 <= f[i].fd && f[i].remote)
      return true;
  return false;
}

/* Return true if any of the N_FILES files in F represents
   stdin and is tailable.  */

static bool
tailable_stdin (const struct File_spec *f, size_t n_files)
{
  size_t i;

  for (i = 0; i < n_files; i++)
    if (!f[i].ignore && STREQ (f[i].name, "-"))
      return true;
  return false;
}

static size_t
wd_hasher (const void *entry, size_t tabsize)
{
  const struct File_spec *spec = entry;
  return spec->wd % tabsize;
}

static bool
wd_comparator (const void *e1, const void *e2)
{
  const struct File_spec *spec1 = e1;
  const struct File_spec *spec2 = e2;
  return spec1->wd == spec2->wd;
}

/* Helper function used by `tail_forever_inotify'.  */
static void
check_fspec (struct File_spec *fspec, int wd, int *prev_wd)
{
  struct stat stats;
  char const *name = pretty_name (fspec);

  if (fstat (fspec->fd, &stats) != 0)
    {
      close_fd (fspec->fd, name);
      fspec->fd = -1;
      fspec->errnum = errno;
      return;
    }

  if (S_ISREG (fspec->mode) && stats.st_size < fspec->size)
    {
      error (0, 0, _("%s: file truncated"), name);
      *prev_wd = wd;
      xlseek (fspec->fd, stats.st_size, SEEK_SET, name);
      fspec->size = stats.st_size;
    }
  else if (S_ISREG (fspec->mode) && stats.st_size == fspec->size
           && timespec_cmp (fspec->mtime, get_stat_mtime (&stats)) == 0)
    return;

  if (wd != *prev_wd)
    {
      *prev_wd = wd;
    }

  uintmax_t bytes_read = dump_remainder (name, fspec->fd, COPY_TO_EOF);
  fspec->size += bytes_read;

}

/* Attempt to tail N_FILES files forever, or until killed.
   Check modifications using the inotify events system.
   Return false on error, or true to revert to polling.  */
static bool
tail_forever_inotify (int wd, struct File_spec *f, size_t n_files,
                      double sleep_interval)
{
  unsigned int max_realloc = 3;

  /* Map an inotify watch descriptor to the name of the file it's watching.  */
  Hash_table *wd_to_name;

  bool found_watchable_file = false;
  bool found_unwatchable_dir = false;
  bool no_inotify_resources = false;
  bool writer_is_dead = false;
  int prev_wd;
  size_t evlen = 0;
  char *evbuf;
  size_t evbuf_off = 0;
  size_t len = 0;

  wd_to_name = hash_initialize (n_files, NULL, wd_hasher, wd_comparator, NULL);
  if (! wd_to_name)
    xalloc_die ();

  /* Add an inotify watch for each watched file.  If -F is specified then watch
     its parent directory too, in this way when they re-appear we can add them
     again to the watch list.  */
  size_t i;
  for (i = 0; i < n_files; i++)
    {
      if (!f[i].ignore)
        {
          size_t fnlen = strlen (f[i].name);
          if (evlen < fnlen)
            evlen = fnlen;

          f[i].wd = -1;

          if (follow_mode == Follow_name)
            {
              size_t dirlen = dir_len (f[i].name);
              char prev = f[i].name[dirlen];
              f[i].basename_start = last_component (f[i].name) - f[i].name;

              f[i].name[dirlen] = '\0';

               /* It's fine to add the same directory more than once.
                  In that case the same watch descriptor is returned.  */
              f[i].parent_wd = inotify_add_watch (wd, dirlen ? f[i].name : ".",
                                                  (IN_CREATE | IN_MOVED_TO
                                                   | IN_ATTRIB));

              f[i].name[dirlen] = prev;

              if (f[i].parent_wd < 0)
                {
                  if (errno != ENOSPC) /* suppress confusing error.  */
                    error (0, errno, _("cannot watch parent directory of %s"),
                           quote (f[i].name));
                  else
                    error (0, 0, _("inotify resources exhausted"));
                  found_unwatchable_dir = true;
                  /* We revert to polling below.  Note invalid uses
                     of the inotify API will still be diagnosed.  */
                  break;
                }
            }

          f[i].wd = inotify_add_watch (wd, f[i].name, inotify_wd_mask);

          if (f[i].wd < 0)
            {
              if (errno == ENOSPC)
                {
                  no_inotify_resources = true;
                  error (0, 0, _("inotify resources exhausted"));
                }
              else if (errno != f[i].errnum)
                error (0, errno, _("cannot watch %s"), quote (f[i].name));
              continue;
            }

          if (hash_insert (wd_to_name, &(f[i])) == NULL)
            xalloc_die ();

          found_watchable_file = true;
        }
    }

  /* Linux kernel 2.6.24 at least has a bug where eventually, ENOSPC is always
     returned by inotify_add_watch.  In any case we should revert to polling
     when there are no inotify resources.  Also a specified directory may not
     be currently present or accessible, so revert to polling.  */
  if (no_inotify_resources || found_unwatchable_dir)
    {
      /* FIXME: release hash and inotify resources allocated above.  */
      errno = 0;
      return true;
    }
  if (follow_mode == Follow_descriptor && !found_watchable_file)
    return false;

  prev_wd = f[n_files - 1].wd;

  /* Check files again.  New data can be available since last time we checked
     and before they are watched by inotify.  */
  for (i = 0; i < n_files; i++)
    {
      if (!f[i].ignore)
        check_fspec (&f[i], f[i].wd, &prev_wd);
    }

  evlen += sizeof (struct inotify_event) + 1;
  evbuf = xmalloc (evlen);

  /* Wait for inotify events and handle them.  Events on directories make sure
     that watched files can be re-added when -F is used.
     This loop sleeps on the `safe_read' call until a new event is notified.  */
  while (1)
    {
      struct File_spec *fspec;
      struct inotify_event *ev;

      /* When watching a PID, ensure that a read from WD will not block
         indefinitely.  */
      if (pid)
        {
          if (writer_is_dead)
            exit (EXIT_SUCCESS);

          writer_is_dead = (kill (pid, 0) != 0 && errno != EPERM);

          struct timeval delay; /* how long to wait for file changes.  */
          if (writer_is_dead)
            delay.tv_sec = delay.tv_usec = 0;
          else
            {
              delay.tv_sec = (time_t) sleep_interval;
              delay.tv_usec = 1000000 * (sleep_interval - delay.tv_sec);
            }

           fd_set rfd;
           FD_ZERO (&rfd);
           FD_SET (wd, &rfd);

           int file_change = select (wd + 1, &rfd, NULL, NULL, &delay);

           if (file_change == 0)
             continue;
           else if (file_change == -1)
             error (EXIT_FAILURE, errno, _("error monitoring inotify event"));
        }

      if (len <= evbuf_off)
        {
          len = safe_read (wd, evbuf, evlen);
          evbuf_off = 0;

          /* For kernels prior to 2.6.21, read returns 0 when the buffer
             is too small.  */
          if ((len == 0 || (len == SAFE_READ_ERROR && errno == EINVAL))
              && max_realloc--)
            {
              len = 0;
              evlen *= 2;
              evbuf = xrealloc (evbuf, evlen);
              continue;
            }

          if (len == 0 || len == SAFE_READ_ERROR)
            error (EXIT_FAILURE, errno, _("error reading inotify event"));
        }

      ev = (struct inotify_event *) (evbuf + evbuf_off);
      evbuf_off += sizeof (*ev) + ev->len;

      if (ev->len) /* event on ev->name in watched directory  */
        {
          size_t j;
          for (j = 0; j < n_files; j++)
            {
              /* With N=hundreds of frequently-changing files, this O(N^2)
                 process might be a problem.  FIXME: use a hash table?  */
              if (f[j].parent_wd == ev->wd
                  && STREQ (ev->name, f[j].name + f[j].basename_start))
                break;
            }

          /* It is not a watched file.  */
          if (j == n_files)
            continue;

          /* It's fine to add the same file more than once.  */
          int new_wd = inotify_add_watch (wd, f[j].name, inotify_wd_mask);
          if (new_wd < 0)
            {
              error (0, errno, _("cannot watch %s"), quote (f[j].name));
              continue;
            }

          fspec = &(f[j]);

          /* Remove `fspec' and re-add it using `new_fd' as its key.  */
          hash_delete (wd_to_name, fspec);
          fspec->wd = new_wd;

          /* If the file was moved then inotify will use the source file wd for
             the destination file.  Make sure the key is not present in the
             table.  */
          struct File_spec *prev = hash_delete (wd_to_name, fspec);
          if (prev && prev != fspec)
            {
              if (follow_mode == Follow_name)
                recheck (prev, false);
              prev->wd = -1;
              close_fd (prev->fd, pretty_name (prev));
            }

          if (hash_insert (wd_to_name, fspec) == NULL)
            xalloc_die ();

          if (follow_mode == Follow_name)
            recheck (fspec, false);
        }
      else
        {
          struct File_spec key;
          key.wd = ev->wd;
          fspec = hash_lookup (wd_to_name, &key);
        }

      if (! fspec)
        continue;

      if (ev->mask & (IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF))
        {
          /* For IN_DELETE_SELF, we always want to remove the watch.
             However, for IN_MOVE_SELF (the file we're watching has
             been clobbered via a rename), when tailing by NAME, we
             must continue to watch the file.  It's only when following
             by file descriptor that we must remove the watch.  */
          if ((ev->mask & IN_DELETE_SELF)
              || ((ev->mask & IN_MOVE_SELF)
                  && follow_mode == Follow_descriptor))
            {
              inotify_rm_watch (wd, fspec->wd);
              hash_delete (wd_to_name, fspec);
            }
          if (follow_mode == Follow_name)
            recheck (fspec, false);

          continue;
        }
      check_fspec (fspec, ev->wd, &prev_wd);
    }
}
#endif

/* end of TAIL routines */

static void __attribute__ ((__noreturn__)) usage(FILE *out)
{
	fputs(_("\nUsage:\n"), out);
	fprintf(out,
	      _(" %s [options] [message]\n"), program_invocation_short_name);

	fputs(_("\nOptions:\n"), out);
	fputs(_(" -d, --udp             use UDP (TCP is default)\n"
		" -i, --id              log the process ID too\n"
		" -f, --file <file>     log the contents of this file\n"
		" -h, --help            display this help text and exit\n"), out);
	fputs(_(" -n, --server <name>   write to this remote syslog server\n"
		" -P, --port <number>   use this UDP port\n"
		" -p, --priority <prio> mark given message with this priority\n"
		" -s, --stderr          output message to standard error as well\n"), out);
	fputs(_(" -t, --tag <tag>       mark every line with this tag\n"
		" -a, --add <string>	insert string into log line\n"
		" -u, --socket <socket> write to this Unix socket\n"
		" -V, --version         output version information and exit\n\n"), out);
	fputs(_(" -q, --quiet		never print file headers\n"
		" -v, --verbose		always print file headers\n"
		" -S, --sleep-interval  change the polling time\n\n"), out);

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

/*
 * logger -- read and log utility
 *
 *	Reads from one or more files and arranges to write the result on the system
 *	log.
 */
int
main(int argc, char **argv) {
	int ch;
	char buf[4096];
	char *udpserver = NULL;
	long tmpport;

/* options imported from tail.c */
  double sleep_interval = 1.0;
  enum header_mode header_mode = multiple_files;
  bool ok = true;
  uintmax_t n_units = DEFAULT_N_LINES;
  size_t n_files;
  char **file;
  struct File_spec *F;
  size_t i;

  have_read_stdin = false;
  count_lines = true;
  print_headers = false;
  /* always implement tail -F behavior */
  forever = true;
  follow_mode = Follow_name;
  reopen_inaccessible_files = true;

/* process command line arguments */
	static const struct option longopts[] = {
		{ "id",		no_argument,	    0, 'i' },
		{ "stderr",	no_argument,	    0, 's' },
		{ "file",	required_argument,  0, 'f' },
		{ "priority",	required_argument,  0, 'p' },
		{ "tag",	required_argument,  0, 't' },
		{ "add",	required_argument,  0, 'a' },
		{ "socket",	required_argument,  0, 'u' },
		{ "udp",	no_argument,	    0, 'd' },
		{ "server",	required_argument,  0, 'n' },
		{ "port",	required_argument,  0, 'P' },
		{ "version",	no_argument,	    0, 'V' },
		{ "help",	no_argument,	    0, 'h' },
		{ "quiet",	no_argument,	    0, 'q' },
		{ "verbose",	no_argument,	    0, 'v' },
		{ "sleep-interval",	required_argument,  0, 'S' },
		{ NULL,		0, 0, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	while ((ch = getopt_long(argc, argv, "f:ip:st:a:u:dn:P:VhqvS:",
					    longopts, NULL)) != -1) {
		switch((char)ch) {
		case 'f':		/* file to log */
			if (freopen(optarg, "r", stdin) == NULL)
				err(EXIT_FAILURE, _("file %s"),
				    optarg);
			break;
		case 'i':		/* log process id also */
			logflags |= LOG_PID;
			break;
		case 'p':		/* priority */
			pri = pencode(optarg);
			break;
		case 's':		/* log to standard error */
			logflags |= LOG_PERROR;
			break;
		case 't':		/* tag */
			tag = optarg;
			break;
		case 'a':		/* add */
			add = optarg;
			break;

		case 'u':		/* unix socket */
			usock = optarg;
			break;
		case 'd':
			optd = 1;	/* use datagrams */
			break;
		case 'n':		/* udp socket */
			optd = 1;	/* use datagrams because udp */
			udpserver = optarg;
			break;
		case 'P':		/* change udp port */
			tmpport = strtol_or_err(optarg,
						_("failed to parse port number"));
			if (tmpport < 0 || 65535 < tmpport)
				errx(EXIT_FAILURE, _("port `%ld' out of range"),
						tmpport);
			udpport = (int) tmpport;
			break;
		case 'V':	
			printf(_("%s from %s\n"), program_invocation_short_name,
						  PACKAGE_STRING);
			exit(EXIT_SUCCESS);
		case 'h':
			usage(stdout);
	        case 'S':	/* from tail.c: sleep interval */	
	   	   {
       		     double s;
       		     if (! (xstrtod (optarg, NULL, &s, c_strtod) && 0 <= s))
              		error (EXIT_FAILURE, 0,
                    		 _("%s: invalid number of seconds"), optarg);
            		sleep_interval = s;
	           }
         		 break;
		case 'q':	/* from tail.c: never print file header */
			header_mode = never;
			break;
		case 'v':	/* from tail.c: always print file header */
			header_mode = always;
			break;
		case '?':
		default:
			usage(stderr);
		}
	}
	argc -= optind;
	argv += optind;

	/* setup for logging */
	if (!usock && !udpserver)
		openlog(tag ? tag : getlogin(), logflags, 0);
	else if (udpserver)
		LogSock = udpopenlog(udpserver,udpport);
	else
		LogSock = myopenlog(usock);

/* Assuming anything left on command line are filenames 

/* anything left on command line after processing command line
 * options is assumed to be the log message itself
 * so log only it and then exit
	if (argc > 0) {
		register char *p, *endp;
		size_t len;

		for (p = buf, endp = buf + sizeof(buf) - 2; *argv;) {
			len = strlen(*argv);
			if (p + len > endp && p > buf) {
			    if (!usock && !udpserver)
				syslog(pri, "%s", buf);
			    else
				mysyslog(LogSock, logflags, pri, tag, add, buf);
				p = buf;
			}
			if (len > sizeof(buf) - 1) {
			    if (!usock && !udpserver)
				syslog(pri, "%s", *argv++);
			    else
				mysyslog(LogSock, logflags, pri, tag, add, *argv++);
			} else {
				if (p != buf)
					*p++ = ' ';
				memmove(p, *argv++, len);
				*(p += len) = '\0';
			}
		} // end for
		if (p != buf) {
		    if (!usock)
			syslog(pri, "%s", buf);
		    else
			mysyslog(LogSock, logflags, pri, tag, add, buf);
		}

/* otherwise tail the file(s) forever 
	} else {
*/


        if (argc <= 0) {
              error (EXIT_FAILURE, errno, _("Must specify at least one file."));
                              exit(EXIT_FAILURE);
		}

	n_files = argc;
	file = argv;

/* exit if a hyphen is found in filename */
  {
    bool found_hyphen = false;

    for (i = 0; i < n_files; i++)
      if (STREQ (file[i], "-"))
        found_hyphen = true;

    /* When following by name, there must be a name.  */
    if (found_hyphen && follow_mode == Follow_name)
      error (EXIT_FAILURE, 0, _("cannot follow %s by name"), quote ("-"));

    /* When following forever, warn if any file is `-'. */
    if (forever && found_hyphen && isatty (STDIN_FILENO))
      error (EXIT_FAILURE, 0, _("Usage error: not allowed to follow standard input"));
  }

  F = xnmalloc (n_files, sizeof *F);
  for (i = 0; i < n_files; i++) {
    F[i].name = file[i];
  }

  if (header_mode == always)
    print_headers = true;

  if (O_BINARY && ! isatty (STDOUT_FILENO))
    xfreopen (NULL, "wb", stdout);

  for (i = 0; i < n_files; i++)
    ok &= tail_file (&F[i], n_units);

/* start tailing */
  if (forever && ignore_fifo_and_pipe (F, n_files))
    {
#if HAVE_INOTIFY
      /* tailable_stdin() checks if the user specifies stdin via  "-",
         or implicitly by providing no arguments. If so, we won't continue.

         any_remote_file() checks if the user has specified any
         files that reside on remote file systems.  inotify is not used
         in this case because it would miss any updates to the file
         that were not initiated from the local system.

         follow_mode == Follow_name  */

	if (tailable_stdin (F, n_files)) {
	  error (EXIT_FAILURE, errno, _("Please specify a real file, not stdin."));
	  exit(EXIT_FAILURE);
	}

      if (!disable_inotify && any_remote_file (F, n_files))
        disable_inotify = true;

      if (!disable_inotify)
        {
          int wd = inotify_init ();
          if (0 <= wd)
              if (!tail_forever_inotify (wd, F, n_files, sleep_interval))
                exit (EXIT_FAILURE);
          error (0, errno, _("inotify cannot be used, reverting to polling"));
        }
#endif
      disable_inotify = true;
      tail_forever (F, n_files, sleep_interval);
    } /* end tailing */

/*  } end tail forever */

  if (!usock)
	closelog();
  else
	close(LogSock);

  if (have_read_stdin && close (STDIN_FILENO) < 0)
    error (EXIT_FAILURE, errno, "-");

  exit (ok ? EXIT_SUCCESS : EXIT_FAILURE);

}

/*
 *  Decode a symbolic name to a numeric value
 */
int
pencode(s)
	register char *s;
{
	char *save;
	int fac, lev;

	for (save = s; *s && *s != '.'; ++s);
	if (*s) {
		*s = '\0';
		fac = decode(save, facilitynames);
		if (fac < 0)
			errx(EXIT_FAILURE,
			    _("unknown facility name: %s."), save);
		*s++ = '.';
	}
	else {
		fac = LOG_USER;
		s = save;
	}
	lev = decode(s, prioritynames);
	if (lev < 0)
		errx(EXIT_FAILURE,
		    _("unknown priority name: %s."), save);
	return ((lev & LOG_PRIMASK) | (fac & LOG_FACMASK));
}

int
decode(name, codetab)
	char *name;
	CODE *codetab;
{
	register CODE *c;

	if (isdigit(*name))
		return (atoi(name));

	for (c = codetab; c->c_name; c++)
		if (!strcasecmp(name, c->c_name))
			return (c->c_val);

	return (-1);
}
