/*****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security, LLC.
 *  This module written by Jim Garlick <garlick@llnl.gov>
 *  UCRL-CODE-232438
 *  All Rights Reserved.
 *
 *  This file is part of Lustre Monitoring Tool, version 2.
 *  Authors: H. Wartens, P. Spencer, N. O'Neill, J. Long, J. Garlick
 *  For details, see http://code.google.com/p/lmt/.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

/* ltop.c - curses based interface to lmt cerebro data */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <curses.h>
#include <signal.h>
#if HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <libgen.h>
#include <time.h>
#include <ctype.h>
#include <assert.h>

#include "list.h"
#include "hash.h"
#include "error.h"

#include "proc.h"
#include "lmt.h"

#include "util.h"
#include "ost.h"
#include "mdt.h"
#include "osc.h"
#include "router.h"

#include "lmtcerebro.h"
#include "lmtconf.h"

#include "sample.h"

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif

typedef struct {
    char fsname[17];            /* file system name */
    char name[17];              /* target index (4 hex digits) */
    char oscstate[2];           /* single char state (blank if unknown) */
    sample_t rbytes;            /* read bytes/sec */
    sample_t wbytes;            /* write bytes/sec */
    sample_t iops;              /* io operations (r/w) per second */
    sample_t num_exports;       /* export count */
    sample_t lock_count;        /* lock count */
    sample_t grant_rate;        /* lock grant rate (LGR) */
    sample_t cancel_rate;       /* lock cancel rate (LCR) */
    sample_t connect;           /* connect+reconnect per second */
    sample_t kbytes_free;       /* free space (kbytes) */
    sample_t kbytes_total;      /* total space (kbytes) */
    char recov_status[32];      /* free form string representing recov status */
    time_t ost_metric_timestamp;/* cerebro timestamp for ost metric (not osc) */
    char ossname[MAXHOSTNAMELEN];/* oss hostname */
    int tag;                    /* display this ost line underlined */
} oststat_t;

typedef struct {
    char fsname[17];            /* file system name */
    char name[17];              /* target index (4 hex digitis) */
    sample_t inodes_free;       /* free inode count */
    sample_t inodes_total;      /* total inode count */
    sample_t open;              /* open ops/sec */
    sample_t close;             /* close ops/sec */
    sample_t getattr;           /* getattr ops/sec */
    sample_t setattr;           /* setattr ops/sec */
    sample_t link;              /* link ops/sec */
    sample_t unlink;            /* unlink ops/sec */
    sample_t mkdir;             /* mkdir ops/sec */
    sample_t rmdir;             /* rmdir ops/sec */
    sample_t statfs;            /* statfs ops/sec */
    sample_t rename;            /* rename ops/sec */
    sample_t getxattr;          /* getxattr ops/sec */
    time_t mdt_metric_timestamp;/* cerebro timestamp for mdt metric */
    char mdsname[MAXHOSTNAMELEN];/* mds hostname */
} mdtstat_t;

typedef enum {
    SORT_OST, SORT_OSS, SORT_RBW, SORT_WBW, SORT_IOPS, SORT_EXP, SORT_LOCKS,
    SORT_LGR, SORT_LCR, SORT_CONN,
} sort_t;

static void _poll_cerebro (char *fs, List mdt_data, List ost_data,
                           int stale_secs, FILE *recf, time_t *tp);
static void _play_file (char *fs, List mdt_data, List ost_data,
                        int stale_secs, FILE *playf, time_t *tp, int *tdiffp);
static void _update_display_top (WINDOW *win, char *fs, List mdt_data,
                                 List ost_data, int stale_secs, FILE *recf,
                                 FILE *playf, time_t tnow, int pause);
static void _update_display_ost (WINDOW *win, List ost_data, int minost,
                                 int selost, int stale_secs, time_t tnow);
static void _destroy_oststat (oststat_t *o);
static void _destroy_mdtstat (mdtstat_t *m);
static void _summarize_ost (List ost_data, List oss_data, int stale_secs);
static void _clear_tags (List ost_data);
static void _tag_nth_ost (List ost_data, int selost, List ost_data2);
static void _sort_ostlist (List ost_data, sort_t s, time_t tnow);
static char *_find_first_fs (FILE *playf, int stale_secs);
static void _record_file (FILE *f, time_t tnow, time_t trcv, char *node,
                          char *name, char *s);

/* Hardwired display geometry.  We also assume 80 chars wide.
 */
#define TOPWIN_LINES    7       /* lines in topwin */
#define OSTWIN_H_LINES  1       /* header lines in ostwin */
#define HDRLINES    (TOPWIN_LINES + OSTWIN_H_LINES)

#define OPTIONS "f:c:t:s:r:p:"
#if HAVE_GETOPT_LONG
#define GETOPT(ac,av,opt,lopt) getopt_long (ac,av,opt,lopt,NULL)
static const struct option longopts[] = {
    {"filesystem",      required_argument,  0, 'f'},
    {"config-file",     required_argument,  0, 'c'},
    {"sample-period",   required_argument,  0, 't'},
    {"stale-secs",      required_argument,  0, 's'},
    {"record",          required_argument,  0, 'r'},
    {"play",            required_argument,  0, 'p'},
    {0, 0, 0, 0},
};
#else
#define GETOPT(ac,av,opt,lopt) getopt (ac,av,opt)
#endif

/* N.B. This global is used ONLY for the purpose of allowing
 * _sort_ostlist () to pass the current time to its various sorting
 * functions.
 */
static time_t sort_tnow = 0;

static void
usage (void)
{
    fprintf (stderr, "Usage: ltop [OPTIONS]\n");
    exit (1);
}

int
main (int argc, char *argv[])
{
    int c;
    char *conffile = NULL;
    WINDOW *topwin, *ostwin;
    int ostcount, selost = -1, minost = 0;
    int ostview = 1, resort = 0;
    sort_t sortby = SORT_OST;
    char *fs = NULL;
    int sopt = 0;
    int sample_period = 2; /* seconds */
    int stale_secs = 12; /* seconds */
    List ost_data = list_create ((ListDelF)_destroy_oststat);
    List mdt_data = list_create ((ListDelF)_destroy_mdtstat);
    List oss_data = list_create ((ListDelF)_destroy_oststat);
    time_t tcycle, last_sample = 0;
    char *recpath = "ltop.log";
    FILE *recf = NULL;
    FILE *playf = NULL;
    int pause = 0;

    err_init (argv[0]);
    optind = 0;
    opterr = 0;

    while ((c = GETOPT (argc, argv, OPTIONS, longopts)) != -1) {
        switch (c) {
            case 'f':   /* --filesystem FS */
                fs = optarg;
                break;
            case 'c':   /* --config-file FILE */
                conffile = optarg;
                break;
            case 't':   /* --sample-period SECS */
                sample_period = strtoul (optarg, NULL, 10);
                sopt = 1;
                break;
            case 's':   /* --stale-secs SECS */
                stale_secs = strtoul (optarg, NULL, 10);
                break;
            case 'r':   /* --record FILE */
                recpath = optarg;
                if (!(recf = fopen (recpath, "w+")))
                    err_exit ("error opening %s for writing", recpath);
                break;
            case 'p':   /* --play FILE */
                if (!(playf = fopen (optarg, "r")))
                    err_exit ("error opening %s for reading", optarg);
                break;
            default:
                usage ();
        }
    }
    if (optind < argc)
        usage();
    if (lmt_conf_init (1, conffile) < 0) /* FIXME: needed? */
        exit (1);
    if (playf && sopt)
        msg_exit ("--sample-period and --play cannot be used together");
    if (playf && recf)
        msg_exit ("--record and --play cannot be used together");
#if ! HAVE_CEREBRO_H
    if (!playf)
	msg_exit ("ltop was not built with cerebro support, use -p option");
#endif
    if (!fs)
        fs = _find_first_fs(playf, stale_secs);
    if (!fs)
        msg_exit ("No live file system data found.  Try using -f option.");

    /* Poll cerebro for data, then sort the ost data for display.
     * If either the mds or any ost's are up, then ostcount > 0.
     */
    if (playf) {
        _play_file (fs, mdt_data, ost_data, stale_secs, playf, &tcycle,
                    &sample_period);
        if (feof (playf))
            msg_exit ("premature end of file on playback file");
    } else
        _poll_cerebro (fs, mdt_data, ost_data, stale_secs, recf, &tcycle);
    _sort_ostlist (ost_data, sortby, tcycle);
    assert (ostview);
    if ((ostcount = list_count (ost_data)) == 0)
        msg_exit ("no data found for file system `%s'", fs);

    /* Curses-fu:  keys will not be echoed, tty control sequences aren't
     * handled by tty driver, getch () times out and returns ERR after
     * sample_period seconds, multi-char keypad/arrow keys are handled.
     * Make cursor invisible.
     */
    if (!(topwin = initscr ()))
        err_exit ("error initializing parent window");
    if (!(ostwin = newwin (ostcount, 80, TOPWIN_LINES, 0)))
        err_exit ("error initializing subwindow");
    raw ();
    noecho ();
    timeout (sample_period * 1000);
    keypad (topwin, TRUE);
    curs_set (0);

    /* Main processing loop:
     * Update display, read kbd (or timeout), update ost_data & mdt_data,
     *   create oss_data (summary of ost_data), [repeat]
     */
    while (!isendwin ()) {
        _update_display_top (topwin, fs, ost_data, mdt_data, stale_secs, recf,
                             playf, tcycle, pause);
        _update_display_ost (ostwin, ostview ? ost_data : oss_data,
                             minost, selost, stale_secs, tcycle);
        switch (getch ()) {
            case KEY_DC:            /* Delete - turn off highlighting */
                selost = -1;
                _clear_tags (ost_data);
                _clear_tags (oss_data);
                break;
            case 'q':               /* q|Ctrl-C - quit */
            case 0x03:
                delwin (ostwin);
                endwin ();
                break;
            case KEY_UP:            /* UpArrow|k - move highlight up */
            case 'k':   /* vi */
                if (selost > 0)
                    selost--;
                if (selost >= minost)
                    break;
                /* fall thru */
            case KEY_PPAGE:         /* PageUp|Ctrl-U - previous page */
            case 0x15:
                minost -= (LINES - HDRLINES);
                if (minost < 0)
                    minost = 0;
                break;
            case KEY_DOWN:          /* DnArrow|j - move highlight down */
            case 'j':   /* vi */
                if (selost < ostcount - 1)
                    selost++;
                if (selost - minost < LINES - HDRLINES)
                    break;
                 /* fall thru */
            case KEY_NPAGE:         /* PageDn|Ctrl-D - next page */
            case 0x04:
                if (minost + LINES - HDRLINES <= ostcount)
                    minost += (LINES - HDRLINES);
                break;
            case 'c':               /* c - toggle compressed oss view */
                ostview = !ostview;
                if (!ostview)
                    _summarize_ost (ost_data, oss_data, stale_secs);
                resort = 1;
                ostcount = list_count (ostview ? ost_data : oss_data);
                minost = 0;
                selost = -1;
                break;
            case ' ':               /* SPACE - tag selected OST */
                if (ostview)
                    _tag_nth_ost (ost_data, selost, NULL);
                else
                    _tag_nth_ost (oss_data, selost, ost_data);
                break;
            case 't':               /* t - sort by ost */
                sortby = SORT_OST;
                resort = 1;
                break;
            case 's':               /* O - sort by oss */
                sortby = SORT_OSS;
                resort = 1;
                break;
            case 'r':               /* r - sort by read MB/s */
                sortby = SORT_RBW;
                resort = 1;
                break;
            case 'w':               /* w - sort by write MB/s */
                sortby = SORT_WBW;
                resort = 1;
                break;
            case 'i':               /* i - sort by IOPS */
                sortby = SORT_IOPS;
                resort = 1;
                break;
            case 'x':               /* x - sort by export count */
                sortby = SORT_EXP;
                resort = 1;
                break;
            case 'l':               /* l - sort by lock count */
                sortby = SORT_LOCKS;
                resort = 1;
                break;
            case 'g':               /* g - sort by lock grant rate */
                sortby = SORT_LGR;
                resort = 1;
                break;
            case 'L':               /* L - sort by lock cancellation rate */
                sortby = SORT_LCR;
                resort = 1;
                break;
            case 'C':               /* C - sort by (re-)connection rate */
                sortby = SORT_CONN;
                resort = 1;
                break;
            case 'R':               /* R - toggle record mode */
                if (!playf) {
                    if (recf) {
                        (void)fclose (recf);
                        recf = NULL;
                    } else
                        recf = fopen (recpath, "w+");
                }
                break;
            case 'p':               /* p - pause playback */
                pause = !pause;
                break;
            case ERR:               /* timeout */
                break;
        }
        if (time (NULL) - last_sample >= sample_period) {
            if (!pause) {
                if (playf)
                    _play_file (fs, mdt_data, ost_data, stale_secs, playf,
                                &tcycle, &sample_period);
                else
                    _poll_cerebro (fs, mdt_data, ost_data, stale_secs, recf,
                                   &tcycle);
            }
            if (!ostview)
                _summarize_ost (ost_data, oss_data, stale_secs);
            ostcount = list_count (ostview ? ost_data : oss_data);
            last_sample = time (NULL);
            timeout (sample_period * 1000);
            resort = 1;
        } else
            timeout ((sample_period - (time (NULL) - last_sample)) * 1000);

        if (resort) {
            _sort_ostlist (ost_data, sortby, tcycle); 
            _sort_ostlist (oss_data, sortby, tcycle); 
            resort = 0;
        }
    }

    list_destroy (ost_data);
    list_destroy (mdt_data);
    list_destroy (oss_data);
    
    if (recf) {
        if (fclose (recf) == EOF)
            err ("Error closing %s", recpath);
        else
            msg ("Log recorded in %s", recpath);
    }
    msg ("Goodbye");
    exit (0);
}

/* Update the top (summary) window of the display.
 * Sum data rate and free space over all OST's.
 * Sum op rates and free inodes over all MDT's (>1 if CMD).
 */
static void
_update_display_top (WINDOW *win, char *fs, List ost_data, List mdt_data,
                     int stale_secs, FILE *recf, FILE *playf, time_t tnow,
                     int pause)
{
    time_t trcv = 0;
    int x = 0;
    ListIterator itr;
    double rmbps = 0, wmbps = 0, iops = 0;
    double tbytes_free = 0, tbytes_total = 0;
    double minodes_free = 0, minodes_total = 0;
    double open = 0, close = 0, getattr = 0, setattr = 0;
    double link = 0, unlink = 0, rmdir = 0, mkdir = 0;
    double statfs = 0, rename = 0, getxattr = 0;
    oststat_t *o;
    mdtstat_t *m;

    itr = list_iterator_create (ost_data);
    while ((o = list_next (itr))) {
        rmbps         += sample_rate (o->rbytes, tnow) / (1024*1024);
        wmbps         += sample_rate (o->wbytes, tnow) / (1024*1024);    
        iops          += sample_rate (o->iops, tnow);
        tbytes_free   += sample_val (o->kbytes_free, tnow) / (1024*1024*1024);
        tbytes_total  += sample_val (o->kbytes_total, tnow) / (1024*1024*1024);
    }
    list_iterator_destroy (itr);
    itr = list_iterator_create (mdt_data);
    while ((m = list_next (itr))) {
        open          += sample_rate (m->open, tnow);
        close         += sample_rate (m->close, tnow);
        getattr       += sample_rate (m->getattr, tnow);
        setattr       += sample_rate (m->setattr, tnow);
        link          += sample_rate (m->link, tnow);
        unlink        += sample_rate (m->unlink, tnow);
        rmdir         += sample_rate (m->rmdir, tnow);
        mkdir         += sample_rate (m->mkdir, tnow);
        statfs        += sample_rate (m->statfs, tnow);
        rename        += sample_rate (m->rename, tnow);
        getxattr      += sample_rate (m->getxattr, tnow);
        minodes_free  += sample_val (m->inodes_free, tnow) / (1024*1024);
        minodes_total += sample_val (m->inodes_total, tnow) / (1024*1024);
        if (m->mdt_metric_timestamp > trcv)
            trcv = m->mdt_metric_timestamp;
    }
    list_iterator_destroy (itr);

    wclear (win);

    mvwprintw (win, x, 0, "Filesystem: %s", fs);
    if (pause) {
        wattron (win, A_REVERSE);
        mvwprintw (win, x, 73, "PAUSED");
        wattroff (win, A_REVERSE);
    } else if (recf) {
        wattron (win, A_REVERSE);
        if (ferror (recf))
            mvwprintw (win, x, 68, "WRITE ERROR");
        else
            mvwprintw (win, x, 70, "RECORDING");
        wattroff (win, A_REVERSE);
    } else if (playf) {
        char *ts = ctime (&tnow);

        wattron (win, A_REVERSE);
        if (ferror (playf))
            mvwprintw (win, x, 69, "READ ERROR");
        else if (feof (playf))
            mvwprintw (win, x, 68, "END OF FILE");
        else
            mvwprintw (win, x, 55, "%*s", strlen (ts) - 1, ts);
        wattroff (win, A_REVERSE);
    }
    x++;
    if (tnow - trcv > stale_secs)
        return;
    mvwprintw (win, x++, 0,
      "    Inodes: %10.3fm total, %10.3fm used (%3.0f%%), %10.3fm free",
               minodes_total, minodes_total - minodes_free,
               ((minodes_total - minodes_free) / minodes_total) * 100,
               minodes_free);
    mvwprintw (win, x++, 0,
      "     Space: %10.3ft total, %10.3ft used (%3.0f%%), %10.3ft free",
               tbytes_total, tbytes_total - tbytes_free,
               ((tbytes_total - tbytes_free) / tbytes_total) * 100,
               tbytes_free);
    mvwprintw (win, x++, 0,
      "   Bytes/s: %10.3fg read,  %10.3fg write,            %6.0f IOPS",
               rmbps / 1024, wmbps / 1024, iops);
    mvwprintw (win, x++, 0,
      "   MDops/s: %6.0f open,   %6.0f close,  %6.0f getattr,  %6.0f setattr",
               open, close, getattr, setattr);
    mvwprintw (win, x++, 0,
      "            %6.0f link,   %6.0f unlink, %6.0f mkdir,    %6.0f rmdir",
               link, unlink, mkdir, rmdir);
    mvwprintw (win, x++, 0,
      "            %6.0f statfs, %6.0f rename, %6.0f getxattr",
               statfs, rename, getxattr);

    wrefresh (win);

    assert (x == TOPWIN_LINES);
}

/* Update the ost window of the display.
 * Minost is the first ost to display (zero origin).
 * Selost is the selected ost, or -1 if none are selected (zero origin).
 * Stale_secs is the number of seconds after which data is expried.
 */
static void
_update_display_ost (WINDOW *win, List ost_data, int minost, int selost,
                     int stale_secs, time_t tnow)
{
    ListIterator itr;
    oststat_t *o;
    int x = 0;
    int skipost = minost;

    wclear (win);

    wattron (win, A_REVERSE);
    mvwprintw (win, x++, 0, "%-80s", " OST S        OSS"
               "   Exp   CR rMB/s wMB/s  IOPS   LOCKS  LGR  LCR");
    wattroff(win, A_REVERSE);
    assert (x == OSTWIN_H_LINES);

    itr = list_iterator_create (ost_data);
    while ((o = list_next (itr))) {
        if (skipost-- > 0)
            continue;
        if (x - 1 + minost == selost)
            wattron (win, A_REVERSE);
        if (o->tag)
            wattron (win, A_UNDERLINE);
        /* available info is expired */
        if ((tnow - o->ost_metric_timestamp) > stale_secs) {
            mvwprintw (win, x, 0, "%4.4s %1.1s", o->name, o->oscstate);
        /* ost is in recovery - display recovery stats */
        } else if (strncmp (o->recov_status, "COMPLETE", 8) != 0) {
            mvwprintw (win, x, 0, "%4.4s %1.1s   %s", o->name, o->oscstate,
                       o->recov_status);
        /* ost is in normal state */
        } else {
            mvwprintw (win, x, 0, "%4.4s %1.1s %10.10s"
                       " %5.0f %4.0f %5.0f %5.0f %5.0f %7.0f %4.0f %4.0f",
                       o->name, o->oscstate, o->ossname,
                       sample_val (o->num_exports, tnow),
                       sample_rate (o->connect, tnow),
                       sample_rate (o->rbytes, tnow) / (1024*1024),
                       sample_rate (o->wbytes, tnow) / (1024*1024),
                       sample_rate (o->iops, tnow),
                       sample_val (o->lock_count, tnow),
                       sample_val (o->grant_rate, tnow),
                       sample_val (o->cancel_rate, tnow));
        }
        if (x - 1 + minost == selost)
            wattroff(win, A_REVERSE);
        if (o->tag)
            wattroff(win, A_UNDERLINE);
        x++;
    }
    list_iterator_destroy (itr);

    wrefresh (win);
}

/*  Used for list_find_first () of MDT by target name, e.g. fs-MDTxxxx.
 */
static int
_match_mdtstat (mdtstat_t *m, char *name)
{
    char *p = strstr (name, "-MDT");

    return (strcmp (m->name, p ? p + 4 : name) == 0);
}

/* Create an mdtstat record.
 */
static mdtstat_t *
_create_mdtstat (char *name, int stale_secs)
{
    mdtstat_t *m = xmalloc (sizeof (*m));
    char *mdtx = strstr (name, "-MDT");

    memset (m, 0, sizeof (*m));
    strncpy (m->name, mdtx ? mdtx + 4 : name, sizeof(m->name) - 1);
    strncpy (m->fsname, name, mdtx ? mdtx - name : sizeof (m->fsname) - 1);
    m->inodes_free =  sample_create (stale_secs);
    m->inodes_total = sample_create (stale_secs);
    m->open =         sample_create (stale_secs);
    m->close =        sample_create (stale_secs);
    m->getattr =      sample_create (stale_secs);
    m->setattr =      sample_create (stale_secs);
    m->link =         sample_create (stale_secs);
    m->unlink =       sample_create (stale_secs);
    m->mkdir =        sample_create (stale_secs);
    m->rmdir =        sample_create (stale_secs);
    m->statfs =       sample_create (stale_secs);
    m->rename =       sample_create (stale_secs);
    m->getxattr =     sample_create (stale_secs);
    return m;
}

/* Destroy an mdtstat record.
 */
static void
_destroy_mdtstat (mdtstat_t *m)
{
    sample_destroy (m->inodes_free);
    sample_destroy (m->inodes_total);
    sample_destroy (m->open);
    sample_destroy (m->close);
    sample_destroy (m->getattr);
    sample_destroy (m->setattr);
    sample_destroy (m->link);
    sample_destroy (m->unlink);
    sample_destroy (m->mkdir);
    sample_destroy (m->rmdir);
    sample_destroy (m->statfs);
    sample_destroy (m->rename);
    sample_destroy (m->getxattr);
    free (m);
}

/*  Used for list_find_first () of OST by target name, e.g. fs-OSTxxxx.
 */
static int
_match_oststat (oststat_t *o, char *name)
{
    char *p = strstr (name, "-OST");

    return (strcmp (o->name, p ? p + 4 : name) == 0);
}

/*  Used for list_find_first () of OST by oss name.
 */
static int
_match_oststat2 (oststat_t *o, char *name)
{
    return (strcmp (o->ossname, name) == 0);
}

/* Helper for _cmp_ostatst2 ()
 */
static char *
_numerical_suffix (char *s, unsigned long *np)
{
    char *p = s + strlen (s);

    while (p > s && isdigit (*(p - 1)))
        p--;
    if (*p)
        *np = strtoul (p, NULL, 10);
    return p;
}

/* Used for list_sort () of OST list by ossname.
 * Like strcmp, but handle variable-width (unpadded) numerical suffixes, if any.
 */
static int
_cmp_oststat_byoss (oststat_t *o1, oststat_t *o2)
{
    unsigned long n1, n2;
    char *p1 = _numerical_suffix (o1->ossname, &n1);
    char *p2 = _numerical_suffix (o2->ossname, &n2);

    if (*p1 && *p2
            && (p1 - o1->ossname) == (p2 - o2->ossname)
            && !strncmp (o1->ossname, o2->ossname, p1 - o1->ossname)) {
        return (n1 < n2 ? -1 
              : n1 > n2 ? 1 : 0);
    }
    return strcmp (o1->ossname, o2->ossname);
}

/* Used for list_sort () of OST list by ostname.
 * Fixed width hex sorts alphanumerically.
 */
static int
_cmp_oststat_byost (oststat_t *o1, oststat_t *o2)
{
    return strcmp (o1->name, o2->name);
}

/* Used for list_sort () of OST list by export count (ascending order).
 */
static int
_cmp_oststat_byexp (oststat_t *o1, oststat_t *o2)
{
    return sample_val_cmp (o1->num_exports, o2->num_exports, sort_tnow);
}

/* Used for list_sort () of OST list by lock count (descending order).
 */
static int
_cmp_oststat_bylocks (oststat_t *o1, oststat_t *o2)
{
    return -1 * sample_val_cmp (o1->lock_count, o2->lock_count, sort_tnow);
}

/* Used for list_sort () of OST list by lock grant rate (descending order).
 */
static int
_cmp_oststat_bylgr (oststat_t *o1, oststat_t *o2)
{
    return -1 * sample_val_cmp (o1->grant_rate, o2->grant_rate, sort_tnow);
}

/* Used for list_sort () of OST list by lock cancel rate (descending order).
 */
static int
_cmp_oststat_bylcr (oststat_t *o1, oststat_t *o2)
{
    return -1 * sample_val_cmp (o1->cancel_rate, o2->cancel_rate, sort_tnow);
}

/* Used for list_sort () of OST list by (re-)connect rate (descending order).
 */
static int
_cmp_oststat_byconn (oststat_t *o1, oststat_t *o2)
{
    return -1 * sample_val_cmp (o1->connect, o2->connect, sort_tnow);
}

/* Used for list_sort () of OST list by iops (descending order).
 */
static int
_cmp_oststat_byiops (oststat_t *o1, oststat_t *o2)
{
    return -1 * sample_rate_cmp (o1->iops, o2->iops, sort_tnow);
}

/* Used for list_sort () of OST list by read b/w (descending order).
 */
static int
_cmp_oststat_byrbw (oststat_t *o1, oststat_t *o2)
{
    return -1 * sample_rate_cmp (o1->rbytes, o2->rbytes, sort_tnow);
}

/* Used for list_sort () of OST list by write b/w (descending order).
 */
static int
_cmp_oststat_bywbw (oststat_t *o1, oststat_t *o2)
{
    return -1 * sample_rate_cmp (o1->wbytes, o2->wbytes, sort_tnow);
}

/* Create an oststat record.
 */
static oststat_t *
_create_oststat (char *name, int stale_secs)
{
    oststat_t *o = xmalloc (sizeof (*o));
    char *ostx = strstr (name, "-OST");

    memset (o, 0, sizeof (*o));
    strncpy (o->name, ostx ? ostx + 4 : name, sizeof(o->name) - 1);
    strncpy (o->fsname, name, ostx ? ostx - name : sizeof (o->fsname) - 1);
    *o->oscstate = '\0';
    o->rbytes =       sample_create (stale_secs);
    o->wbytes =       sample_create (stale_secs);
    o->iops =         sample_create (stale_secs);
    o->num_exports =  sample_create (stale_secs);
    o->lock_count =   sample_create (stale_secs);
    o->grant_rate =   sample_create (stale_secs);
    o->cancel_rate =  sample_create (stale_secs);
    o->connect =      sample_create (stale_secs);
    o->kbytes_free =  sample_create (stale_secs);
    o->kbytes_total = sample_create (stale_secs);
    return o;
}

/* Destroy an oststat record.
 */
static void
_destroy_oststat (oststat_t *o)
{
    sample_destroy (o->rbytes);
    sample_destroy (o->wbytes);
    sample_destroy (o->iops);
    sample_destroy (o->num_exports);
    sample_destroy (o->lock_count);
    sample_destroy (o->grant_rate);
    sample_destroy (o->cancel_rate);
    sample_destroy (o->connect);
    sample_destroy (o->kbytes_free);
    sample_destroy (o->kbytes_total);
    free (o);
}

/* Copy an oststat record.
 */
static oststat_t *
_copy_oststat (oststat_t *o1)
{
    oststat_t *o = xmalloc (sizeof (*o));

    memcpy (o, o1, sizeof (*o));
    o->rbytes =       sample_copy (o1->rbytes);
    o->wbytes =       sample_copy (o1->wbytes);
    o->iops =         sample_copy (o1->iops);
    o->num_exports =  sample_copy (o1->num_exports);
    o->lock_count =   sample_copy (o1->lock_count);
    o->grant_rate =   sample_copy (o1->grant_rate);
    o->cancel_rate =  sample_copy (o1->cancel_rate);
    o->connect =      sample_copy (o1->connect);
    o->kbytes_free =  sample_copy (o1->kbytes_free);
    o->kbytes_total = sample_copy (o1->kbytes_total);
    return o;
}

/* Match an OST or MDT target against a file system name.
 * Target names are assumed to be of the form fs-OSTxxxx or fs-MDTxxxx.
 */
static int
_fsmatch (char *name, char *fs)
{
    char *p = strchr (name, '-');
    int len = p ? p - name : strlen (name);

    if (strlen (fs) == len && strncmp (name, fs, len) == 0)
        return 1;
    return 0;
}

/* Update oststat_t record (oscstate field) in ost_data list for
 * specified ostname.  Create an entry if one doesn't exist.
 * FIXME(design): we only keep one OSC state per OST, but possibly multiple
 * MDT's are reporting it under CMD and last in wins.
 */
static void
_update_osc (char *name, char *state, List ost_data,
             time_t tnow, time_t trcv, int stale_secs)
{
    oststat_t *o;

    if (!(o = list_find_first (ost_data, (ListFindF)_match_oststat, name))) {
        o = _create_oststat (name, stale_secs);
        list_append (ost_data, o);
    }
    if (tnow - trcv > stale_secs)
        strncpy (o->oscstate, "", sizeof (o->oscstate) - 1);
    else    
        strncpy (o->oscstate, state, sizeof (o->oscstate) - 1);
}

static void
_decode_osc_v1 (char *val, char *fs, List ost_data,
             time_t tnow, time_t trcv, int stale_secs)
{
    char *s, *mdsname, *oscname, *oscstate;
    List oscinfo;
    ListIterator itr;

    if (lmt_osc_decode_v1 (val, &mdsname, &oscinfo) < 0)
        return;
    itr = list_iterator_create (oscinfo);
    while ((s = list_next (itr))) {
        if (lmt_osc_decode_v1_oscinfo (s, &oscname, &oscstate) >= 0) {
            if (!fs || _fsmatch (oscname, fs))
                _update_osc (oscname, oscstate, ost_data,
                             tnow, trcv, stale_secs);
            free (oscname);
            free (oscstate);
        }
    }
    list_iterator_destroy (itr);
    list_destroy (oscinfo);
    free (mdsname);
}

/* Update oststat_t record in ost_data list for specified ostname.
 * Create an entry if one doesn't exist.
 */
static void
_update_ost (char *ostname, char *ossname, uint64_t read_bytes,
             uint64_t write_bytes, uint64_t iops, uint64_t num_exports,
             uint64_t lock_count, uint64_t grant_rate, uint64_t cancel_rate,
             uint64_t connect, char *recov_status, uint64_t kbytes_free,
             uint64_t kbytes_total, List ost_data,
             time_t tnow, time_t trcv, int stale_secs)
{
    oststat_t *o;

    if (!(o = list_find_first (ost_data, (ListFindF)_match_oststat, ostname))) {
        o = _create_oststat (ostname, stale_secs);
        list_append (ost_data, o);
    }
    if (o->ost_metric_timestamp < trcv) {
        if (strcmp (ossname, o->ossname) != 0) { /* failover/failback */
            sample_invalidate (o->rbytes);
            sample_invalidate (o->wbytes);
            sample_invalidate (o->iops);
            sample_invalidate (o->num_exports);
            sample_invalidate (o->lock_count);
            sample_invalidate (o->kbytes_free);
            sample_invalidate (o->kbytes_total);
            snprintf (o->ossname, sizeof (o->ossname), "%s", ossname);
        }
        o->ost_metric_timestamp = trcv;
        sample_update (o->rbytes, (double)read_bytes, trcv);
        sample_update (o->wbytes, (double)write_bytes, trcv);
        sample_update (o->iops, (double)iops, trcv);
        sample_update (o->num_exports, (double)num_exports, trcv);
        sample_update (o->lock_count, (double)lock_count, trcv);
        sample_update (o->grant_rate, (double)grant_rate, trcv);
        sample_update (o->cancel_rate, (double)cancel_rate, trcv);
        sample_update (o->connect, (double)connect, trcv);
        sample_update (o->kbytes_free, (double)kbytes_free, trcv);
        sample_update (o->kbytes_total, (double)kbytes_total, trcv);
        snprintf (o->recov_status, sizeof(o->recov_status), "%s", recov_status);
    }
}

static void
_decode_ost_v2 (char *val, char *fs, List ost_data,
                time_t tnow, time_t trcv, int stale_secs)
{
    List ostinfo;
    char *s, *ossname, *ostname, *recov_status;
    float pct_cpu, pct_mem;
    uint64_t read_bytes, write_bytes;
    uint64_t kbytes_free, kbytes_total;
    uint64_t inodes_free, inodes_total;
    uint64_t iops, num_exports;
    uint64_t lock_count, grant_rate, cancel_rate;
    uint64_t connect, reconnect;
    ListIterator itr;
    
    if (lmt_ost_decode_v2 (val, &ossname, &pct_cpu, &pct_mem, &ostinfo) < 0)
        return;
    itr = list_iterator_create (ostinfo);
    while ((s = list_next (itr))) {
        if (lmt_ost_decode_v2_ostinfo (s, &ostname,
                                       &read_bytes, &write_bytes,
                                       &kbytes_free, &kbytes_total,
                                       &inodes_free, &inodes_total, &iops,
                                       &num_exports, &lock_count,
                                       &grant_rate, &cancel_rate,
                                       &connect, &reconnect,
                                       &recov_status) == 0) {
            if (!fs || _fsmatch (ostname, fs)) {
                _update_ost (ostname, ossname, read_bytes, write_bytes,
                             iops, num_exports, lock_count, grant_rate,
                             cancel_rate, connect + reconnect, recov_status,
                             kbytes_free, kbytes_total, ost_data,
                             tnow, trcv, stale_secs);
            }
            free (ostname);
            free (recov_status);
        }
    }
    list_iterator_destroy (itr);
    list_destroy (ostinfo);
    free (ossname);
}

/* Update mdtstat_t record in mdt_data list for specified mdtname.
 * Create an entry if one doesn't exist.
 */
static void
_update_mdt (char *mdtname, char *mdsname, uint64_t inodes_free,
             uint64_t inodes_total, uint64_t kbytes_free,
             uint64_t kbytes_total, List mdops, List mdt_data,
             time_t tnow, time_t trcv, int stale_secs)
{
    char *opname, *s;
    ListIterator itr;
    mdtstat_t *m;
    uint64_t samples, sum, sumsquares;

    if (!(m = list_find_first (mdt_data, (ListFindF)_match_mdtstat, mdtname))) {
        m = _create_mdtstat (mdtname, stale_secs);
        list_append (mdt_data, m);
    }
    if (m->mdt_metric_timestamp < trcv) {
        if (strcmp (mdsname, m->mdsname) != 0) { /* failover/failback */
            sample_invalidate (m->inodes_free);
            sample_invalidate (m->inodes_total);
            sample_invalidate (m->open);
            sample_invalidate (m->close);
            sample_invalidate (m->getattr);
            sample_invalidate (m->setattr);
            sample_invalidate (m->link);
            sample_invalidate (m->unlink);
            sample_invalidate (m->mkdir);
            sample_invalidate (m->rmdir);
            sample_invalidate (m->statfs);
            sample_invalidate (m->rename);
            sample_invalidate (m->getxattr);
            snprintf (m->mdsname, sizeof (m->mdsname), "%s", mdsname);
        }
        m->mdt_metric_timestamp = trcv;
        sample_update (m->inodes_free, (double)inodes_free, trcv);
        sample_update (m->inodes_total, (double)inodes_total, trcv);
        itr = list_iterator_create (mdops);
        while ((s = list_next (itr))) {
            if (lmt_mdt_decode_v1_mdops (s, &opname,
                                    &samples, &sum, &sumsquares) == 0) {
                if (!strcmp (opname, "open"))
                    sample_update (m->open, (double)samples, trcv);
                else if (!strcmp (opname, "close"))
                    sample_update (m->close, (double)samples, trcv);
                else if (!strcmp (opname, "getattr"))
                    sample_update (m->getattr, (double)samples, trcv);
                else if (!strcmp (opname, "setattr"))
                    sample_update (m->setattr, (double)samples, trcv);
                else if (!strcmp (opname, "link"))
                    sample_update (m->link, (double)samples, trcv);
                else if (!strcmp (opname, "unlink"))
                    sample_update (m->unlink, (double)samples, trcv);
                else if (!strcmp (opname, "mkdir"))
                    sample_update (m->mkdir, (double)samples, trcv);
                else if (!strcmp (opname, "rmdir"))
                    sample_update (m->rmdir, (double)samples, trcv);
                else if (!strcmp (opname, "statfs"))
                    sample_update (m->statfs, (double)samples, trcv);
                else if (!strcmp (opname, "rename"))
                    sample_update (m->rename, (double)samples, trcv);
                else if (!strcmp (opname, "getxattr"))
                    sample_update (m->getxattr, (double)samples, trcv);
                free (opname);
            }
        }
        list_iterator_destroy (itr);
    }
}

static void
_decode_mdt_v1 (char *val, char *fs, List mdt_data,
                time_t tnow, time_t trcv, int stale_secs)
{
    List mdops, mdtinfo;
    char *s, *mdsname, *mdtname;
    float pct_cpu, pct_mem;
    uint64_t kbytes_free, kbytes_total;
    uint64_t inodes_free, inodes_total;
    ListIterator itr;

    if (lmt_mdt_decode_v1 (val, &mdsname, &pct_cpu, &pct_mem, &mdtinfo) < 0)
        return;
    itr = list_iterator_create (mdtinfo);
    while ((s = list_next (itr))) {
        if (lmt_mdt_decode_v1_mdtinfo (s, &mdtname, &inodes_free,
                                       &inodes_total, &kbytes_free,
                                       &kbytes_total, &mdops) == 0) {
            if (!fs || _fsmatch (mdtname, fs)) {
                _update_mdt (mdtname, mdsname, inodes_free, inodes_total,
                             kbytes_free, kbytes_total, mdops, mdt_data,
                             tnow, trcv, stale_secs);
            }
            free (mdtname);
            list_destroy (mdops);
        }
    }
    list_iterator_destroy (itr);
    list_destroy (mdtinfo);
    free (mdsname);
}

static void
_poll_cerebro (char *fs, List mdt_data, List ost_data, int stale_secs,
               FILE *recf, time_t *tp)
{
    time_t trcv, tnow = time (NULL);
    cmetric_t c;
    List l = NULL;
    char *s, *name, *node;
    ListIterator itr;
    float vers;

#if HAVE_CEREBRO_H
    if (lmt_cbr_get_metrics ("lmt_mdt,lmt_ost,lmt_osc", &l) < 0)
#endif
        return;
    itr = list_iterator_create (l);
    while ((c = list_next (itr))) {
        if (!(name = lmt_cbr_get_name (c)))
            continue;
        if (!(node = lmt_cbr_get_nodename (c)))
            continue;
        if (!(s = lmt_cbr_get_val (c)))
            continue;
        if (sscanf (s, "%f;", &vers) != 1)
            continue;
        trcv = lmt_cbr_get_time (c);
        if (recf)
            _record_file (recf, tnow, trcv, node, name, s);
        if (!strcmp (name, "lmt_mdt") && vers == 1)
            _decode_mdt_v1 (s, fs, mdt_data, tnow, trcv, stale_secs);
        else if (!strcmp (name, "lmt_ost") && vers == 2)
            _decode_ost_v2 (s, fs, ost_data, tnow, trcv, stale_secs);
        else if (!strcmp (name, "lmt_osc") && vers == 1)
            _decode_osc_v1 (s, fs, ost_data, tnow, trcv, stale_secs);
    }
    list_iterator_destroy (itr);
    list_destroy (l);
    if (tp)
        *tp = tnow;
}

/* Write a cerebro metric record and some other info to a line in a file.
 * Ignore any errors, check with ferror () elsewhere.
 */
static void
_record_file (FILE *f, time_t tnow, time_t trcv, char *node,
              char *name, char *s)
{
    (void)fprintf (f, "%"PRIu64" %"PRIu64" %s %s %s\n",
                   (uint64_t)tnow, (uint64_t)trcv, node, name, s);
}

/* Analagous to _poll_cerebro (), except input is taken from a file
 * written by _record_file ().   The wall clock time recorded in the first
 * field groups records into batches.  This function reads only one batch,
 * and places its wall clock time in *tp.
 */
static void
_play_file (char *fs, List mdt_data, List ost_data, int stale_secs,
            FILE *f, time_t *tp, int *tdiffp)
{
    static char s[65536];
    float vers;
    uint64_t tnow, trcv, tmark = 0;
    char node[64], name[16];
    fpos_t pos;
    int tdiff = 0;

    if (feof (f) || ferror (f))
        return;
    while (fscanf (f, "%"PRIu64" %"PRIu64" %64s %16s %65536[^\n]\n",
                   &tnow, &trcv, node, name, s) == 5) {
        if (tmark != 0 && tmark != tnow) {
            if (fsetpos (f, &pos) < 0) /* rewind! */
                err_exit ("fsetpos failed on playback file");
            tdiff = tnow - tmark;
            break;
        }
        if (sscanf (s, "%f;", &vers) != 1)
            msg_exit ("Parse error reading metric version in playback file");
        if (!strcmp (name, "lmt_mdt") && vers == 1)
            _decode_mdt_v1 (s, fs, mdt_data, tnow, trcv, stale_secs);
        else if (!strcmp (name, "lmt_ost") && vers == 2)
            _decode_ost_v2 (s, fs, ost_data, tnow, trcv, stale_secs);
        else if (!strcmp (name, "lmt_osc") && vers == 1)
            _decode_osc_v1 (s, fs, ost_data, tnow, trcv, stale_secs);
        if (fgetpos (f, &pos) < 0)
            err_exit ("fgetpos failed on playback file");
        tmark = tnow;
    }
    if (ferror (f))
        err_exit ("Error reading playback file");
    if (tmark == 0)
        msg_exit ("Error parsing playback file");
    if (tp)
        *tp = tmark;
    if (tdiffp && !feof (f))
        *tdiffp = tdiff;
}

/* Peek at the data to find a default file system to monitor.
 */
static char *
_find_first_fs (FILE *playf, int stale_secs)
{
    List ost_data = list_create ((ListDelF)_destroy_oststat);
    List mdt_data = list_create ((ListDelF)_destroy_mdtstat);
    ListIterator itr;
    mdtstat_t *m;
    oststat_t *o;
    char *ret = NULL;

    if (playf) {
        _play_file (NULL, mdt_data, ost_data, stale_secs, playf, NULL, NULL);
        if (fseek (playf, 0, SEEK_SET) < 0)
            err_exit ("error rewinding playback file");
    } else
        _poll_cerebro (NULL, mdt_data, ost_data, stale_secs, NULL, NULL);

    itr = list_iterator_create (mdt_data);
    while (!ret && (m = list_next (itr)))
        ret = xstrdup (m->fsname);
    list_iterator_destroy (itr);

    itr = list_iterator_create (ost_data);
    while (!ret && (o = list_next (itr)))
        ret = xstrdup (o->fsname);
    list_iterator_destroy (itr);

    list_destroy (ost_data);
    list_destroy (mdt_data);

    return ret;
}

/* Re-create oss_data, one record per oss, with data aggregated from
 * the OST's on that OSS.
 */
static void
_summarize_ost (List ost_data, List oss_data, int stale_secs)
{
    oststat_t *o, *o2;
    ListIterator itr;

    while ((o = list_dequeue (oss_data)))
        _destroy_oststat (o);

    itr = list_iterator_create (ost_data);
    while ((o = list_next (itr))) {
        o2 = list_find_first (oss_data, (ListFindF)_match_oststat2, o->ossname);
        if (o2) {
            sample_add (o2->rbytes, o->rbytes);
            sample_add (o2->wbytes, o->wbytes);
            sample_add (o2->iops, o->iops);
            sample_add (o2->kbytes_free, o->kbytes_free);
            sample_add (o2->kbytes_total, o->kbytes_total);
            sample_add (o2->lock_count, o->lock_count);
            sample_add (o2->grant_rate, o->grant_rate);
            sample_add (o2->cancel_rate, o->cancel_rate);
            sample_add (o2->connect, o->connect);
            if (o->ost_metric_timestamp > o2->ost_metric_timestamp)
                o2->ost_metric_timestamp = o->ost_metric_timestamp;
            /* Ensure recov_status and oscstate reflect any unrecovered or
             * non-full state of individual OSTs.  Last in wins.
             */
            if (strcmp (o->oscstate, "F") != 0)
                memcpy (o2->oscstate, o->oscstate, sizeof (o->oscstate));
            if (strncmp (o->recov_status, "COMPLETE", 8) != 0)
                memcpy (o2->recov_status, o->recov_status,
                        sizeof (o->recov_status));
            /* Similarly, any "missing clients" on OST's should be reflected.
             * in the OSS exports count.
             */
            sample_min (o2->num_exports, o->num_exports);
            /* Maintain OST count in name field.
             */
            snprintf (o2->name, sizeof (o2->name), "(%d)",
                      (int)strtoul (o2->name + 1, NULL, 10) + 1);
            if (o->tag)
                o2->tag = o->tag;
        } else {
            o2 = _copy_oststat (o);
            snprintf (o2->name, sizeof (o2->name), "(%d)", 1);
            list_append (oss_data, o2);
        }
    }
    list_iterator_destroy (itr);
    list_sort (oss_data, (ListCmpF)_cmp_oststat_byoss);
}

/* Clear all tags.
 */
static void
_clear_tags (List ost_data)
{
    oststat_t *o;
    ListIterator itr;

    itr = list_iterator_create (ost_data);
    while ((o = list_next (itr)))
        o->tag = 0;
    list_iterator_destroy (itr);
}

/* Set tag value on ost's with specified oss.
 */
static void
_tag_ost_byoss (List ost_data, char *ossname, int tagval)
{
    oststat_t *o;
    ListIterator itr;

    itr = list_iterator_create (ost_data);
    while ((o = list_next (itr)))
        if (!strcmp (o->ossname, ossname))
            o->tag = tagval;
    list_iterator_destroy (itr);
}

/* Toggle tag value on nth ost.
 * If tagging ost_data (first param), set the last paramter NULL.
 * If tagging oss_data (first param), set the last parmater to ost_data,
 * and all ost's on this oss will get tagged too.
 */
static void
_tag_nth_ost (List ost_data, int selost, List ost_data2)
{
    oststat_t *o;
    ListIterator itr;
    int n = 0;

    itr = list_iterator_create (ost_data);
    while ((o = list_next (itr))) {
        if (selost == n++) {
            o->tag = !o->tag;
            break;
        }
    }
    list_iterator_destroy (itr);
    if (ost_data2 && o != NULL)
        _tag_ost_byoss (ost_data2, o->ossname, o->tag);
}

static void
_sort_ostlist (List ost_data, sort_t s, time_t tnow)
{
    ListCmpF c = NULL;

    switch (s) {
        case SORT_OST:
            c = (ListCmpF)_cmp_oststat_byost;
            break;
        case SORT_OSS:
            c = (ListCmpF)_cmp_oststat_byoss;
            break;
        case SORT_RBW:
            c = (ListCmpF)_cmp_oststat_byrbw;
            break;
        case SORT_WBW:
            c = (ListCmpF)_cmp_oststat_bywbw;
            break;
        case SORT_IOPS:
            c = (ListCmpF)_cmp_oststat_byiops;
            break;
        case SORT_EXP:
            c = (ListCmpF)_cmp_oststat_byexp;
            break;
        case SORT_LOCKS:
            c = (ListCmpF)_cmp_oststat_bylocks;
            break;
        case SORT_LGR:
            c = (ListCmpF)_cmp_oststat_bylgr;
            break;
        case SORT_LCR:
            c = (ListCmpF)_cmp_oststat_bylcr;
            break;
        case SORT_CONN:
            c = (ListCmpF)_cmp_oststat_byconn;
            break;
    }
    sort_tnow = tnow;
    list_sort (ost_data, c);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */