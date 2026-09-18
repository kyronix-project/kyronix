#include "mnconf-common.h"
#include "lkc.h"

int jump_key_char;

int handle_search_keys(int key, size_t start, size_t end, void *_data)
{
	(void)key; (void)start; (void)end; (void)_data;
	return 0;
}
