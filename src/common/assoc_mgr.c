/*****************************************************************************\
 *  accounting_storage_slurmdbd.c - accounting interface to slurmdbd.
 *****************************************************************************
 *  Copyright (C) 2004-2007 The Regents of the University of California.
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
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

#include "assoc_mgr.h"

#include <sys/types.h>
#include <pwd.h>

#include "src/common/uid.h"
#include "src/common/xstring.h"
#include "src/slurmdbd/read_config.h"

static List local_association_list = NULL;
static List local_qos_list = NULL;
static List local_user_list = NULL;
static char *local_cluster_name = NULL;

void (*remove_assoc_notify) (acct_association_rec_t *rec) = NULL;

static pthread_mutex_t local_association_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t local_qos_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t local_user_lock = PTHREAD_MUTEX_INITIALIZER;

/* locks should be put in place before calling this function */
static int _set_assoc_parent_and_user(acct_association_rec_t *assoc)
{
	if(!assoc) {
		error("you didn't give me an association");
		return SLURM_ERROR;
	}

	if(assoc->parent_id) {
		acct_association_rec_t *assoc2 = NULL;
		ListIterator itr = list_iterator_create(local_association_list);
		while((assoc2 = list_next(itr))) {
			if(assoc2->id == assoc->parent_id) {
				assoc->parent_acct_ptr = assoc2;
				break;
			}
		}
		list_iterator_destroy(itr);
	}
	if(assoc->user) {
		uid_t pw_uid = uid_from_string(assoc->user);
		if(pw_uid == (uid_t) -1) 
			assoc->uid = (uint32_t)NO_VAL;
		else
			assoc->uid = pw_uid;	
	} else {
		assoc->uid = (uint32_t)NO_VAL;	
	}
	//log_assoc_rec(assoc);

	return SLURM_SUCCESS;
}

static int _get_local_association_list(void *db_conn, int enforce)
{
	acct_association_cond_t assoc_q;
	char *cluster_name = NULL;
//	DEF_TIMERS;
	slurm_mutex_lock(&local_association_lock);
	if(local_association_list)
		list_destroy(local_association_list);

	memset(&assoc_q, 0, sizeof(acct_association_cond_t));
	if(local_cluster_name) {
		assoc_q.cluster_list = list_create(slurm_destroy_char);
		cluster_name = xstrdup(local_cluster_name);
		if(!cluster_name) {
			if(enforce && !slurmdbd_conf) {
				error("_get_local_association_list: "
				      "no cluster name here going to get "
				      "all associations.");
			}
		} else 
			list_append(assoc_q.cluster_list, cluster_name);
	}

//	START_TIMER;
	local_association_list =
		acct_storage_g_get_associations(db_conn, &assoc_q);
//	END_TIMER2("get_associations");

	if(assoc_q.cluster_list)
		list_destroy(assoc_q.cluster_list);
	
	if(!local_association_list) {
		/* create list so we don't keep calling this if there
		   isn't anything there */
		local_association_list = list_create(NULL);
		slurm_mutex_unlock(&local_association_lock);
		if(enforce) {
			error("_get_local_association_list: "
			      "no list was made.");
			return SLURM_ERROR;
		} else {
			debug3("not enforcing associations and no "
			       "list was given so we are giving a blank list");
			return SLURM_SUCCESS;
		}
	} else {
		acct_association_rec_t *assoc = NULL;
		ListIterator itr = list_iterator_create(local_association_list);
		//START_TIMER;
		while((assoc = list_next(itr))) 
			_set_assoc_parent_and_user(assoc);
		list_iterator_destroy(itr);
		//END_TIMER2("load_associations");
	}
	slurm_mutex_unlock(&local_association_lock);

	return SLURM_SUCCESS;
}

static int _get_local_qos_list(void *db_conn, int enforce)
{
	slurm_mutex_lock(&local_qos_lock);
	if(local_qos_list)
		list_destroy(local_qos_list);
	local_qos_list = acct_storage_g_get_qos(db_conn, NULL);

	if(!local_qos_list) {
		slurm_mutex_unlock(&local_qos_lock);
		if(enforce) {
			error("_get_local_qos_list: "
			      "no list was made.");
			return SLURM_ERROR;
		} else {
			return SLURM_SUCCESS;
		}		
	}

	slurm_mutex_unlock(&local_qos_lock);
	return SLURM_SUCCESS;
}

static int _get_local_user_list(void *db_conn, int enforce)
{
	acct_user_cond_t user_q;

	memset(&user_q, 0, sizeof(acct_user_cond_t));
	user_q.with_coords = 1;

	slurm_mutex_lock(&local_user_lock);
	if(local_user_list)
		list_destroy(local_user_list);
	local_user_list = acct_storage_g_get_users(db_conn, &user_q);

	if(!local_user_list) {
		slurm_mutex_unlock(&local_user_lock);
		if(enforce) {
			error("_get_local_user_list: "
			      "no list was made.");
			return SLURM_ERROR;
		} else {
			return SLURM_SUCCESS;
		}		
	} else {
		acct_user_rec_t *user = NULL;
		ListIterator itr = list_iterator_create(local_user_list);
		//START_TIMER;
		while((user = list_next(itr))) {
			uid_t pw_uid = uid_from_string(user->name);
			if(pw_uid == (uid_t) -1) {
				error("couldn't get a uid for user %s",
				      user->name);
				user->uid = (uint32_t)NO_VAL;
			} else
				user->uid = pw_uid;
		}
		list_iterator_destroy(itr);
		//END_TIMER2("load_users");
	}

	slurm_mutex_unlock(&local_user_lock);
	return SLURM_SUCCESS;
}

extern int assoc_mgr_init(void *db_conn, assoc_init_args_t *args)
{
	int enforce = 0;

	if(args) {
		enforce = args->enforce;
		if(args->remove_assoc_notify)
			remove_assoc_notify = args->remove_assoc_notify;
	}

	if(!local_cluster_name && !slurmdbd_conf)
		local_cluster_name = slurm_get_cluster_name();

	if(!local_association_list) 
		if(_get_local_association_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if(!local_qos_list) 
		if(_get_local_qos_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	return SLURM_SUCCESS;
}

extern int assoc_mgr_fini(void)
{
	if(local_association_list) 
		list_destroy(local_association_list);
	if(local_qos_list)
		list_destroy(local_qos_list);
	if(local_user_list)
		list_destroy(local_user_list);
	xfree(local_cluster_name);
	local_association_list = NULL;
	local_qos_list = NULL;
	local_user_list = NULL;

	return SLURM_SUCCESS;
}

extern int assoc_mgr_fill_in_assoc(void *db_conn, acct_association_rec_t *assoc,
				   int enforce, 
				   acct_association_rec_t **assoc_pptr)
{
	ListIterator itr = NULL;
	acct_association_rec_t * found_assoc = NULL;
	acct_association_rec_t * ret_assoc = NULL;

	if (assoc_pptr)
		*assoc_pptr = NULL;
	if(!local_association_list) {
		if(_get_local_association_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;
	}
	if((!local_association_list || !list_count(local_association_list))
	   && !enforce) 
		return SLURM_SUCCESS;

	if(!assoc->id) {
		if(!assoc->acct) {
			acct_user_rec_t user;

			if(assoc->uid == (uint32_t)NO_VAL) {
				if(enforce) {
					error("get_assoc_id: "
					      "Not enough info to "
					      "get an association");
					return SLURM_ERROR;
				} else {
					return SLURM_SUCCESS;
				}
			}
			memset(&user, 0, sizeof(acct_user_rec_t));
			user.uid = assoc->uid;
			if(assoc_mgr_fill_in_user(db_conn, &user, enforce) 
			   == SLURM_ERROR) {
				if(enforce) 
					return SLURM_ERROR;
				else {
					return SLURM_SUCCESS;
				}
			}					
			assoc->user = user.name;
			assoc->acct = user.default_acct;
		} 
		
		if(!assoc->cluster)
			assoc->cluster = local_cluster_name;
	}
/* 	info("looking for assoc of user=%s(%u), acct=%s, " */
/* 	     "cluster=%s, partition=%s", */
/* 	     assoc->user, assoc->uid, assoc->acct,  */
/* 	     assoc->cluster, assoc->partition); */
	slurm_mutex_lock(&local_association_lock);
	itr = list_iterator_create(local_association_list);
	while((found_assoc = list_next(itr))) {
		if(assoc->id) {
			if(assoc->id == found_assoc->id) {
				ret_assoc = found_assoc;
				break;
			}
			continue;
		} else {
			if(assoc->uid == (uint32_t)NO_VAL
			   && found_assoc->uid != (uint32_t)NO_VAL) {
				debug3("we are looking for a "
				       "nonuser association");
				continue;
			} else if(assoc->uid != found_assoc->uid) {
				debug4("not the right user %u != %u",
				       assoc->uid, found_assoc->uid);
				continue;
			}
			
			if(found_assoc->acct 
			   && strcasecmp(assoc->acct, found_assoc->acct)) {
				debug4("not the right account %s != %s",
				       assoc->acct, found_assoc->acct);
				continue;
			}

			/* only check for on the slurmdbd */
			if(!local_cluster_name && found_assoc->cluster
			   && strcasecmp(assoc->cluster,
					 found_assoc->cluster)) {
				debug4("not the right cluster");
				continue;
			}
	
			if(assoc->partition
			   && (!found_assoc->partition 
			       || strcasecmp(assoc->partition, 
					     found_assoc->partition))) {
				ret_assoc = found_assoc;
				debug3("found association for no partition");
				continue;
			}
		}
		ret_assoc = found_assoc;
		break;
	}
	list_iterator_destroy(itr);
	
	if(!ret_assoc) {
		slurm_mutex_unlock(&local_association_lock);
		if(enforce) 
			return SLURM_ERROR;
		else
			return SLURM_SUCCESS;
	}
	debug3("found correct association");
	if (assoc_pptr)
		*assoc_pptr = ret_assoc;
	assoc->id = ret_assoc->id;
	if(!assoc->user)
		assoc->user = ret_assoc->user;
	if(!assoc->acct)
		assoc->acct = ret_assoc->acct;
	if(!assoc->cluster)
		assoc->cluster = ret_assoc->cluster;
	if(!assoc->partition)
		assoc->partition = ret_assoc->partition;
	assoc->fairshare                 = ret_assoc->fairshare;
	assoc->max_cpu_secs_per_job      = ret_assoc->max_cpu_secs_per_job;
	assoc->max_jobs                  = ret_assoc->max_jobs;
	assoc->max_nodes_per_job         = ret_assoc->max_nodes_per_job;
	assoc->max_wall_duration_per_job = ret_assoc->max_wall_duration_per_job;
	assoc->parent_acct_ptr           = ret_assoc->parent_acct_ptr;
	if(assoc->parent_acct) {
		xfree(assoc->parent_acct);
		assoc->parent_acct       = xstrdup(ret_assoc->parent_acct);
	} else 
		assoc->parent_acct       = ret_assoc->parent_acct;
	slurm_mutex_unlock(&local_association_lock);

	return SLURM_SUCCESS;
}

extern int assoc_mgr_fill_in_user(void *db_conn, acct_user_rec_t *user,
				  int enforce)
{
	ListIterator itr = NULL;
	acct_user_rec_t * found_user = NULL;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if((!local_user_list || !list_count(local_user_list)) && !enforce) 
		return SLURM_SUCCESS;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((found_user = list_next(itr))) {
		if(user->uid == found_user->uid) 
			break;
	}
	list_iterator_destroy(itr);

	if(found_user) {
		memcpy(user, found_user, sizeof(acct_user_rec_t));		
		slurm_mutex_unlock(&local_user_lock);
		return SLURM_SUCCESS;
	}
	slurm_mutex_unlock(&local_user_lock);
	return SLURM_ERROR;
}

extern acct_admin_level_t assoc_mgr_get_admin_level(void *db_conn,
						    uint32_t uid)
{
	ListIterator itr = NULL;
	acct_user_rec_t * found_user = NULL;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, 0) == SLURM_ERROR)
			return ACCT_ADMIN_NOTSET;

	if(!local_user_list) 
		return ACCT_ADMIN_NOTSET;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((found_user = list_next(itr))) {
		if(uid == found_user->uid) 
			break;
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_user_lock);
		
	if(found_user) 
		return found_user->admin_level;
		
	return ACCT_ADMIN_NOTSET;	
}

extern int assoc_mgr_is_user_acct_coord(void *db_conn,
					uint32_t uid,
					char *acct_name)
{
	ListIterator itr = NULL;
	acct_coord_rec_t *acct = NULL;
	acct_user_rec_t * found_user = NULL;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, 0) == SLURM_ERROR)
			return ACCT_ADMIN_NOTSET;

	if(!local_user_list) 
		return ACCT_ADMIN_NOTSET;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((found_user = list_next(itr))) {
		if(uid == found_user->uid) 
			break;
	}
	list_iterator_destroy(itr);
		
	if(!found_user || !found_user->coord_accts) {
		slurm_mutex_unlock(&local_user_lock);
		return 0;
	}
	itr = list_iterator_create(found_user->coord_accts);
	while((acct = list_next(itr))) {
		if(!strcmp(acct_name, acct->name))
			break;
	}
	list_iterator_destroy(itr);
	
	if(acct) {
		slurm_mutex_unlock(&local_user_lock);
		return 1;
	}
	slurm_mutex_unlock(&local_user_lock);

	return 0;	
}

extern int assoc_mgr_update_local_assocs(acct_update_object_t *update)
{
	acct_association_rec_t * rec = NULL;
	acct_association_rec_t * object = NULL;
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;
	int parents_changed = 0;

	if(!local_association_list)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&local_association_lock);
	itr = list_iterator_create(local_association_list);
	while((object = list_pop(update->objects))) {
		if(object->cluster && local_cluster_name) {
			/* only update the local clusters assocs */
			if(strcasecmp(object->cluster, local_cluster_name))
				continue;
		}
		list_iterator_reset(itr);
		while((rec = list_next(itr))) {
			if(object->id) {
				if(object->id == rec->id) {
					break;
				}
				continue;
			} else {
				if(!object->user && rec->user) {
					debug4("we are looking for a "
					       "nonuser association");
					continue;
				} else if(object->uid != rec->uid) {
					debug4("not the right user");
					continue;
				}
				
				if(object->acct
				   && (!rec->acct 
				       || strcasecmp(object->acct,
						     rec->acct))) {
					debug4("not the right account");
					continue;
				}
				
				/* only check for on the slurmdbd */
				if(!local_cluster_name && object->acct
				   && (!rec->cluster
				       || strcasecmp(object->cluster,
						     rec->cluster))) {
					debug4("not the right cluster");
					continue;
				}
				
				if(object->partition
				   && (!rec->partition 
				       || strcasecmp(object->partition, 
						     rec->partition))) {
					debug4("not the right partition");
					continue;
				}
				break;
			}			
		}
		//info("%d assoc %u", update->type, object->id);
		switch(update->type) {
		case ACCT_MODIFY_ASSOC:
			if(!rec) {
				rc = SLURM_ERROR;
				break;
			}
			debug("updating assoc %u", rec->id);
			if(object->fairshare != NO_VAL) {
				rec->fairshare = object->fairshare;
			}

			if(object->max_jobs != NO_VAL) {
				rec->max_jobs = object->max_jobs;
			}

			if(object->max_nodes_per_job != NO_VAL) {
				rec->max_nodes_per_job =
					object->max_nodes_per_job;
			}

			if(object->max_wall_duration_per_job != NO_VAL) {
				rec->max_wall_duration_per_job =
					object->max_wall_duration_per_job;
			}

			if(object->max_cpu_secs_per_job != NO_VAL) {
				rec->max_cpu_secs_per_job = 
					object->max_cpu_secs_per_job;
			}

			if(object->parent_acct) {
				xfree(rec->parent_acct);
				rec->parent_acct = xstrdup(object->parent_acct);
			}
			if(object->parent_id) {
				rec->parent_id = object->parent_id;
				// after all new parents have been set we will
				// reset the parent pointers below
				parents_changed = 1;
				
			}
			log_assoc_rec(rec);
			break;
		case ACCT_ADD_ASSOC:
			if(rec) {
				//rc = SLURM_ERROR;
				break;
			}
			_set_assoc_parent_and_user(object);
			list_append(local_association_list, object);
			break;
		case ACCT_REMOVE_ASSOC:
			if(!rec) {
				//rc = SLURM_ERROR;
				break;
			}
			if (remove_assoc_notify)
				remove_assoc_notify(rec);
			list_delete_item(itr);
			break;
		default:
			break;
		}
		if(update->type != ACCT_ADD_ASSOC) {
			destroy_acct_association_rec(object);			
		}				
	}

	if(parents_changed) {
		ListIterator itr2 = 
			list_iterator_create(local_association_list);
		list_iterator_reset(itr);

		while((object = list_next(itr))) {
			if(object->parent_id) {
				while((rec = list_next(itr2))) {
					if(rec->id == object->parent_id) {
						object->parent_acct_ptr = rec;
						break;
					}
				}
				list_iterator_reset(itr2);
			}
		}
		list_iterator_destroy(itr2);
	}

	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_association_lock);

	return rc;	
}

extern int assoc_mgr_update_local_users(acct_update_object_t *update)
{
	acct_user_rec_t * rec = NULL;
	acct_user_rec_t * object = NULL;
		
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;
	uid_t pw_uid;

	if(!local_user_list)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((object = list_pop(update->objects))) {
		list_iterator_reset(itr);
		while((rec = list_next(itr))) {
			if(!strcasecmp(object->name, rec->name)) {
				break;
			}
		}
		//info("%d user %s", update->type, object->name);
		switch(update->type) {
		case ACCT_MODIFY_USER:
			if(!rec) {
				rc = SLURM_ERROR;
				break;
			}

			if(object->default_acct) {
				xfree(rec->default_acct);
				rec->default_acct = object->default_acct;
				object->default_acct = NULL;
			}

			if(object->qos_list) {
				if(rec->qos_list)
					list_destroy(rec->qos_list);
				rec->qos_list = object->qos_list;
				object->qos_list = NULL;
			}

			if(object->admin_level != ACCT_ADMIN_NOTSET) 
				rec->admin_level = object->admin_level;

			break;
		case ACCT_ADD_USER:
			if(rec) {
				//rc = SLURM_ERROR;
				break;
			}
			pw_uid = uid_from_string(object->name);
			if(pw_uid == (uid_t) -1) {
				error("couldn't get a uid for user %s",
				      object->name);
				object->uid = NO_VAL;
			} else
				object->uid = pw_uid;
			list_append(local_user_list, object);
			break;
		case ACCT_REMOVE_USER:
			if(!rec) {
				//rc = SLURM_ERROR;
				break;
			}
			list_delete_item(itr);
			break;
		case ACCT_ADD_COORD:
			/* same as ACCT_REMOVE_COORD */
		case ACCT_REMOVE_COORD:
			if(!rec) {
				//rc = SLURM_ERROR;
				break;
			}
			/* We always get a complete list here */
			if(!object->coord_accts) {
				if(rec->coord_accts)
					list_flush(rec->coord_accts);
			} else {
				if(rec->coord_accts)
					list_destroy(rec->coord_accts);
				rec->coord_accts = object->coord_accts;
				object->coord_accts = NULL;
			}
			break;
		default:
			break;
		}
		if(update->type != ACCT_ADD_USER) {
			destroy_acct_user_rec(object);			
		}
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_user_lock);

	return rc;	
}

extern int assoc_mgr_update_local_qos(acct_update_object_t *update)
{
	acct_qos_rec_t * rec = NULL;
	acct_qos_rec_t * object = NULL;
		
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;

	if(!local_qos_list)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&local_qos_lock);
	itr = list_iterator_create(local_qos_list);
	while((object = list_pop(update->objects))) {
		list_iterator_reset(itr);
		while((rec = list_next(itr))) {
			if(object->id == rec->id) {
				break;
			}
		}
		//info("%d qos %s", update->type, object->name);
		switch(update->type) {
		case ACCT_ADD_QOS:
			if(rec) {
				//rc = SLURM_ERROR;
				break;
			}
			list_append(local_qos_list, object);
			break;
		case ACCT_REMOVE_QOS:
			if(!rec) {
				//rc = SLURM_ERROR;
				break;
			}
			list_delete_item(itr);
			break;
		default:
			break;
		}
		if(update->type != ACCT_ADD_QOS) {
			destroy_acct_qos_rec(object);			
		}
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_qos_lock);

	return rc;	
}

extern int assoc_mgr_validate_assoc_id(void *db_conn,
				       uint32_t assoc_id,
				       int enforce)
{
	ListIterator itr = NULL;
	acct_association_rec_t * found_assoc = NULL;

	if(!local_association_list) 
		if(_get_local_association_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if((!local_association_list || !list_count(local_association_list))
	   && !enforce) 
		return SLURM_SUCCESS;
	
	slurm_mutex_lock(&local_association_lock);
	itr = list_iterator_create(local_association_list);
	while((found_assoc = list_next(itr))) {
		if(assoc_id == found_assoc->id) 
			break;
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_association_lock);

	if(found_assoc || !enforce)
		return SLURM_SUCCESS;

	return SLURM_ERROR;
}

extern void assoc_mgr_clear_used_info(void)
{
	ListIterator itr = NULL;
	acct_association_rec_t * found_assoc = NULL;

	if (!local_association_list)
		return;

	slurm_mutex_lock(&local_association_lock);
	itr = list_iterator_create(local_association_list);
	while((found_assoc = list_next(itr))) {
		found_assoc->used_jobs  = 0;
		found_assoc->used_share = 0;
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_association_lock);
}

