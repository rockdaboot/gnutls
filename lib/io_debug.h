/*
 * This file is part of GNUTLS.
 *
 * GNUTLS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GNUTLS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
 
/* This debug file was contributed by 
 * Paul Sheer <psheer@icon.co.za>
 *
 * It does return EAGAIN errors in recv() and
 * send() calls in order to check, whether
 * non blocking support is ok.
 *
 */

#ifdef IO_DEBUG

#include <gnutls_int.h>

#define EDUNNO 11 /* EAGAIN */

extern int errno;
static int initialized_rand = 0;

#define INITIALIZE_RAND if (initialized_rand==0) {\
		srand(time(0)); \
		initialized_rand = 1; \
		}
static int recv_debug (int fd, char *buf, int len, int flags)
{
    INITIALIZE_RAND;
    
    if (!(rand() % 2)) {
	errno = EDUNNO;
	return -1;
    }
    if (len > 1)
	len = 1;
    return recv (fd, buf, len, flags);
}

#define recv recv_debug

static int send_debug (int fd, const char *buf, int len, int flags)
{
    INITIALIZE_RAND;
    
    if (!(rand() % 2)) {
	errno = EDUNNO;
	return -1;
    }
    if (len > 1)
	len = 1;
    return send (fd, buf, len, flags);
}

#define send send_debug

#endif
