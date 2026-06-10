/* See LICENSE file for copyright and license details. */
#include <unistd.h>

int main(void)
{
	write(1, "\033[H\033[2J\033[3J", 11);
	return 0;
}
