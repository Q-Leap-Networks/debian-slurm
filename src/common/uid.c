/*****************************************************************************\
 * src/common/uid.c - uid/gid lookup utility functions
 * $Id: uid.c 13672 2008-03-19 23:10:58Z jette $
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
 *  LLNL-CODE-402394.
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

#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>

#include "uid.h"

uid_t
uid_from_string (char *name)
{
	struct passwd *pwd = NULL;
	char *p = NULL;
	uid_t uid = (uid_t) strtoul (name, &p, 10);

	if (*p != '\0')
		pwd = getpwnam (name);
	else
		pwd = getpwuid (uid);

	return pwd ? pwd->pw_uid : (uid_t) -1; 
}

char *
uid_to_string (uid_t uid)
{
	struct passwd *pwd = NULL;

	/* Suse Linux does not handle multiple users with UID=0 well */
	if (uid == 0)
		return "root";

	pwd = getpwuid(uid);
	return pwd ? pwd->pw_name : "nobody";
}

gid_t
gid_from_string (char *name)
{
	struct group *g = NULL;
	char *p = NULL;
	gid_t gid = (gid_t) strtoul (name, &p, 10);

	if (*p != '\0')
		g = getgrnam (name);
	else
		g = getgrgid (gid);

	return g ? g->gr_gid : (gid_t) -1;
}
