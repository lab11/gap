#ifndef BOARDS_H
#define BOARDS_H

#if defined(BOARD_GAP)
	#include "gap.h"
#else
	#error "Board is not defined"
#endif

#endif
