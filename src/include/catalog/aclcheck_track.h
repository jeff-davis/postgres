/*-------------------------------------------------------------------------
 *
 * aclcheck_track.h
 *	  Track permission checks for revalidation after lock acquisition
 *	  in dependencyLockAndCheckObject().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/aclcheck_track.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ACLCHECK_TRACK_H
#define ACLCHECK_TRACK_H

#include "storage/sinval.h"
#include "utils/acl.h"

typedef struct AclCheckEntry
{
	Oid			classId;
	Oid			objectId;
	Oid			roleId;
	AclMode		mode;
	uint64		inval_count;
} AclCheckEntry;

extern AclCheckEntry *aclcheck_tracked;
extern int	aclcheck_tracked_count;
extern int	aclcheck_tracked_max;
extern bool aclcheck_tracking_active;

extern void aclcheck_track_reset(void);
extern void aclcheck_track_grow(void);
extern bool aclcheck_track_find(Oid classId, Oid objectId,
								Oid *roleId, AclMode *mode,
								uint64 *inval_count);

/*
 * Record an aclcheck for later revalidation.
 *
 * Called from object_aclcheck_ext() and pg_class_aclcheck_ext().
 * Only records when inside an utility statement.
 */
static inline void
aclcheck_track_record(Oid classId, Oid objectId, Oid roleId, AclMode mode)
{
	if (!aclcheck_tracking_active)
		return;

	if (aclcheck_tracked_count >= aclcheck_tracked_max)
		aclcheck_track_grow();

	aclcheck_tracked[aclcheck_tracked_count].classId = classId;
	aclcheck_tracked[aclcheck_tracked_count].objectId = objectId;
	aclcheck_tracked[aclcheck_tracked_count].roleId = roleId;
	aclcheck_tracked[aclcheck_tracked_count].mode = mode;
	aclcheck_tracked[aclcheck_tracked_count].inval_count = SharedInvalidMessageCounter;
	aclcheck_tracked_count++;
}

#endif							/* ACLCHECK_TRACK_H */
