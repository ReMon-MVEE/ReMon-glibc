#include <sysdep.h>

	.text
	.syntax unified

ENTRY(mvee_infinite_loop)
	nop
	nop
	nop
	nop
	nop
	b mvee_infinite_loop
END(mvee_infinite_loop)
