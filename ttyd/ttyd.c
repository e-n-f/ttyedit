/*
 * ttyd -- terminal helper daemon
 *
 * Copyright 1999 Eric Fischer <enf@pobox.com>
 *
 * You can modify and distribute this program under the terms of the GNU
 * General Public License.  Contact the author to arrange other licenses.
 */

#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>

/* list of mappings between terminal device numbers and names */

struct ttylist {
	char *name;
	dev_t dev;
	struct ttylist *next;
};

/* doubly linked list of history lines for each process that has a history */

struct line {
	struct line *prev;
	char *text;
	struct line *next;
};

/* linked list of processes/devices we have a history for */

struct hist {
	pid_t pid;
	dev_t dev;
	struct line *lines;	/* last history line for this pid & dev */
	struct line *current;	/* current line, if any */
	struct hist *next;
};

struct ttylist *findttys();
struct ttylist *findtty (dev_t);
struct hist *findhist (struct hist **, pid_t, dev_t);
void handlehist (struct ttyhelper *, struct hist *, struct ttylist *);
void cleanup (struct hist **);
void beep (int fd);
char **av;

int
main (int argc, char **argv)
{
	struct ttyhelper th;
	struct ttylist *tl = NULL;
	struct hist *hists = NULL;
	time_t starttime, now;
	int size = 1;
	char *buf;
	int cons;

	av = argv;
	time (&starttime);

	/*
	 * the buffer will later be grown to be big enough for
	 * whatever requests come in
	 */

	buf = malloc (size * sizeof (char));
	if (!buf) {
		fprintf (stderr, "%s: out of memory\n", argv[0]);
		exit (EXIT_FAILURE);
	}

	/*
	 * find the device numbers of all the terminal devices,
	 * so we can map numbers to names
	 */

	tl = findttys();

	/*
	 * the main loop.  keep calling ioctl() to get the
	 * next request from the terminal driver.
	 */

	while (1) {
		th.th_len = size;
		th.th_info = buf;

		/*
		 * It seems silly to have to close and reopen /dev/console
		 * every time, but otherwise, at least on the sparc, it
		 * flakes out after logging out on the console.
		 *
		 * It would be better, I guess, if this were a device of
		 * its own instead of an ioctl on an arbitrary tty.
		 */

		cons = open (_PATH_CONSOLE, O_RDONLY);

		if (ioctl (cons, TIOCHELPER, &th) == 0) {
			struct ttylist *t;

			/*
			 * discard the first few requests, because if
			 * they've been sitting there for a while they
			 * won't make any sense at all
			 */

			time (&now);
			if (now <= starttime + 2)
				continue;

			/*
			 * find the name of the terminal corresponding
			 * to this history request
			 */

			for (t = tl; t; t = t->next) {
				if (t->dev == th.th_tty)
					break;
			}

			/*
			 * unless the terminal is unknown, handle
			 * whatever request it's asking for
			 */

			if (t) {
				struct hist *h;

				h = findhist (&hists, th.th_pid, th.th_tty);
				handlehist (&th, h, t);
				cleanup (&hists);
			} else {
				fprintf (stderr, "%s: unknown tty %d\n",
					 argv[0], (int)th.th_tty);
			}
		} else {
			if (errno == EPERM) {
				/*
				 * EPERM means we're not running as root
				 */

				fprintf (stderr, "%s: TIOCHELPER: %s\n",
					 argv[0], strerror (errno));
				fprintf (stderr, "%s: can only usefully be "
					 "run as root\n", argv[0]);
				exit (EXIT_FAILURE);
			} else if (errno == E2BIG) {
				/*
				 * E2BIG means we need to grow the
				 * request buffer
				 */

				free (buf);
				size *= 2;

				buf = malloc (size * sizeof (char));
				if (!buf) {
					fprintf (stderr, "%s: out of memory\n",
						 argv[0]);
					exit (EXIT_FAILURE);
				}
			} else {
				/*
				 * Some other problem; report it
				 * and keep trying
				 */

				fprintf (stderr, "%s: ioctl: %s\n",
					 argv[0], strerror (errno));

				/*
				 * stupid workaround: if the console has
				 * temporarily ceased to be a tty, sleep
				 * a little to give it a chance to recover
				 * rather than getting in a tight loop.
				 */

				if (errno == ENOTTY)
					sleep (1);
			}

		}

		close (cons);
	}

	return EXIT_SUCCESS;
}

/*
 * handlehist -- store or retrieve a line on the history list
 */

void
handlehist (struct ttyhelper *th, struct hist *h, struct ttylist *t)
{
	int fd;

	fd = open (t->name, O_WRONLY);

	switch (th->th_request) {
	case TH_HIST_KEEP:
		/*
		 * store an item in the doubly linked list of history lines
		 * belonging to this terminal and process.
		 *
		 * there really should be some upper bound on the length to
		 * which we allow this list to grow.
		 */
		if (th->th_len) {
			struct line *l;

			l = malloc (sizeof (struct line));
			if (!l) {
				fprintf (stderr, "%s: out of memory\n", av[0]);
				exit (EXIT_FAILURE);
			}

			l->text = malloc ((th->th_len + 1) * sizeof (char));
			if (!l->text) {
				fprintf (stderr, "%s: out of memory\n", av[0]);
				exit (EXIT_FAILURE);
			}

			strncpy (l->text, th->th_info, th->th_len);
			l->text[th->th_len] = '\0';

			l->prev = h->lines;
			l->next = NULL;

			if (h->lines)
				h->lines->next = l;
			h->lines = l;
		}

		h->current = NULL;  /* back to the bottom of the list */
		break;

	case TH_HIST_PREV:
		/*
		 * retrieve the previous (up) history line
		 */
		if (!h->current) {
			if (h->lines) {
				h->current = h->lines;
				goto totty;
			} else
				beep (fd);
		} else {
			if (h->current->prev) {
				h->current = h->current->prev;
				goto totty;
			} else {
#if 0
				/* don't beep -- messes up the screen */
				beep (fd);
#endif
				;
			}
		}

		break;

	case TH_HIST_NEXT:
		/*
		 * retrieve the next (down) history line
		 */
		if (!h->current)
			beep (fd);
		else {
			h->current = h->current->next;
totty:
			{
				struct ttyinput ti;

				/*
				 * now that we have the line from the history,
				 * use ioctl() to put it in the terminal's
				 * input buffer.
				 */

				if (h->current) {
					ti.ti_len = strlen (h->current->text);
					ti.ti_text = h->current->text;
					ti.ti_magic = 0; /* XXX */
				} else {
					ti.ti_len = 0;
					ti.ti_text = "";
					ti.ti_magic = 0; /* XXX */
				}

				ioctl (fd, TIOCTOEOL);
				ioctl (fd, TIOCSINPUT, &ti);
			}
		}

		break;

	default:
		fprintf (stderr, "%s: unknown request %d from %s\n",
			 av[0], th->th_request, t->name);
	}

	close (fd);
}

/*
 * findttys -- return a linked list of all terminal device names
 * and the dev_t numbers that correspond to them.
 */

struct ttylist *
findttys()
{
	DIR *d;
	struct dirent *de;
	struct ttylist *tl = NULL;

	chdir (_PATH_DEV);

	d = opendir (".");
	if (!d) {
		fprintf (stderr, "%s: can't open %s: %s\n", av[0],
			 _PATH_DEV, strerror (errno));
		exit (EXIT_FAILURE);
	}

	while (de = readdir (d)) {
		if (strncmp (de->d_name, "tty", 3) == 0 ||
		    strncmp (de->d_name, "cons", 4) == 0) {
			struct stat st;
			struct ttylist *n;

			if (stat (de->d_name, &st) != 0) {
				fprintf (stderr, "%s: stat %s: %s\n",
					 av[0], de->d_name, strerror (errno));
				exit (EXIT_FAILURE);
			}

			n = malloc (sizeof (struct ttylist));
			if (!n) {
				fprintf (stderr, "%s: out of memory\n", av[0]);
				exit (EXIT_FAILURE);
			}

			n->name = strdup (de->d_name);
			n->dev = st.st_rdev;
			n->next = tl;
			tl = n;
		}
	}

	closedir (d);
	return tl;
}

/*
 * findhist -- find the history list corresponding to the process
 * and device specified, or create one if there isn't one yet
 */

struct hist *
findhist (struct hist **hists, pid_t pid, dev_t dev)
{
	struct hist *h;

	for (h = *hists; h; h = h->next) {
		if (h->pid == pid && h->dev == dev)
			break;
	}

	if (!h) {
		h = malloc (sizeof (struct hist));
		if (!h) {
			fprintf (stderr, "%s: out of memory\n", av[0]);
			exit (EXIT_FAILURE);
		}

		h->dev = dev;
		h->pid = pid;
		h->lines = NULL;
		h->current = NULL;
		h->next = *hists;

		*hists = h;
	}

	return h;
}

/*
 * cleanup -- find any history lists corresponding to processes
 * that no longer exist and get rid of them
 */

void
cleanup (struct hist **hpp)
{
	/*
	 * there must be a better way to do this than polling each
	 * process to find out whether it's still alive.
	 */

	while (*hpp) {
		/*
		 * signal 0 is nondestructive, but will return
		 * a known error if the process does not exist
		 */

		if (kill ((*hpp)->pid, 0)) {
			if (errno == ESRCH) {
				struct hist *tofree;
				struct line *l;

				printf ("time to clean up pid %d\n",
					(int)((*hpp)->pid));

				tofree = *hpp;
				*hpp = (*hpp)->next;

				while (tofree->lines) {
					l = tofree->lines;
					tofree->lines = tofree->lines->prev;
					free (l->text);
					free (l);
				}

				free (tofree);
				continue;
			}
		}

		hpp = &((*hpp)->next);
	}
}

/*
 * beep -- write a beep character to the specified file descriptor
 */

void
beep (int fd)
{
	write (fd, "\a", 1);
}

