/*
-*- linux-c -*-
   drbd_proc.c
   Kernel module for 2.6.x Kernels

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/autoconf.h>
#include <linux/module.h>

#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/drbd.h>
#include "drbd_int.h"
#include "lru_cache.h" /* for lc_sprintf_stats */

int drbd_proc_open(struct inode *inode, struct file *file);


struct proc_dir_entry *drbd_proc;
struct file_operations drbd_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= drbd_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


/*lge
 * progress bars shamelessly adapted from driver/md/md.c
 * output looks like
 *	[=====>..............] 33.5% (23456/123456)
 *	finish: 2:20:20 speed: 6,345 (6,456) K/sec
 */
void drbd_syncer_progress(struct drbd_conf *mdev, struct seq_file *seq)
{
	unsigned long db, dt, dbdt, rt, rs_left;
	int i, x, y, res;

	drbd_get_syncer_progress(mdev, &rs_left, &res);

	x = res/50;
	y = 20-x;
	seq_printf(seq, "\t[");
	for (i = 1; i < x; i++)
		seq_printf(seq, "=");
	seq_printf(seq, ">");
	for (i = 0; i < y; i++)
		seq_printf(seq, ".");
	seq_printf(seq, "] ");

	seq_printf(seq, "sync'ed:%3u.%u%% ", res / 10, res % 10);
	/* if more than 1 GB display in MB */
	if (mdev->rs_total > 0x100000L)
		seq_printf(seq, "(%lu/%lu)M\n\t",
			    (unsigned long) Bit2KB(rs_left) >> 10,
			    (unsigned long) Bit2KB(mdev->rs_total) >> 10 );
	else
		seq_printf(seq, "(%lu/%lu)K\n\t",
			    (unsigned long) Bit2KB(rs_left),
			    (unsigned long) Bit2KB(mdev->rs_total) );

	/* see drivers/md/md.c
	 * We do not want to overflow, so the order of operands and
	 * the * 100 / 100 trick are important. We do a +1 to be
	 * safe against division by zero. We only estimate anyway.
	 *
	 * dt: time from mark until now
	 * db: blocks written from mark until now
	 * rt: remaining time
	 */
	dt = (jiffies - mdev->rs_mark_time) / HZ;

	if (dt > 20) {
		/* if we made no update to rs_mark_time for too long,
		 * we are stalled. show that. */
		seq_printf(seq, "stalled\n");
		return;
	}

	if (!dt)
		dt++;
	db = mdev->rs_mark_left - rs_left;
	rt = (dt * (rs_left / (db/100+1)))/100; /* seconds */

	seq_printf(seq, "finish: %lu:%02lu:%02lu",
		rt / 3600, (rt % 3600) / 60, rt % 60);

	/* current speed average over (SYNC_MARKS * SYNC_MARK_STEP) jiffies */
	dbdt = Bit2KB(db/dt);
	if (dbdt > 1000)
		seq_printf(seq, " speed: %ld,%03ld",
			dbdt/1000, dbdt % 1000);
	else
		seq_printf(seq, " speed: %ld", dbdt);

	/* mean speed since syncer started
	 * we do account for PausedSync periods */
	dt = (jiffies - mdev->rs_start - mdev->rs_paused) / HZ;
	if (dt <= 0)
		dt = 1;
	db = mdev->rs_total - rs_left;
	dbdt = Bit2KB(db/dt);
	if (dbdt > 1000)
		seq_printf(seq, " (%ld,%03ld)",
			dbdt/1000, dbdt % 1000);
	else
		seq_printf(seq, " (%ld)", dbdt);

	seq_printf(seq, " K/sec\n");
}

#ifdef ENABLE_DYNAMIC_TRACE
void resync_dump_detail(struct seq_file *seq, struct lc_element *e)
{
	struct bm_extent *bme = (struct bm_extent *)e;

	seq_printf(seq, "%5d %s %s\n", bme->rs_left,
		   bme->flags & BME_NO_WRITES ? "NO_WRITES" : "---------",
		   bme->flags & BME_LOCKED ? "LOCKED" : "------"
		   );
}
#endif

int drbd_seq_show(struct seq_file *seq, void *v)
{
	int i, hole = 0;
	const char *sn;
	struct drbd_conf *mdev;

	seq_printf(seq, "version: " REL_VERSION " (api:%d/proto:%d-%d)\n%s\n",
		   API_VERSION, PRO_VERSION_MIN, PRO_VERSION_MAX, drbd_buildtag());

	/*
	  cs .. connection state
	  st .. node state (local/remote)
	  ld .. local data consistentency
	  ns .. network send
	  nr .. network receive
	  dw .. disk write
	  dr .. disk read
	  pe .. pending (waiting for ack)
	  ua .. unack'd (still need to send ack)
	  al .. access log write count
	*/

	for (i = 0; i < minor_count; i++) {
		mdev = minor_to_mdev(i);
		if (!mdev) {
			hole = 1;
			continue;
		}
		if (hole) {
			hole = 0;
			seq_printf( seq, "\n");
		}

		sn = conns_to_name(mdev->state.conn);

		if ( mdev->state.conn == StandAlone &&
		     mdev->state.disk == Diskless) {
			seq_printf( seq, "%2d: cs:Unconfigured\n", i);
		} else {
			seq_printf( seq,
			   "%2d: cs:%s st:%s/%s ds:%s/%s %c %c%c%c%c\n"
			   "    ns:%u nr:%u dw:%u dr:%u al:%u bm:%u "
			   "lo:%d pe:%d ua:%d ap:%d",
			   i, sn,
			   roles_to_name(mdev->state.role),
			   roles_to_name(mdev->state.peer),
			   disks_to_name(mdev->state.disk),
			   disks_to_name(mdev->state.pdsk),
			   (mdev->net_conf == NULL ? ' ' :
			    (mdev->net_conf->wire_protocol - DRBD_PROT_A+'A')),
			   mdev->state.susp ? 's' : 'r',
			   mdev->state.aftr_isp ? 'a' : '-',
			   mdev->state.peer_isp ? 'p' : '-',
			   mdev->state.user_isp ? 'u' : '-',
			   mdev->send_cnt/2,
			   mdev->recv_cnt/2,
			   mdev->writ_cnt/2,
			   mdev->read_cnt/2,
			   mdev->al_writ_cnt,
			   mdev->bm_writ_cnt,
			   atomic_read(&mdev->local_cnt),
			   atomic_read(&mdev->ap_pending_cnt) +
			   atomic_read(&mdev->rs_pending_cnt),
			   atomic_read(&mdev->unacked_cnt),
			   atomic_read(&mdev->ap_bio_cnt)
			);
			seq_printf(seq, " oos:%lu\n",
				   drbd_bm_total_weight(mdev) << (BM_BLOCK_SIZE_B - 10));
		}
		if ( mdev->state.conn == SyncSource ||
		     mdev->state.conn == SyncTarget )
			drbd_syncer_progress(mdev, seq);

		if ( mdev->state.conn == VerifyS ||
		     mdev->state.conn == VerifyT ) {
			seq_printf(seq,"\t%3d%%      %lu/%lu\n",
				   (int)((mdev->rs_total-mdev->ov_left) /
					 (mdev->rs_total/100+1)),
				   mdev->rs_total - mdev->ov_left,
				   mdev->rs_total);
		}

#ifdef ENABLE_DYNAMIC_TRACE
		if (proc_details >= 1) {
			if (mdev->resync)
				lc_printf_stats(seq, mdev->resync);

			if (mdev->act_log)
				lc_printf_stats(seq, mdev->act_log);
		}

		if (proc_details >= 2) {
			if (mdev->resync) {
				lc_dump(mdev->resync, seq, "rs_left",
					resync_dump_detail);
			}
		}
#endif
	}

	return 0;
}

int drbd_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, drbd_seq_show, PDE(inode)->data);
}

/* PROC FS stuff end */
