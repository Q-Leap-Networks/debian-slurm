/*****************************************************************************\
 *  basil_interface.c - slurmctld interface to BASIL, Cray's Batch Application
 *	Scheduler Interface Layer (BASIL). In order to support development,
 *	these functions will provide basic BASIL-like functionality even
 *	without a BASIL command being present.
 *****************************************************************************
 *  Copyright (C) 2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
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

/* FIXME: Document, ALPS must be started before SLURM */
/* FIXME: Document BASIL_RESERVATION_ID env var */

#if HAVE_CONFIG_H
#  include "config.h"
#endif	/* HAVE_CONFIG_H */

#include <slurm/slurm_errno.h>
#include <stdlib.h>
#include <string.h>

#include "src/common/log.h"
#include "src/common/node_select.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/basil_interface.h"
#include "src/slurmctld/slurmctld.h"

#define BASIL_DEBUG 1

#ifdef HAVE_CRAY

/* Make sure that each SLURM node has a BASIL node ID */
static void _validate_basil_node_id(void)
{
	int i;
	struct node_record *node_ptr = node_record_table_ptr;

	for (i = 0;  i < node_record_count; i++, node_ptr++) {
		if (node_ptr->basil_node_id != NO_VAL)
			continue;
		if (IS_NODE_DOWN(node_ptr))
			continue;

		error("Node %s has no basil node_id", node_ptr->name);
		last_node_update = time(NULL);
		set_node_down(node_ptr->name, "No BASIL node_id");
	}
}
#endif	/* HAVE_CRAY */

/*
 * basil_query - Query BASIL for node and reservation state.
 * Execute once at slurmctld startup and periodically thereafter.
 * RET 0 or error code
 */
extern int basil_query(void)
{
	int error_code = SLURM_SUCCESS;
#ifdef HAVE_CRAY
	struct node_record *node_ptr;
	int i;
	static bool first_run = true;

	/*
	 * Issue the BASIL INVENTORY QUERY
	 * FIXME: Still to be done,
	 *        return SLURM_ERROR on failure
	 */
	debug("basil query initiated");

	if (first_run) {
		/* Set basil_node_id to NO_VAL since the default value
		 * of zero is a valid BASIL node ID */
		node_ptr = node_record_table_ptr;
		for (i = 0; i < node_record_count; i++, node_ptr++)
			node_ptr->basil_node_id = NO_VAL;
		first_run = false;
	}

	/* Validate configuration for each node that BASIL reports: TBD */
	_validate_basil_node_id();

	/*
	 * Confirm that each BASIL reservation is still valid,
	 * iterate through each current ALPS reservation,
	 * purge vestigial reservations.
	 * FIXME: still to be done
	 */
#endif	/* HAVE_CRAY */

	return error_code;
}

/*
 * basil_reserve - create a BASIL reservation.
 * IN job_ptr - pointer to job which has just been allocated resources
 * RET 0 or error code, job will abort or be requeued on failure
 */
extern int basil_reserve(struct job_record *job_ptr)
{
	int error_code = SLURM_SUCCESS;
#ifdef HAVE_CRAY
	uint32_t reservation_id;

	/*
	 * Issue the BASIL RESERVE request
	 * FIXME: still to be done, return SLURM_ERROR on error.
	 */
	select_g_select_jobinfo_set(job_ptr->select_jobinfo,
				    SELECT_JOBDATA_RESV_ID, &reservation_id);
	debug("basil reservation made job_id=%u resv_id=%u",
	      job_ptr->job_id, reservation_id);
#endif	/* HAVE_CRAY */
	return error_code;
}

/*
 * basil_release - release a BASIL reservation by job.
 * IN job_ptr - pointer to job which has just been deallocated resources
 * RET 0 or error code
 */
extern int basil_release(struct job_record *job_ptr)
{
	int error_code = SLURM_SUCCESS;
#ifdef HAVE_CRAY
	uint32_t reservation_id = 0;
	select_g_select_jobinfo_get(job_ptr->select_jobinfo,
				    SELECT_JOBDATA_RESV_ID, &reservation_id);
	if (reservation_id)
		error_code = basil_release_id(reservation_id);

#endif	/* HAVE_CRAY */
	return error_code;
}

/*
 * basil_release_id - release a BASIL reservation by ID.
 * IN reservation_id - ID of reservation to release
 * RET 0 or error code
 */
extern int basil_release_id(uint32_t reservation_id)
{
	int error_code = SLURM_SUCCESS;
#ifdef HAVE_CRAY
	/*
	 * Issue the BASIL RELEASE request
	 * FIXME: still to be done, return SLURM_ERROR on error.
	 */
	debug("basil release of reservation %d complete", reservation_id);
#endif	/* HAVE_CRAY */
	return error_code;
}
