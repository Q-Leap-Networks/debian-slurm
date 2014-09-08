/*****************************************************************************\
 *  bluegene.c - blue gene node configuration processing module.
 *
 *  $Id: bluegene.c 22587 2011-03-01 21:19:25Z da $
 *****************************************************************************
 *  Copyright (C) 2004 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <auble1@llnl.gov> et. al.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "bluegene.h"
#include "defined_block.h"
#include "src/slurmctld/locks.h"

#define MMCS_POLL_TIME 30	/* seconds between poll of MMCS for
				 * down switches and nodes */
#define BG_POLL_TIME 1	        /* seconds between poll of state
				 * change in bg blocks */
#define MAX_FREE_RETRIES           200 /* max number of
					* FREE_SLEEP_INTERVALS to wait
					* before putting a
					* deallocating block into
					* error state.
					*/
#define FREE_SLEEP_INTERVAL        3 /* When freeing a block wait this
				      * long before looking at state
				      * again.
				      */

#define _DEBUG 0

typedef struct {
	List track_list;
	uint32_t job_id;
	bool destroy;
} bg_free_block_list_t;

/* Global variables */

bg_config_t *bg_conf = NULL;
bg_lists_t *bg_lists = NULL;
bool agent_fini = false;
time_t last_bg_update;
pthread_mutex_t block_state_mutex = PTHREAD_MUTEX_INITIALIZER;
int blocks_are_created = 0;
int num_unused_cpus = 0;

static void _destroy_bg_config(bg_config_t *bg_conf);
static void _destroy_bg_lists(bg_lists_t *bg_lists);

static void _set_bg_lists();
static int  _validate_config_nodes(List curr_block_list,
				   List found_block_list, char *dir);
static int _delete_old_blocks(List curr_block_list,
			      List found_block_list);
static int _post_block_free(bg_record_t *bg_record, bool restore);
static void *_track_freeing_blocks(void *args);
static char *_get_bg_conf(void);
static int  _reopen_bridge_log(void);
static void _destroy_bitmap(void *object);


/* Initialize all plugin variables */
extern int init_bg(void)
{
	_set_bg_lists();

	if (!bg_conf)
		bg_conf = xmalloc(sizeof(bg_config_t));

	xfree(bg_conf->slurm_user_name);
	xfree(bg_conf->slurm_node_prefix);
	slurm_conf_lock();
	xassert(slurmctld_conf.slurm_user_name);
	xassert(slurmctld_conf.node_prefix);
	bg_conf->slurm_user_name = xstrdup(slurmctld_conf.slurm_user_name);
	bg_conf->slurm_node_prefix = xstrdup(slurmctld_conf.node_prefix);
	bg_conf->slurm_debug_flags = slurmctld_conf.debug_flags;
	slurm_conf_unlock();

#ifdef HAVE_BGL
	if (bg_conf->blrts_list)
		list_destroy(bg_conf->blrts_list);
	bg_conf->blrts_list = list_create(destroy_image);
#endif
	if (bg_conf->linux_list)
		list_destroy(bg_conf->linux_list);
	bg_conf->linux_list = list_create(destroy_image);
	if (bg_conf->mloader_list)
		list_destroy(bg_conf->mloader_list);
	bg_conf->mloader_list = list_create(destroy_image);
	if (bg_conf->ramdisk_list)
		list_destroy(bg_conf->ramdisk_list);
	bg_conf->ramdisk_list = list_create(destroy_image);

	ba_init(NULL, 1);

	verbose("BlueGene plugin loaded successfully");

	return SLURM_SUCCESS;
}

/* Purge all plugin variables */
extern void fini_bg(void)
{
	if (!agent_fini) {
		error("The agent hasn't been finied yet!");
		agent_fini = true;
	}

	_destroy_bg_config(bg_conf);
	_destroy_bg_lists(bg_lists);

	ba_fini();
}

/*
 * block_state_mutex should be locked before calling this function
 */
extern bool blocks_overlap(bg_record_t *rec_a, bg_record_t *rec_b)
{
	if ((rec_a->bp_count > 1) && (rec_b->bp_count > 1)) {
		/* Test for conflicting passthroughs */
		reset_ba_system(false);
		check_and_set_node_list(rec_a->bg_block_list);
		if (check_and_set_node_list(rec_b->bg_block_list)
		    == SLURM_ERROR)
			return true;
	}

	if (rec_a->bitmap && rec_b->bitmap
	    && !bit_overlap(rec_a->bitmap, rec_b->bitmap))
		return false;

	if ((rec_a->node_cnt >= bg_conf->bp_node_cnt)
	    || (rec_b->node_cnt >= bg_conf->bp_node_cnt))
		return true;

	if (rec_a->ionode_bitmap && rec_b->ionode_bitmap
	    && !bit_overlap(rec_a->ionode_bitmap, rec_b->ionode_bitmap))
		return false;

	return true;
}

/* block_state_mutex must be unlocked before calling this. */
extern void bg_requeue_job(uint32_t job_id, bool wait_for_start)
{
	int rc;
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };

	/* Wait for the slurmd to begin the batch script, slurm_fail_job()
	   is a no-op if issued prior to the script initiation do
	   clean up just incase the fail job isn't ran. */
	if (wait_for_start)
		sleep(2);

	lock_slurmctld(job_write_lock);
	if ((rc = job_requeue(0, job_id, -1, (uint16_t)NO_VAL))) {
		error("Couldn't requeue job %u, failing it: %s",
		      job_id, slurm_strerror(rc));
		job_fail(job_id);
	}
	unlock_slurmctld(job_write_lock);
}

extern int remove_all_users(char *bg_block_id, char *user_name)
{
	int returnc = REMOVE_USER_NONE;
#ifdef HAVE_BG_FILES
	char *user;
	rm_partition_t *block_ptr = NULL;
	int rc, i, user_count;

	/* We can't use bridge_get_block_info here because users are
	   filled in there.  This function is very slow but necessary
	   here to get the correct block count and the users. */
	if ((rc = bridge_get_block(bg_block_id, &block_ptr)) != STATUS_OK) {
		if (rc == INCONSISTENT_DATA
		    && bg_conf->layout_mode == LAYOUT_DYNAMIC)
			return REMOVE_USER_FOUND;

		error("bridge_get_block(%s): %s",
		      bg_block_id,
		      bg_err_str(rc));
		return REMOVE_USER_ERR;
	}

	if ((rc = bridge_get_data(block_ptr, RM_PartitionUsersNum,
				  &user_count))
	    != STATUS_OK) {
		error("bridge_get_data(RM_PartitionUsersNum): %s",
		      bg_err_str(rc));
		returnc = REMOVE_USER_ERR;
		user_count = 0;
	} else
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("got %d users for %s", user_count, bg_block_id);
	for(i=0; i<user_count; i++) {
		if (i) {
			if ((rc = bridge_get_data(block_ptr,
						  RM_PartitionNextUser,
						  &user))
			    != STATUS_OK) {
				error("bridge_get_data"
				      "(RM_PartitionNextUser): %s",
				      bg_err_str(rc));
				returnc = REMOVE_USER_ERR;
				break;
			}
		} else {
			if ((rc = bridge_get_data(block_ptr,
						  RM_PartitionFirstUser,
						  &user))
			    != STATUS_OK) {
				error("bridge_get_data"
				      "(RM_PartitionFirstUser): %s",
				      bg_err_str(rc));
				returnc = REMOVE_USER_ERR;
				break;
			}
		}
		if (!user) {
			error("No user was returned from database");
			continue;
		}
		if (!strcmp(user, bg_conf->slurm_user_name)) {
			free(user);
			continue;
		}

		if (user_name) {
			if (!strcmp(user, user_name)) {
				returnc = REMOVE_USER_FOUND;
				free(user);
				continue;
			}
		}

		info("Removing user %s from Block %s", user, bg_block_id);
		if ((rc = bridge_remove_block_user(bg_block_id, user))
		    != STATUS_OK) {
			debug("user %s isn't on block %s",
			      user,
			      bg_block_id);
		}
		free(user);
	}
	if ((rc = bridge_free_block(block_ptr)) != STATUS_OK) {
		error("bridge_free_block(): %s", bg_err_str(rc));
	}
#endif
	return returnc;
}

/* if SLURM_ERROR you will need to fail the job with
   slurm_fail_job(bg_record->job_running);
*/

extern int set_block_user(bg_record_t *bg_record)
{
	int rc = 0;
	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		info("resetting the boot state flag and "
		     "counter for block %s.",
		     bg_record->bg_block_id);
	bg_record->boot_state = 0;
	bg_record->boot_count = 0;

	if ((rc = update_block_user(bg_record, 1)) == 1) {
		last_bg_update = time(NULL);
		rc = SLURM_SUCCESS;
	} else if (rc == -1) {
		error("Unable to add user name to block %s. "
		      "Cancelling job.",
		      bg_record->bg_block_id);
		rc = SLURM_ERROR;
	}
	xfree(bg_record->target_name);
	bg_record->target_name = xstrdup(bg_conf->slurm_user_name);

	return rc;
}

/**
 * sort the partitions by increasing size
 */
extern void sort_bg_record_inc_size(List records){
	if (records == NULL)
		return;
	list_sort(records, (ListCmpF) bg_record_cmpf_inc);
	last_bg_update = time(NULL);
}

/*
 * block_agent - thread periodically updates status of
 * bluegene blocks.
 *
 */
extern void *block_agent(void *args)
{
	static time_t last_bg_test;
	int rc;
	time_t now = time(NULL);

	last_bg_test = now - BG_POLL_TIME;
	while (!agent_fini) {
		if (difftime(now, last_bg_test) >= BG_POLL_TIME) {
			if (agent_fini)		/* don't bother */
				break;	/* quit now */
			if (blocks_are_created) {
				last_bg_test = now;
				if ((rc = update_block_list()) == 1)
					last_bg_update = now;
				else if (rc == -1)
					error("Error with update_block_list");
			}
		}

		sleep(1);
		now = time(NULL);
	}
	return NULL;
}

/*
 * state_agent - thread periodically updates status of
 * bluegene nodes.
 *
 */
extern void *state_agent(void *args)
{
	static time_t last_mmcs_test;
	time_t now = time(NULL);

	last_mmcs_test = now - MMCS_POLL_TIME;
	while (!agent_fini) {
		if (difftime(now, last_mmcs_test) >= MMCS_POLL_TIME) {
			if (agent_fini)		/* don't bother */
				break; 	/* quit now */
			if (blocks_are_created) {
				/* can run for a while so set the
				 * time after the call so there is
				 * always MMCS_POLL_TIME between
				 * calls */
				test_mmcs_failures();
				last_mmcs_test = time(NULL);
			}
		}

		sleep(1);
		now = time(NULL);
	}
	return NULL;
}

/* must set the protecting mutex if any before this function is called */

extern int remove_from_bg_list(List my_bg_list, bg_record_t *bg_record)
{
	bg_record_t *found_record = NULL;
	ListIterator itr;
	int rc = SLURM_ERROR;

	if (!bg_record)
		return rc;

	//slurm_mutex_lock(&block_state_mutex);
	itr = list_iterator_create(my_bg_list);
	while ((found_record = list_next(itr))) {
		if (found_record)
			if (bg_record == found_record) {
				list_remove(itr);
				rc = SLURM_SUCCESS;
				break;
			}
	}
	list_iterator_destroy(itr);
	//slurm_mutex_unlock(&block_state_mutex);

	return rc;
}

/* This is here to remove from the orignal list when dealing with
 * copies like above all locks need to be set.  This function does not
 * free anything you must free it when you are done */
extern bg_record_t *find_and_remove_org_from_bg_list(List my_list,
						     bg_record_t *bg_record)
{
	ListIterator itr = list_iterator_create(my_list);
	bg_record_t *found_record = NULL;

	while ((found_record = (bg_record_t *) list_next(itr)) != NULL) {
		/* check for full node bitmap compare */
		if (bit_equal(bg_record->bitmap, found_record->bitmap)
		    && bit_equal(bg_record->ionode_bitmap,
				 found_record->ionode_bitmap)) {
			if (!strcmp(bg_record->bg_block_id,
				    found_record->bg_block_id)) {
				list_remove(itr);
				if (bg_conf->slurm_debug_flags
				    & DEBUG_FLAG_SELECT_TYPE)
					info("got the block");
				break;
			}
		}
	}
	list_iterator_destroy(itr);
	return found_record;
}

/* This is here to remove from the orignal list when dealing with
 * copies like above all locks need to be set */
extern bg_record_t *find_org_in_bg_list(List my_list, bg_record_t *bg_record)
{
	ListIterator itr = list_iterator_create(my_list);
	bg_record_t *found_record = NULL;

	while ((found_record = (bg_record_t *) list_next(itr)) != NULL) {
		/* check for full node bitmap compare */
		if (bit_equal(bg_record->bitmap, found_record->bitmap)
		    && bit_equal(bg_record->ionode_bitmap,
				 found_record->ionode_bitmap)) {

			if (!strcmp(bg_record->bg_block_id,
				    found_record->bg_block_id)) {
				if (bg_conf->slurm_debug_flags
				    & DEBUG_FLAG_SELECT_TYPE)
					info("got the block");
				break;
			}
		}
	}
	list_iterator_destroy(itr);
	return found_record;
}

extern int bg_free_block(bg_record_t *bg_record, bool wait, bool locked)
{
	int rc = SLURM_SUCCESS;
	int count = 0;

	if (!bg_record) {
		error("bg_free_block: there was no bg_record");
		return SLURM_ERROR;
	}

	if (!locked)
		slurm_mutex_lock(&block_state_mutex);

	while (count < MAX_FREE_RETRIES) {
		/* block was removed */
		if (bg_record->magic != BLOCK_MAGIC) {
			error("block was removed while freeing it here");
			if (!locked)
				slurm_mutex_unlock(&block_state_mutex);
			return SLURM_SUCCESS;
		}
		/* Reset these here so we don't try to reboot it
		   when the state goes to free.
		*/
		bg_record->boot_state = 0;
		bg_record->boot_count = 0;
		/* Here we don't need to check if the block is still
		 * in exsistance since this function can't be called on
		 * the same block twice.  It may
		 * had already been removed at this point also.
		 */
#ifdef HAVE_BG_FILES
		if (bg_record->state != NO_VAL
		    && bg_record->state != RM_PARTITION_FREE
		    && bg_record->state != RM_PARTITION_DEALLOCATING) {
			if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
				info("bridge_destroy %s",
				     bg_record->bg_block_id);
			rc = bridge_destroy_block(bg_record->bg_block_id);
			if (rc != STATUS_OK) {
				if (rc == PARTITION_NOT_FOUND) {
					debug("block %s is not found",
					      bg_record->bg_block_id);
					break;
				} else if (rc == INCOMPATIBLE_STATE) {
#ifndef HAVE_BGL
					/* If the state is error and
					   we get an incompatible
					   state back here, it means
					   we set it ourselves so
					   break out.
					*/
					if (bg_record->state
					    == RM_PARTITION_ERROR)
						break;
#endif
					if (bg_conf->slurm_debug_flags
					    & DEBUG_FLAG_SELECT_TYPE)
						info("bridge_destroy_partition"
						     "(%s): %s State = %d",
						     bg_record->bg_block_id,
						     bg_err_str(rc),
						     bg_record->state);
				} else {
					error("bridge_destroy_partition"
					      "(%s): %s State = %d",
					      bg_record->bg_block_id,
					      bg_err_str(rc),
					      bg_record->state);
				}
			}
		}
#else
		/* Fake a free since we are n deallocating
		   state before this.
		*/
		if (bg_record->state == RM_PARTITION_ERROR)
			break;
		else if (count >= 3)
			bg_record->state = RM_PARTITION_FREE;
		else if (bg_record->state != RM_PARTITION_FREE)
			bg_record->state = RM_PARTITION_DEALLOCATING;
#endif

		if (!wait || (bg_record->state == RM_PARTITION_FREE)
#ifdef HAVE_BGL
		    ||  (bg_record->state == RM_PARTITION_ERROR)
#endif
			) {
			break;
		}
		/* If we were locked outside of this we need to unlock
		   to not cause deadlock on this mutex until we are
		   done.
		*/
		slurm_mutex_unlock(&block_state_mutex);
		sleep(FREE_SLEEP_INTERVAL);
		count++;
		slurm_mutex_lock(&block_state_mutex);
	}

	rc = SLURM_SUCCESS;
	if ((bg_record->state == RM_PARTITION_FREE)
	    || (bg_record->state == RM_PARTITION_ERROR))
		remove_from_bg_list(bg_lists->booted, bg_record);
	else if (count >= MAX_FREE_RETRIES) {
		/* Something isn't right, go mark this one in an error
		   state. */
		update_block_msg_t block_msg;
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("bg_free_block: block %s is not in state "
			     "free (%s), putting it in error state.",
			     bg_record->bg_block_id,
			     bg_block_state_string(bg_record->state));
		slurm_init_update_block_msg(&block_msg);
		block_msg.bg_block_id = bg_record->bg_block_id;
		block_msg.state = RM_PARTITION_ERROR;
		block_msg.reason = "Block would not deallocate";
		slurm_mutex_unlock(&block_state_mutex);
		select_p_update_block(&block_msg);
		slurm_mutex_lock(&block_state_mutex);
		rc = SLURM_ERROR;
	}
	if (!locked)
		slurm_mutex_unlock(&block_state_mutex);

	return rc;
}

/* block_state_mutex should be unlocked before calling this */
extern int free_block_list(uint32_t job_id, List track_in_list,
			   bool destroy, bool wait)
{
	bg_record_t *bg_record = NULL;
	int retries;
	ListIterator itr = NULL;
	bg_free_block_list_t *bg_free_list;
	pthread_attr_t attr_agent;
	pthread_t thread_agent;

	if (!track_in_list || !list_count(track_in_list))
		return SLURM_SUCCESS;

	bg_free_list = xmalloc(sizeof(bg_free_block_list_t));
	bg_free_list->track_list = list_create(NULL);
	bg_free_list->destroy = destroy;
	bg_free_list->job_id = job_id;

	slurm_mutex_lock(&block_state_mutex);
	list_transfer(bg_free_list->track_list, track_in_list);
	itr = list_iterator_create(bg_free_list->track_list);
	while ((bg_record = list_next(itr))) {
		if (bg_record->magic != BLOCK_MAGIC) {
			error("block was already destroyed");
			continue;
		}

		bg_record->free_cnt++;

		if (bg_record->job_ptr
		    && !IS_JOB_FINISHED(bg_record->job_ptr)) {
			info("We are freeing a block (%s) that has job %u(%u).",
			     bg_record->bg_block_id,
			     bg_record->job_ptr->job_id,
			     bg_record->job_running);
			/* This is not thread safe if called from
			   bg_job_place.c anywhere from within
			   submit_job() */
			slurm_mutex_unlock(&block_state_mutex);
			bg_requeue_job(bg_record->job_ptr->job_id, 0);
			slurm_mutex_lock(&block_state_mutex);
		}
		if (remove_from_bg_list(bg_lists->job_running, bg_record)
		    == SLURM_SUCCESS)
			num_unused_cpus += bg_record->cpu_cnt;

		bg_free_block(bg_record, 0, 1);
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&block_state_mutex);

	if (wait) {
		/* Track_freeing_blocks waits until the list is done
		   and frees the memory of bg_free_list.
		*/
		_track_freeing_blocks(bg_free_list);
		return SLURM_SUCCESS;
	}

	/* _track_freeing_blocks handles cleanup */
	slurm_attr_init(&attr_agent);
	if (pthread_attr_setdetachstate(&attr_agent, PTHREAD_CREATE_DETACHED))
		error("pthread_attr_setdetachstate error %m");
	retries = 0;
	while (pthread_create(&thread_agent, &attr_agent,
			      _track_freeing_blocks,
			      bg_free_list)) {
		error("pthread_create error %m");
		if (++retries > MAX_PTHREAD_RETRIES)
			fatal("Can't create "
			      "pthread");
		/* sleep and retry */
		usleep(1000);
	}
	slurm_attr_destroy(&attr_agent);
	return SLURM_SUCCESS;
}

/*
 * Read and process the bluegene.conf configuration file so to interpret what
 * blocks are static/dynamic, torus/mesh, etc.
 */
extern int read_bg_conf(void)
{
	int i;
	int count = 0;
	s_p_hashtbl_t *tbl = NULL;
	char *layout = NULL;
	blockreq_t **blockreq_array = NULL;
	image_t **image_array = NULL;
	image_t *image = NULL;
	static time_t last_config_update = (time_t) 0;
	struct stat config_stat;
	ListIterator itr = NULL;
	char* bg_conf_file = NULL;

	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		info("Reading the bluegene.conf file");

	/* check if config file has changed */
	bg_conf_file = _get_bg_conf();

	if (stat(bg_conf_file, &config_stat) < 0)
		fatal("can't stat bluegene.conf file %s: %m", bg_conf_file);
	if (last_config_update) {
		_reopen_bridge_log();
		if (last_config_update == config_stat.st_mtime) {
			if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
				info("%s unchanged", bg_conf_file);
		} else {
			info("Restart slurmctld for %s changes "
			     "to take effect",
			     bg_conf_file);
		}
		last_config_update = config_stat.st_mtime;
		xfree(bg_conf_file);
		return SLURM_SUCCESS;
	}
	last_config_update = config_stat.st_mtime;

	/* initialization */
	/* bg_conf defined in bg_node_alloc.h */
	tbl = s_p_hashtbl_create(bg_conf_file_options);

	if (s_p_parse_file(tbl, NULL, bg_conf_file) == SLURM_ERROR)
		fatal("something wrong with opening/reading bluegene "
		      "conf file");
	xfree(bg_conf_file);

#ifdef HAVE_BGL
	if (s_p_get_array((void ***)&image_array,
			  &count, "AltBlrtsImage", tbl)) {
		for (i = 0; i < count; i++) {
			list_append(bg_conf->blrts_list, image_array[i]);
			image_array[i] = NULL;
		}
	}
	if (!s_p_get_string(&bg_conf->default_blrtsimage, "BlrtsImage", tbl)) {
		if (!list_count(bg_conf->blrts_list))
			fatal("BlrtsImage not configured "
			      "in bluegene.conf");
		itr = list_iterator_create(bg_conf->blrts_list);
		image = list_next(itr);
		image->def = true;
		list_iterator_destroy(itr);
		bg_conf->default_blrtsimage = xstrdup(image->name);
		info("Warning: using %s as the default BlrtsImage.  "
		     "If this isn't correct please set BlrtsImage",
		     bg_conf->default_blrtsimage);
	} else {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("default BlrtsImage %s",
			     bg_conf->default_blrtsimage);
		image = xmalloc(sizeof(image_t));
		image->name = xstrdup(bg_conf->default_blrtsimage);
		image->def = true;
		image->groups = NULL;
		/* we want it to be first */
		list_push(bg_conf->blrts_list, image);
	}

	if (s_p_get_array((void ***)&image_array,
			  &count, "AltLinuxImage", tbl)) {
		for (i = 0; i < count; i++) {
			list_append(bg_conf->linux_list, image_array[i]);
			image_array[i] = NULL;
		}
	}
	if (!s_p_get_string(&bg_conf->default_linuximage, "LinuxImage", tbl)) {
		if (!list_count(bg_conf->linux_list))
			fatal("LinuxImage not configured "
			      "in bluegene.conf");
		itr = list_iterator_create(bg_conf->linux_list);
		image = list_next(itr);
		image->def = true;
		list_iterator_destroy(itr);
		bg_conf->default_linuximage = xstrdup(image->name);
		info("Warning: using %s as the default LinuxImage.  "
		     "If this isn't correct please set LinuxImage",
		     bg_conf->default_linuximage);
	} else {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("default LinuxImage %s",
			     bg_conf->default_linuximage);
		image = xmalloc(sizeof(image_t));
		image->name = xstrdup(bg_conf->default_linuximage);
		image->def = true;
		image->groups = NULL;
		/* we want it to be first */
		list_push(bg_conf->linux_list, image);
	}

	if (s_p_get_array((void ***)&image_array,
			  &count, "AltRamDiskImage", tbl)) {
		for (i = 0; i < count; i++) {
			list_append(bg_conf->ramdisk_list, image_array[i]);
			image_array[i] = NULL;
		}
	}
	if (!s_p_get_string(&bg_conf->default_ramdiskimage,
			    "RamDiskImage", tbl)) {
		if (!list_count(bg_conf->ramdisk_list))
			fatal("RamDiskImage not configured "
			      "in bluegene.conf");
		itr = list_iterator_create(bg_conf->ramdisk_list);
		image = list_next(itr);
		image->def = true;
		list_iterator_destroy(itr);
		bg_conf->default_ramdiskimage = xstrdup(image->name);
		info("Warning: using %s as the default RamDiskImage.  "
		     "If this isn't correct please set RamDiskImage",
		     bg_conf->default_ramdiskimage);
	} else {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("default RamDiskImage %s",
			     bg_conf->default_ramdiskimage);
		image = xmalloc(sizeof(image_t));
		image->name = xstrdup(bg_conf->default_ramdiskimage);
		image->def = true;
		image->groups = NULL;
		/* we want it to be first */
		list_push(bg_conf->ramdisk_list, image);
	}
#else

	if (s_p_get_array((void ***)&image_array,
			  &count, "AltCnloadImage", tbl)) {
		for (i = 0; i < count; i++) {
			list_append(bg_conf->linux_list, image_array[i]);
			image_array[i] = NULL;
		}
	}
	if (!s_p_get_string(&bg_conf->default_linuximage, "CnloadImage", tbl)) {
		if (!list_count(bg_conf->linux_list))
			fatal("CnloadImage not configured "
			      "in bluegene.conf");
		itr = list_iterator_create(bg_conf->linux_list);
		image = list_next(itr);
		image->def = true;
		list_iterator_destroy(itr);
		bg_conf->default_linuximage = xstrdup(image->name);
		info("Warning: using %s as the default CnloadImage.  "
		     "If this isn't correct please set CnloadImage",
		     bg_conf->default_linuximage);
	} else {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("default CnloadImage %s",
			     bg_conf->default_linuximage);
		image = xmalloc(sizeof(image_t));
		image->name = xstrdup(bg_conf->default_linuximage);
		image->def = true;
		image->groups = NULL;
		/* we want it to be first */
		list_push(bg_conf->linux_list, image);
	}

	if (s_p_get_array((void ***)&image_array,
			  &count, "AltIoloadImage", tbl)) {
		for (i = 0; i < count; i++) {
			list_append(bg_conf->ramdisk_list, image_array[i]);
			image_array[i] = NULL;
		}
	}
	if (!s_p_get_string(&bg_conf->default_ramdiskimage,
			    "IoloadImage", tbl)) {
		if (!list_count(bg_conf->ramdisk_list))
			fatal("IoloadImage not configured "
			      "in bluegene.conf");
		itr = list_iterator_create(bg_conf->ramdisk_list);
		image = list_next(itr);
		image->def = true;
		list_iterator_destroy(itr);
		bg_conf->default_ramdiskimage = xstrdup(image->name);
		info("Warning: using %s as the default IoloadImage.  "
		     "If this isn't correct please set IoloadImage",
		     bg_conf->default_ramdiskimage);
	} else {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("default IoloadImage %s",
			     bg_conf->default_ramdiskimage);
		image = xmalloc(sizeof(image_t));
		image->name = xstrdup(bg_conf->default_ramdiskimage);
		image->def = true;
		image->groups = NULL;
		/* we want it to be first */
		list_push(bg_conf->ramdisk_list, image);
	}

#endif
	if (s_p_get_array((void ***)&image_array,
			  &count, "AltMloaderImage", tbl)) {
		for (i = 0; i < count; i++) {
			list_append(bg_conf->mloader_list, image_array[i]);
			image_array[i] = NULL;
		}
	}
	if (!s_p_get_string(&bg_conf->default_mloaderimage,
			    "MloaderImage", tbl)) {
		if (!list_count(bg_conf->mloader_list))
			fatal("MloaderImage not configured "
			      "in bluegene.conf");
		itr = list_iterator_create(bg_conf->mloader_list);
		image = list_next(itr);
		image->def = true;
		list_iterator_destroy(itr);
		bg_conf->default_mloaderimage = xstrdup(image->name);
		info("Warning: using %s as the default MloaderImage.  "
		     "If this isn't correct please set MloaderImage",
		     bg_conf->default_mloaderimage);
	} else {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("default MloaderImage %s",
			     bg_conf->default_mloaderimage);
		image = xmalloc(sizeof(image_t));
		image->name = xstrdup(bg_conf->default_mloaderimage);
		image->def = true;
		image->groups = NULL;
		/* we want it to be first */
		list_push(bg_conf->mloader_list, image);
	}

	if (!s_p_get_uint16(
		    &bg_conf->bp_node_cnt, "BasePartitionNodeCnt", tbl)) {
		error("BasePartitionNodeCnt not configured in bluegene.conf "
		      "defaulting to 512 as BasePartitionNodeCnt");
		bg_conf->bp_node_cnt = 512;
		bg_conf->quarter_node_cnt = 128;
	} else {
		if (bg_conf->bp_node_cnt <= 0)
			fatal("You should have more than 0 nodes "
			      "per base partition");

		bg_conf->quarter_node_cnt = bg_conf->bp_node_cnt/4;
	}
	/* bg_conf->cpus_per_bp should had already been set from the
	 * node_init */
	if (bg_conf->cpus_per_bp < bg_conf->bp_node_cnt) {
		fatal("For some reason we have only %u cpus per bp, but "
		      "have %u cnodes per bp.  You need at least the same "
		      "number of cpus as you have cnodes per bp.  "
		      "Check the NodeName Procs= "
		      "definition in the slurm.conf.",
		      bg_conf->cpus_per_bp, bg_conf->bp_node_cnt);
	}

	bg_conf->cpu_ratio = bg_conf->cpus_per_bp/bg_conf->bp_node_cnt;
	if (!bg_conf->cpu_ratio)
		fatal("We appear to have less than 1 cpu on a cnode.  "
		      "You specified %u for BasePartitionNodeCnt "
		      "in the blugene.conf and %u cpus "
		      "for each node in the slurm.conf",
		      bg_conf->bp_node_cnt, bg_conf->cpus_per_bp);
	num_unused_cpus =
		DIM_SIZE[X] * DIM_SIZE[Y] * DIM_SIZE[Z]
		* bg_conf->cpus_per_bp;

	if (!s_p_get_uint16(
		    &bg_conf->nodecard_node_cnt, "NodeCardNodeCnt", tbl)) {
		error("NodeCardNodeCnt not configured in bluegene.conf "
		      "defaulting to 32 as NodeCardNodeCnt");
		bg_conf->nodecard_node_cnt = 32;
	}

	if (bg_conf->nodecard_node_cnt<=0)
		fatal("You should have more than 0 nodes per nodecard");

	bg_conf->bp_nodecard_cnt =
		bg_conf->bp_node_cnt / bg_conf->nodecard_node_cnt;

	if (!s_p_get_uint16(&bg_conf->numpsets, "Numpsets", tbl))
		fatal("Warning: Numpsets not configured in bluegene.conf");

	if (bg_conf->numpsets) {
		bitstr_t *tmp_bitmap = NULL;
		int small_size = 1;

		/* THIS IS A HACK TO MAKE A 1 NODECARD SYSTEM WORK */
		if (bg_conf->bp_node_cnt == bg_conf->nodecard_node_cnt) {
			bg_conf->quarter_ionode_cnt = 2;
			bg_conf->nodecard_ionode_cnt = 2;
		} else {
			bg_conf->quarter_ionode_cnt = bg_conf->numpsets/4;
			bg_conf->nodecard_ionode_cnt =
				bg_conf->quarter_ionode_cnt/4;
		}

		/* How many nodecards per ionode */
		bg_conf->nc_ratio =
			((double)bg_conf->bp_node_cnt
			 / (double)bg_conf->nodecard_node_cnt)
			/ (double)bg_conf->numpsets;
		/* How many ionodes per nodecard */
		bg_conf->io_ratio =
			(double)bg_conf->numpsets /
			((double)bg_conf->bp_node_cnt
			 / (double)bg_conf->nodecard_node_cnt);
		//info("got %f %f", bg_conf->nc_ratio, bg_conf->io_ratio);
		/* figure out the smallest block we can have on the
		   system */
#ifdef HAVE_BGL
		if (bg_conf->io_ratio >= 1)
			bg_conf->smallest_block=32;
		else
			bg_conf->smallest_block=128;
#else
		if (bg_conf->io_ratio >= 2)
			bg_conf->smallest_block=16;
		else if (bg_conf->io_ratio == 1)
			bg_conf->smallest_block=32;
		else if (bg_conf->io_ratio == .5)
			bg_conf->smallest_block=64;
		else if (bg_conf->io_ratio == .25)
			bg_conf->smallest_block=128;
		else if (bg_conf->io_ratio == .125)
			bg_conf->smallest_block=256;
		else {
			error("unknown ioratio %f.  Can't figure out "
			      "smallest block size, setting it to midplane",
			      bg_conf->io_ratio);
			bg_conf->smallest_block = 512;
		}
#endif
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("Smallest block possible on this system is %u",
			     bg_conf->smallest_block);
		/* below we are creating all the possible bitmaps for
		 * each size of small block
		 */
		if ((int)bg_conf->nodecard_ionode_cnt < 1) {
			bg_conf->nodecard_ionode_cnt = 0;
		} else {
			bg_lists->valid_small32 = list_create(_destroy_bitmap);
			if ((small_size = bg_conf->nodecard_ionode_cnt))
				small_size--;
			i = 0;
			while (i<bg_conf->numpsets) {
				tmp_bitmap = bit_alloc(bg_conf->numpsets);
				bit_nset(tmp_bitmap, i, i+small_size);
				i += small_size+1;
				list_append(bg_lists->valid_small32,
					    tmp_bitmap);
			}
		}
		/* If we only have 1 nodecard just jump to the end
		   since this will never need to happen below.
		   Pretty much a hack to avoid seg fault;). */
		if (bg_conf->bp_node_cnt == bg_conf->nodecard_node_cnt)
			goto no_calc;

		bg_lists->valid_small128 = list_create(_destroy_bitmap);
		if ((small_size = bg_conf->quarter_ionode_cnt))
			small_size--;
		i = 0;
		while (i<bg_conf->numpsets) {
			tmp_bitmap = bit_alloc(bg_conf->numpsets);
			bit_nset(tmp_bitmap, i, i+small_size);
			i += small_size+1;
			list_append(bg_lists->valid_small128, tmp_bitmap);
		}

#ifndef HAVE_BGL
		bg_lists->valid_small64 = list_create(_destroy_bitmap);
		if ((small_size = bg_conf->nodecard_ionode_cnt * 2))
			small_size--;
		i = 0;
		while (i<bg_conf->numpsets) {
			tmp_bitmap = bit_alloc(bg_conf->numpsets);
			bit_nset(tmp_bitmap, i, i+small_size);
			i += small_size+1;
			list_append(bg_lists->valid_small64, tmp_bitmap);
		}

		bg_lists->valid_small256 = list_create(_destroy_bitmap);
		if ((small_size = bg_conf->quarter_ionode_cnt * 2))
			small_size--;
		i = 0;
		while (i<bg_conf->numpsets) {
			tmp_bitmap = bit_alloc(bg_conf->numpsets);
			bit_nset(tmp_bitmap, i, i+small_size);
			i += small_size+1;
			list_append(bg_lists->valid_small256, tmp_bitmap);
		}
#endif
	} else {
		fatal("your numpsets is 0");
	}

no_calc:

	if (!s_p_get_uint16(&bg_conf->bridge_api_verb, "BridgeAPIVerbose", tbl))
		info("Warning: BridgeAPIVerbose not configured "
		     "in bluegene.conf");
	if (!s_p_get_string(&bg_conf->bridge_api_file,
			    "BridgeAPILogFile", tbl))
		info("BridgeAPILogFile not configured in bluegene.conf");
	else
		_reopen_bridge_log();

	if (s_p_get_string(&layout, "DenyPassthrough", tbl)) {
		if (strstr(layout, "X"))
			ba_deny_pass |= PASS_DENY_X;
		if (strstr(layout, "Y"))
			ba_deny_pass |= PASS_DENY_Y;
		if (strstr(layout, "Z"))
			ba_deny_pass |= PASS_DENY_Z;
		if (!strcasecmp(layout, "ALL"))
			ba_deny_pass |= PASS_DENY_ALL;
		bg_conf->deny_pass = ba_deny_pass;
		xfree(layout);
	}

	if (!s_p_get_string(&layout, "LayoutMode", tbl)) {
		info("Warning: LayoutMode was not specified in bluegene.conf "
		     "defaulting to STATIC partitioning");
		bg_conf->layout_mode = LAYOUT_STATIC;
	} else {
		if (!strcasecmp(layout,"STATIC"))
			bg_conf->layout_mode = LAYOUT_STATIC;
		else if (!strcasecmp(layout,"OVERLAP"))
			bg_conf->layout_mode = LAYOUT_OVERLAP;
		else if (!strcasecmp(layout,"DYNAMIC"))
			bg_conf->layout_mode = LAYOUT_DYNAMIC;
		else {
			fatal("I don't understand this LayoutMode = %s",
			      layout);
		}
		xfree(layout);
	}

	/* add blocks defined in file */
	if (bg_conf->layout_mode != LAYOUT_DYNAMIC) {
		if (!s_p_get_array((void ***)&blockreq_array,
				   &count, "BPs", tbl)) {
			info("WARNING: no blocks defined in bluegene.conf, "
			     "only making full system block");
			create_full_system_block(NULL);
		}

		for (i = 0; i < count; i++) {
			add_bg_record(bg_lists->main, NULL,
				      blockreq_array[i], 0, 0);
		}
	}
	s_p_hashtbl_destroy(tbl);

	return SLURM_SUCCESS;
}

extern int validate_current_blocks(char *dir)
{
	/* found bg blocks already on system */
	List curr_block_list = NULL;
	List found_block_list = NULL;
	static time_t last_config_update = (time_t) 0;
	ListIterator itr = NULL;
	bg_record_t *bg_record = NULL;

	/* only run on startup */
	if (last_config_update)
		return SLURM_SUCCESS;

	last_config_update = time(NULL);
	curr_block_list = list_create(destroy_bg_record);
	found_block_list = list_create(NULL);
//#if 0
	/* Check to see if the configs we have are correct */
	if (_validate_config_nodes(curr_block_list, found_block_list, dir)
	    == SLURM_ERROR) {
		_delete_old_blocks(curr_block_list, found_block_list);
	}
//#endif
	/* looking for blocks only I created */
	if (bg_conf->layout_mode == LAYOUT_DYNAMIC) {
		init_wires();
		info("No blocks created until jobs are submitted");
	} else {
		if (create_defined_blocks(bg_conf->layout_mode,
					  found_block_list)
		    == SLURM_ERROR) {
			/* error in creating the static blocks, so
			 * blocks referenced by submitted jobs won't
			 * correspond to actual slurm blocks.
			 */
			fatal("Error, could not create the static blocks");
			return SLURM_ERROR;
		}
	}

	/* ok now since bg_lists->main has been made we now can put blocks in
	   an error state this needs to be done outside of a lock
	   it doesn't matter much in the first place though since
	   no threads are started before this function. */
	itr = list_iterator_create(bg_lists->main);
	while ((bg_record = list_next(itr))) {
		if (bg_record->state == RM_PARTITION_ERROR)
			put_block_in_error_state(bg_record,
						 BLOCK_ERROR_STATE, NULL);
	}
	list_iterator_destroy(itr);

	list_destroy(curr_block_list);
	curr_block_list = NULL;
	list_destroy(found_block_list);
	found_block_list = NULL;

	slurm_mutex_lock(&block_state_mutex);
	last_bg_update = time(NULL);
	sort_bg_record_inc_size(bg_lists->main);
	slurm_mutex_unlock(&block_state_mutex);
	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		info("Blocks have finished being created.");
	return SLURM_SUCCESS;
}

static void _destroy_bg_config(bg_config_t *bg_conf)
{
	if (bg_conf) {
#ifdef HAVE_BGL
		if (bg_conf->blrts_list) {
			list_destroy(bg_conf->blrts_list);
			bg_conf->blrts_list = NULL;
		}
		xfree(bg_conf->default_blrtsimage);
#endif
		xfree(bg_conf->bridge_api_file);
		xfree(bg_conf->default_linuximage);
		xfree(bg_conf->default_mloaderimage);
		xfree(bg_conf->default_ramdiskimage);
		if (bg_conf->linux_list) {
			list_destroy(bg_conf->linux_list);
			bg_conf->linux_list = NULL;
		}

		if (bg_conf->mloader_list) {
			list_destroy(bg_conf->mloader_list);
			bg_conf->mloader_list = NULL;
		}

		if (bg_conf->ramdisk_list) {
			list_destroy(bg_conf->ramdisk_list);
			bg_conf->ramdisk_list = NULL;
		}
		xfree(bg_conf->slurm_user_name);
		xfree(bg_conf->slurm_node_prefix);
		xfree(bg_conf);
	}
}

static void _destroy_bg_lists(bg_lists_t *bg_lists)
{
	if (bg_lists) {
		if (bg_lists->booted) {
			list_destroy(bg_lists->booted);
			bg_lists->booted = NULL;
		}

		if (bg_lists->job_running) {
			list_destroy(bg_lists->job_running);
			bg_lists->job_running = NULL;
			num_unused_cpus = 0;
		}

		if (bg_lists->main) {
			list_destroy(bg_lists->main);
			bg_lists->main = NULL;
		}

		if (bg_lists->valid_small32) {
			list_destroy(bg_lists->valid_small32);
			bg_lists->valid_small32 = NULL;
		}
		if (bg_lists->valid_small64) {
			list_destroy(bg_lists->valid_small64);
			bg_lists->valid_small64 = NULL;
		}
		if (bg_lists->valid_small128) {
			list_destroy(bg_lists->valid_small128);
			bg_lists->valid_small128 = NULL;
		}
		if (bg_lists->valid_small256) {
			list_destroy(bg_lists->valid_small256);
			bg_lists->valid_small256 = NULL;
		}

		xfree(bg_lists);
	}
}

static void _set_bg_lists()
{
	if (!bg_lists)
		bg_lists = xmalloc(sizeof(bg_lists_t));

	slurm_mutex_lock(&block_state_mutex);

	if (bg_lists->booted)
		list_destroy(bg_lists->booted);
	bg_lists->booted = list_create(NULL);

	if (bg_lists->job_running)
		list_destroy(bg_lists->job_running);
	bg_lists->job_running = list_create(NULL);

	if (bg_lists->main)
		list_destroy(bg_lists->main);
	bg_lists->main = list_create(destroy_bg_record);

	slurm_mutex_unlock(&block_state_mutex);

}

/*
 * _validate_config_nodes - Match slurm configuration information with
 *                          current BG block configuration.
 * IN/OUT curr_block_list -  List of blocks already existing on the system.
 * IN/OUT found_block_list - List of blocks found on the system
 *                              that are listed in the bluegene.conf.
 * NOTE: Both of the lists above should be created with list_create(NULL)
 *       since the bg_lists->main will contain the complete list of pointers
 *       and be destroyed with it.
 *
 * RET - SLURM_SUCCESS if they match, else an error
 * code. Writes bg_block_id into bg_lists->main records.
 */

static int _validate_config_nodes(List curr_block_list,
				  List found_block_list, char *dir)
{
	int rc = SLURM_ERROR;
	bg_record_t* bg_record = NULL;
	bg_record_t* init_bg_record = NULL;
	int full_created = 0;
	ListIterator itr_conf;
	ListIterator itr_curr;
	char tmp_char[256];

	xassert(curr_block_list);
	xassert(found_block_list);

#ifdef HAVE_BG_FILES
	/* read current bg block info into curr_block_list This
	 * happens in the state load before this in emulation mode */
	if (read_bg_blocks(curr_block_list) == SLURM_ERROR)
		return SLURM_ERROR;
	/* since we only care about error states here we don't care
	   about the return code this must be done after the bg_lists->main
	   is created */
	load_state_file(curr_block_list, dir);
#else
	/* read in state from last run. */
	if ((rc = load_state_file(curr_block_list, dir)) != SLURM_SUCCESS)
		return rc;
	/* This needs to be reset to SLURM_ERROR or it will never we
	   that way again ;). */
	rc = SLURM_ERROR;
#endif
	if (!bg_recover)
		return SLURM_ERROR;

	itr_curr = list_iterator_create(curr_block_list);
	itr_conf = list_iterator_create(bg_lists->main);
	while ((bg_record = list_next(itr_conf))) {
		list_iterator_reset(itr_curr);
		while ((init_bg_record = list_next(itr_curr))) {
			if (strcasecmp(bg_record->nodes,
				       init_bg_record->nodes))
				continue; /* wrong nodes */
			if (!bit_equal(bg_record->ionode_bitmap,
				       init_bg_record->ionode_bitmap))
				continue;
#ifdef HAVE_BGL
			if (bg_record->conn_type != init_bg_record->conn_type)
				continue; /* wrong conn_type */
#else
			if ((bg_record->conn_type != init_bg_record->conn_type)
			    && ((bg_record->conn_type < SELECT_SMALL)
				&& (init_bg_record->conn_type < SELECT_SMALL)))
				continue; /* wrong conn_type */
#endif

			copy_bg_record(init_bg_record, bg_record);
			/* remove from the curr list since we just
			   matched it no reason to keep it around
			   anymore */
			list_delete_item(itr_curr);
			break;
		}

		if (!bg_record->bg_block_id) {
			format_node_name(bg_record, tmp_char,
					 sizeof(tmp_char));
			info("Block found in bluegene.conf to be "
			     "created: Nodes:%s",
			     tmp_char);
			rc = SLURM_ERROR;
		} else {
			if (bg_record->full_block)
				full_created = 1;

			list_push(found_block_list, bg_record);
			format_node_name(bg_record, tmp_char,
					 sizeof(tmp_char));
			info("Existing: BlockID:%s Nodes:%s Conn:%s",
			     bg_record->bg_block_id,
			     tmp_char,
			     conn_type_string(bg_record->conn_type));
			if (((bg_record->state == RM_PARTITION_READY)
			     || (bg_record->state == RM_PARTITION_CONFIGURING))
			    && !block_ptr_exist_in_list(bg_lists->booted,
							bg_record))
				list_push(bg_lists->booted, bg_record);
		}
	}
	if (bg_conf->layout_mode == LAYOUT_DYNAMIC)
		goto finished;

	if (!full_created) {
		list_iterator_reset(itr_curr);
		while ((init_bg_record = list_next(itr_curr))) {
			if (init_bg_record->full_block) {
				list_remove(itr_curr);
				bg_record = init_bg_record;
				list_append(bg_lists->main, bg_record);
				list_push(found_block_list, bg_record);
				format_node_name(bg_record, tmp_char,
						 sizeof(tmp_char));
				info("Existing: BlockID:%s Nodes:%s Conn:%s",
				     bg_record->bg_block_id,
				     tmp_char,
				     conn_type_string(bg_record->conn_type));
				if (((bg_record->state == RM_PARTITION_READY)
				     || (bg_record->state
					 == RM_PARTITION_CONFIGURING))
				    && !block_ptr_exist_in_list(
					    bg_lists->booted, bg_record))
					list_push(bg_lists->booted,
						  bg_record);
				break;
			}
		}
	}

finished:
	list_iterator_destroy(itr_conf);
	list_iterator_destroy(itr_curr);
	if (!list_count(curr_block_list))
		rc = SLURM_SUCCESS;
	return rc;
}

static int _delete_old_blocks(List curr_block_list, List found_block_list)
{
	ListIterator itr_curr, itr_found;
	bg_record_t *found_record = NULL, *init_record = NULL;
	List destroy_list = list_create(NULL);

	xassert(curr_block_list);
	xassert(found_block_list);

	info("removing unspecified blocks");
	if (!bg_recover) {
		itr_curr = list_iterator_create(curr_block_list);
		while ((init_record = list_next(itr_curr))) {
			list_remove(itr_curr);
			list_push(destroy_list, init_record);
		}
		list_iterator_destroy(itr_curr);
	} else {
		itr_curr = list_iterator_create(curr_block_list);
		while ((init_record = list_next(itr_curr))) {
			itr_found = list_iterator_create(found_block_list);
			while ((found_record = list_next(itr_found))) {
				if (!strcmp(init_record->bg_block_id,
					    found_record->bg_block_id)) {
					/* don't delete this one */
					break;
				}
			}
			list_iterator_destroy(itr_found);

			if (found_record == NULL) {
				list_remove(itr_curr);
				list_push(destroy_list, init_record);
			}
		}
		list_iterator_destroy(itr_curr);
	}

	free_block_list(NO_VAL, destroy_list, 1, 1);
	list_destroy(destroy_list);

	info("I am done deleting");

	return SLURM_SUCCESS;
}

/* block_state_mutex should be locked before calling this */
static int _post_block_free(bg_record_t *bg_record, bool restore)
{
#ifdef HAVE_BG_FILES
	int rc = SLURM_SUCCESS;
#endif
	if (bg_record->magic != BLOCK_MAGIC) {
		error("block already destroyed");
		return SLURM_ERROR;
	}

	bg_record->free_cnt--;

	if (bg_record->free_cnt > 0) {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("%d other are trying to destroy this block %s",
			     bg_record->free_cnt, bg_record->bg_block_id);
		return SLURM_SUCCESS;
	}

	if ((bg_record->state != RM_PARTITION_FREE)
	    && (bg_record->state != RM_PARTITION_ERROR)){
		/* Something isn't right, go mark this one in an error
		   state. */
		update_block_msg_t block_msg;
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("_post_block_free: block %s is not in state "
			     "free (%s), putting it in error state.",
			     bg_record->bg_block_id,
			     bg_block_state_string(bg_record->state));
		slurm_init_update_block_msg(&block_msg);
		block_msg.bg_block_id = bg_record->bg_block_id;
		block_msg.state = RM_PARTITION_ERROR;
		block_msg.reason = "Block would not deallocate";
		slurm_mutex_unlock(&block_state_mutex);
		select_p_update_block(&block_msg);
		slurm_mutex_lock(&block_state_mutex);
		return SLURM_SUCCESS;
	}

	/* A bit of a sanity check to make sure blocks are being
	   removed out of all the lists.
	*/
	if (blocks_are_created) {
		remove_from_bg_list(bg_lists->booted, bg_record);
		if (remove_from_bg_list(bg_lists->job_running, bg_record)
		    == SLURM_SUCCESS)
			num_unused_cpus += bg_record->cpu_cnt;
	}

	if (restore)
		return SLURM_SUCCESS;

	if (blocks_are_created
	    && remove_from_bg_list(bg_lists->main, bg_record)
	    != SLURM_SUCCESS) {
		/* This should only happen if called from
		 * bg_job_place.c where the block was never added to
		 * the list. */
		debug("_post_block_free: It appears this block %s isn't "
		      "in the main list anymore.",
		      bg_record->bg_block_id);
	}

#ifdef HAVE_BG_FILES
	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		info("_post_block_free: removing %s from database",
		     bg_record->bg_block_id);

	rc = bridge_remove_block(bg_record->bg_block_id);
	if (rc != STATUS_OK) {
		if (rc == PARTITION_NOT_FOUND) {
			debug("_post_block_free: block %s is not found",
			      bg_record->bg_block_id);
		} else {
			error("_post_block_free: "
			      "rm_remove_partition(%s): %s",
			      bg_record->bg_block_id,
			      bg_err_str(rc));
		}
	} else
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("_post_block_free: done %s",
			     bg_record->bg_block_id);
#endif
	destroy_bg_record(bg_record);
	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		info("_post_block_free: destroyed");

	return SLURM_SUCCESS;
}


static void *_track_freeing_blocks(void *args)
{
	bg_free_block_list_t *bg_free_list = (bg_free_block_list_t *)args;
	List track_list = bg_free_list->track_list;
	bool destroy = bg_free_list->destroy;
	uint32_t job_id = bg_free_list->job_id;
	int retry_cnt = 0;
	int free_cnt = 0, track_cnt = list_count(track_list);
	ListIterator itr = list_iterator_create(track_list);
	bg_record_t *bg_record;
	bool restore = true;

	debug("_track_freeing_blocks: Going to free %d for job %u",
	      track_cnt, job_id);
	while (retry_cnt < MAX_FREE_RETRIES) {
		free_cnt = 0;
		slurm_mutex_lock(&block_state_mutex);
		/* just to make sure state is updated */
		update_block_list_state(track_list);
		list_iterator_reset(itr);
		/* just incase this changes from the update function */
		track_cnt = list_count(track_list);
		while ((bg_record = list_next(itr))) {
			if (bg_record->magic != BLOCK_MAGIC) {
				/* update_block_list_state should
				   remove this already from the list
				   so we shouldn't ever have this.
				*/
				error("_track_freeing_blocks: block was "
				      "already destroyed");
				free_cnt++;
				continue;
			}
#ifndef HAVE_BG_FILES
			/* Fake a free since we are n deallocating
			   state before this.
			*/
			if ((bg_record->state != RM_PARTITION_ERROR)
			    && (retry_cnt >= 3))
				bg_record->state = RM_PARTITION_FREE;
#endif
			if ((bg_record->state == RM_PARTITION_FREE)
			    || (bg_record->state == RM_PARTITION_ERROR))
				free_cnt++;
			else if (bg_record->state != RM_PARTITION_DEALLOCATING)
				bg_free_block(bg_record, 0, 1);
		}
		slurm_mutex_unlock(&block_state_mutex);
		if (free_cnt == track_cnt)
			break;
		debug("_track_freeing_blocks: freed %d of %d for job %u",
		      free_cnt, track_cnt, job_id);
		sleep(FREE_SLEEP_INTERVAL);
		retry_cnt++;
	}
	debug("_track_freeing_blocks: Freed them all for job %u", job_id);

	if ((bg_conf->layout_mode == LAYOUT_DYNAMIC) || destroy)
		restore = false;

	/* If there is a block in error state we need to keep all
	 * these blocks around. */
	slurm_mutex_lock(&block_state_mutex);
	list_iterator_reset(itr);
	while ((bg_record = list_next(itr))) {
		/* block no longer exists */
		if (bg_record->magic != BLOCK_MAGIC)
			continue;
		if (bg_record->state != RM_PARTITION_FREE) {
			restore = true;
			break;
		}
	}

	list_iterator_reset(itr);
	while ((bg_record = list_next(itr)))
		_post_block_free(bg_record, restore);
	slurm_mutex_unlock(&block_state_mutex);
	last_bg_update = time(NULL);
	list_iterator_destroy(itr);
	list_destroy(track_list);
	xfree(bg_free_list);
	return NULL;
}

static char *_get_bg_conf(void)
{
	char *val = getenv("SLURM_CONF");
	char *rc = NULL;
	int i;

	if (!val)
		return xstrdup(BLUEGENE_CONFIG_FILE);

	/* Replace file name on end of path */
	i = strlen(val) - strlen("slurm.conf") + strlen("bluegene.conf") + 1;
	rc = xmalloc(i);
	strcpy(rc, val);
	val = strrchr(rc, (int)'/');
	if (val)	/* absolute path */
		val++;
	else		/* not absolute path */
		val = rc;
	strcpy(val, "bluegene.conf");
	return rc;
}

static int _reopen_bridge_log(void)
{
	int rc = SLURM_SUCCESS;

	if (bg_conf->bridge_api_file == NULL)
		return rc;

#ifdef HAVE_BG_FILES
	rc = bridge_set_log_params(bg_conf->bridge_api_file,
				   bg_conf->bridge_api_verb);
#endif
	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		info("Bridge api file set to %s, verbose level %d",
		     bg_conf->bridge_api_file, bg_conf->bridge_api_verb);

	return rc;
}

static void _destroy_bitmap(void *object)
{
	bitstr_t *bitstr = (bitstr_t *)object;

	if (bitstr) {
		FREE_NULL_BITMAP(bitstr);
	}
}
