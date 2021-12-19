/*
 * This file is part of the Simutrans project under the Artistic License.
 * (see LICENSE.txt)
 */

#include "checklist.h"


#include "../network/memory_rw.h"


#include <cstdio>


checklist_t::checklist_t() :
#if defined(HEAVY_MODE) && HEAVY_MODE >= 1
	hash(0)
#else
	random_seed(0),
	halt_entry(0),
	line_entry(0),
	convoy_entry(0)
#endif
{
}


#if defined(HEAVY_MODE) && HEAVY_MODE >= 1
checklist_t::checklist_t(const uint32 &hash) :
	hash(hash)
#else
checklist_t::checklist_t(uint32 _random_seed, uint16 _halt_entry, uint16 _line_entry, uint16 _convoy_entry) :
	random_seed(_random_seed),
	halt_entry(_halt_entry),
	line_entry(_line_entry),
	convoy_entry(_convoy_entry)
#endif
{
}


bool checklist_t::operator==(const checklist_t& other) const
{
#if defined(HEAVY_MODE) && HEAVY_MODE >= 1
	return hash == other.hash;
#else
	return
		random_seed==other.random_seed &&
		halt_entry==other.halt_entry &&
		line_entry==other.line_entry &&
		convoy_entry==other.convoy_entry;
#endif
}


bool checklist_t::operator!=(const checklist_t& other) const
{
	return !(*this==other);
}


void checklist_t::rdwr(memory_rw_t *buffer)
{
#if defined(HEAVY_MODE) && HEAVY_MODE >= 1
	buffer->rdwr_long(hash);
#else
	buffer->rdwr_long(random_seed);
	buffer->rdwr_short(halt_entry);
	buffer->rdwr_short(line_entry);
	buffer->rdwr_short(convoy_entry);
#endif
}


int checklist_t::print(char *buffer, const char *entity) const
{
#if defined(HEAVY_MODE) && HEAVY_MODE >= 1
	return sprintf(buffer, "%s=[adler32=%08x] ", entity, hash);
#else
	return sprintf(buffer, "%s=[rand=%u halt=%u line=%u cnvy=%u] ",
		entity, random_seed, halt_entry, line_entry, convoy_entry);
#endif
}

