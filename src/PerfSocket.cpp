/*---------------------------------------------------------------
 * Copyright (c) 1999,2000,2001,2002,2003
 * The Board of Trustees of the University of Illinois
 * All Rights Reserved.
 *---------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software (Iperf) and associated
 * documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 *
 * Redistributions of source code must retain the above
 * copyright notice, this list of conditions and
 * the following disclaimers.
 *
 *
 * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimers in the documentation and/or other materials
 * provided with the distribution.
 *
 *
 * Neither the names of the University of Illinois, NCSA,
 * nor the names of its contributors may be used to endorse
 * or promote products derived from this Software without
 * specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ________________________________________________________________
 * National Laboratory for Applied Network Research
 * National Center for Supercomputing Applications
 * University of Illinois at Urbana-Champaign
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________
 *
 * PerfSocket.cpp
 * by Mark Gates <mgates@nlanr.net>
 *    Ajay Tirumala <tirumala@ncsa.uiuc.edu>
 * -------------------------------------------------------------------
 * Has routines the Client and Server classes use in common for
 * performance testing the network.
 * Changes in version 1.2.0
 *     for extracting data from files
 * -------------------------------------------------------------------
 * headers
 * uses
 *   <stdlib.h>
 *   <stdio.h>
 *   <string.h>
 *
 *   <sys/types.h>
 *   <sys/socket.h>
 *   <unistd.h>
 *
 *   <arpa/inet.h>
 *   <netdb.h>
 *   <netinet/in.h>
 *   <sys/socket.h>
 * ------------------------------------------------------------------- */


#define HEADERS()

#include "headers.h"

#include "PerfSocket.hpp"
#include "SocketAddr.h"
#include "util.h"
#if HAVE_DECL_SO_BINDTODEVICE
#include <net/if.h>
#endif
/* -------------------------------------------------------------------
 * Set socket options before the listen() or connect() calls.
 * These are optional performance tuning factors.
 * ------------------------------------------------------------------- */

void SetSocketOptions( thread_Settings *inSettings ) {
    // set the TCP window size (socket buffer sizes)
    // also the UDP buffer size
    // must occur before call to accept() for large window sizes
    setsock_tcp_windowsize( inSettings->mSock, inSettings->mTCPWin,
                            (inSettings->mThreadMode == kMode_Client ? 1 : 0) );

    if ( isCongestionControl( inSettings ) ) {
#ifdef TCP_CONGESTION
	Socklen_t len = strlen( inSettings->mCongestion ) + 1;
	int rc = setsockopt( inSettings->mSock, IPPROTO_TCP, TCP_CONGESTION,
			     inSettings->mCongestion, len);
	if (rc == SOCKET_ERROR ) {
		fprintf(stderr, "Attempt to set '%s' congestion control failed: %s\n",
			inSettings->mCongestion, strerror(errno));
		exit(1);
	}
#else
	fprintf( stderr, "The -Z option is not available on this operating system\n");
#endif
    }

#if HAVE_DECL_SO_BINDTODEVICE
    if ((inSettings->mThreadMode == kMode_Client) && inSettings->mIfrnametx) {
    	//指定了要发送的设备，将socket绑定到设备
        struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), inSettings->mIfrnametx);
	if (setsockopt(inSettings->mSock, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr)) < 0) {
	    char *buf;
	    int len = snprintf(NULL, 0, "%s %s", "bind to device", inSettings->mIfrnametx);
	    len++;  // Trailing null byte + extra
	    buf = (char *) malloc(len);
	    len = snprintf(buf, len, "%s %s", "bind to device", inSettings->mIfrnametx);
	    WARN_errno(1, buf );
	    free(buf);
	    free(inSettings->mIfrnametx);
	    inSettings->mIfrnametx = NULL;
	}
    }
#endif

    // check if we're sending multicast
    if (isMulticast(inSettings)) {
    	//udp的组播测试
#ifdef HAVE_MULTICAST
	if (!isUDP(inSettings)) {
	    FAIL(1, "Multicast requires -u option ", inSettings);
	    exit(1);
	}
	// check for default TTL, multicast is 1 and unicast is the system default
	if (inSettings->mTTL == -1) {
	    inSettings->mTTL = 1;
	}
	if (inSettings->mTTL > 0) {
	    // set TTL
	    int val = inSettings->mTTL;
	    if ( !isIPV6(inSettings) ) {
		int rc = setsockopt( inSettings->mSock, IPPROTO_IP, IP_MULTICAST_TTL,
				     (char*) &val, (Socklen_t) sizeof(val));

		WARN_errno( rc == SOCKET_ERROR, "multicast v4 ttl" );
	    } else
#  ifdef HAVE_IPV6_MULTICAST
	    {
		int rc = setsockopt( inSettings->mSock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
				     (char*) &val, (Socklen_t) sizeof(val));
		WARN_errno( rc == SOCKET_ERROR, "multicast v6 ttl" );
	    }
#  else
	    FAIL_errno(1, "v6 multicast not supported", inSettings);
#  endif
	}
#endif
    } else if (inSettings->mTTL > 0) {
    	//设置报文的ttl
	int val = inSettings->mTTL;
	int rc = setsockopt( inSettings->mSock, IPPROTO_IP, IP_TTL,
			     (char*) &val, (Socklen_t) sizeof(val));
	WARN_errno( rc == SOCKET_ERROR, "v4 ttl" );
    }

#ifdef IP_TOS
#if HAVE_DECL_IPV6_TCLASS && ! defined HAVE_WINSOCK2_H
    // IPV6_TCLASS is defined on Windows but not implemented.
    if (isIPV6(inSettings)) {
	const int dscp = inSettings->mTOS;
	int rc = setsockopt(inSettings->mSock, IPPROTO_IPV6, IPV6_TCLASS, (char*) &dscp, sizeof(dscp));
        WARN_errno( rc == SOCKET_ERROR, "setsockopt IPV6_TCLASS" );
    } else
#endif
    // set IP TOS (type-of-service) field
    if ( inSettings->mTOS > 0 ) {
    	//设置报文发送出去时的tos
        int  tos = inSettings->mTOS;
        Socklen_t len = sizeof(tos);
        int rc = setsockopt( inSettings->mSock, IPPROTO_IP, IP_TOS,
                             (char*) &tos, len );
        WARN_errno( rc == SOCKET_ERROR, "setsockopt IP_TOS" );
    }
#endif

    if ( !isUDP( inSettings ) ) {
    	//tcp情况下，按参数设置mss大小
        // set the TCP maximum segment size
        setsock_tcp_mss( inSettings->mSock, inSettings->mMSS );

#ifdef TCP_NODELAY

        // set TCP nodelay option
        if ( isNoDelay( inSettings ) ) {
            int nodelay = 1;
            Socklen_t len = sizeof(nodelay);
            int rc = setsockopt( inSettings->mSock, IPPROTO_TCP, TCP_NODELAY,
                                 (char*) &nodelay, len );
            WARN_errno( rc == SOCKET_ERROR, "setsockopt TCP_NODELAY" );
        }
#endif
    }

#if HAVE_DECL_SO_MAX_PACING_RATE
    /* If socket pacing is specified try to enable it. */
    if (isFQPacing(inSettings) && inSettings->mFQPacingRate > 0) {
	int rc = setsockopt(inSettings->mSock, SOL_SOCKET, SO_MAX_PACING_RATE, &inSettings->mFQPacingRate, sizeof(inSettings->mFQPacingRate));
        WARN_errno( rc == SOCKET_ERROR, "setsockopt SO_MAX_PACING_RATE" );
    }
#endif /* HAVE_SO_MAX_PACING_RATE */
}

void SetSocketOptionsSendTimeout( thread_Settings *mSettings, int timer) {
    if (timer > 0) {
#ifdef WIN32
	// Windows SO_SNDTIMEO uses ms
	DWORD timeout = (double) timer / 1e3;
#else
	struct timeval timeout;
	timeout.tv_sec = timer / 1000000;
	timeout.tv_usec = timer % 1000000;
#endif
	if (setsockopt( mSettings->mSock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0 ) {
	    WARN_errno( mSettings->mSock == SO_SNDTIMEO, "socket" );
	}
    }
}


// end SetSocketOptions
