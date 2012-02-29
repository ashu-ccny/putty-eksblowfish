#ifndef _BCRYPTTYPE_H_
#define _BCRYPTTYPE_H_

#include <stdint.h>


/* Add this type so we'll compile nicely on Solaris.
   Thanks to Jeremy LaTrasse and the Twitter crew. */
//#ifdef __sun
#define u_int8_t uint8_t
#define u_int16_t uint16_t
#define u_int32_t uint32_t
#define u_int64_t uint64_t
//#endif


#endif
