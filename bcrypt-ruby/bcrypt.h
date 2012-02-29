
#include "bcrypttype.h"

#ifndef _BCRYPT_H_
#define _BCRYPT_H_

extern char * bcrypt_gensalt(u_int8_t log_rounds, u_int8_t *rseed);
extern char * bcrypt_gensalt_simple(u_int8_t log_rounds);
extern char * bcrypt(const char *key, const char *salt);


#endif
