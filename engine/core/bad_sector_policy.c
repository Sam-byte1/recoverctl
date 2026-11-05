#include "bad_sector_policy.h"

bad_sector_action_t bad_sector_policy_decide(
     uint32_t attempt_number,
     uint32_t max_retries )
{
	if( attempt_number <= max_retries )
	{
		return BAD_SECTOR_ACTION_RETRY;
	}
	return BAD_SECTOR_ACTION_SKIP;
}
