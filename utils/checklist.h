/*
 * This file is part of the Simutrans project under the Artistic License.
 * (see LICENSE.txt)
 */

#ifndef UTILS_CHECKLIST_H
#define UTILS_CHECKLIST_H


#include "../simtypes.h"


class memory_rw_t;


struct checklist_t
{
public:
	checklist_t();
#if defined(HEAVY_MODE) && HEAVY_MODE >= 1
	explicit checklist_t(const uint32 &hash);
#else
	checklist_t(uint32 _random_seed, uint16 _halt_entry, uint16 _line_entry, uint16 _convoy_entry);
#endif

	bool operator==(const checklist_t &other) const;
	bool operator!=(const checklist_t &other) const;

	void rdwr(memory_rw_t *buffer);
	int print(char *buffer, const char *entity) const;

#if defined(HEAVY_MODE) && HEAVY_MODE >= 1
private:
	uint32 hash;
#else
public:
	uint32 random_seed;
	uint16 halt_entry;
	uint16 line_entry;
	uint16 convoy_entry;
#endif
};

#endif
