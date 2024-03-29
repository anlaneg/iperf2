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
 * Launch.cpp
 * by Kevin Gibbs <kgibbs@nlanr.net>
 * -------------------------------------------------------------------
 * Functions to launch new server and client threads from C while
 * the server and client are in C++.
 * The launch function for reporters is in Reporter.c since it is
 * in C and does not need a special launching function.
 * ------------------------------------------------------------------- */

#include "headers.h"
#include "Thread.h"
#include "Settings.hpp"
#include "Client.hpp"
#include "Listener.hpp"
#include "Server.hpp"
#include "PerfSocket.hpp"

#if HAVE_SCHED_SETSCHEDULER
#include <sched.h>
#endif
#ifdef HAVE_MLOCKALL
#include <sys/mman.h>
#endif
static void set_scheduler(thread_Settings *thread) {
#if HAVE_SCHED_SETSCHEDULER
	//如果设置为realtime，则更改调度
    if ( isRealtime( thread ) ) {
	struct sched_param sp;
	sp.sched_priority = sched_get_priority_max(SCHED_RR);
	// SCHED_OTHER, SCHED_FIFO, SCHED_RR
	if (sched_setscheduler(0, SCHED_RR, &sp) < 0)  {
	    perror("Client set scheduler");
#ifdef HAVE_MLOCKALL
	} else if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
	    // lock the threads memory
	    perror ("mlockall");
#endif // MLOCK
	}
    }
#endif // SCHED
}

/*
 * listener_spawn is responsible for creating a Listener class
 * and launching the listener. It is provided as a means for
 * the C thread subsystem to launch the listener C++ object.
 */
void listener_spawn( thread_Settings *thread ) {
    Listener *theListener = NULL;
    // the Listener need to trigger a settings report
    setReport(thread);
    // start up a listener
    theListener = new Listener( thread );

    // Start listening
    theListener->Run();
    DELETE_PTR( theListener );
}

/*
 * server_spawn is responsible for creating a Server class
 * and launching the server. It is provided as a means for
 * the C thread subsystem to launch the server C++ object.
 */
void server_spawn( thread_Settings *thread) {
    Server *theServer = NULL;

    // Start up the server
    theServer = new Server( thread );
    // set traffic thread to realtime if needed
    set_scheduler(thread);
    // Run the test
    if ( isUDP( thread ) ) {
        theServer->RunUDP();
    } else {
        theServer->RunTCP();
    }
    DELETE_PTR( theServer);
}

/*
 * client_spawn is responsible for creating a Client class
 * and launching the client. It is provided as a means for
 * the C thread subsystem to launch the client C++ object.
 */
void client_spawn( thread_Settings *thread ) {
    Client *theClient = NULL;
    thread_Settings *reverse_client = NULL;

    // start up the client
    // Note: socket connect() happens here in the constructor
    // that should be fixed as a clean up
    theClient = new Client( thread );

    // set traffic thread to realtime if needed
    set_scheduler(thread);

    // If this is a reverse test, then run that way
    if (isReverse(thread)) {
#ifdef HAVE_THREAD_DEBUG
      thread_debug("Client reverse (server) thread starting sock=%d", thread->mSock);
#endif
      // Settings copy will malloc space for the
      // reverse thread settings and the run_wrapper
      // will free it
      Settings_Copy(thread, &reverse_client);
      if (reverse_client && (thread->mSock > 0)) {
	reverse_client->mSock = thread->mSock;
	reverse_client->mThreadMode = kMode_Server;
	setServerReverse(reverse_client); // cause the connection report to show reverse
	thread_start(reverse_client);
      } else {
	fprintf(stderr, "Reverse test failed to start per thread settings or socket problem\n");
	exit(1);
      }
    }
    // There are a few different client startup modes
    // o) Normal
    // o) Dual (-d or -r)
    // o) Reverse
    // o) ServerReverse
    // o) WriteAck
    // o) Bidir
    if (!isServerReverse(thread)) {
      theClient->InitiateServer();
    }
    // If this is a only reverse test, then join the client thread to it
    if (isReverse(thread) && !isBidir(thread)) {
	if (!thread_equalid(reverse_client->mTID, thread_zeroid())) {
	  if (pthread_join(reverse_client->mTID, NULL) != 0) {
	    WARN( 1, "pthread_join reverse failed" );
	  } else {
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Client reverse thread finished sock=%d", reverse_client->mSock);
#endif
	  }
	}
    } else {
      // Run the normal client test
      theClient->Run();
    }
    DELETE_PTR( theClient );
}

/*
 * client_init handles multiple threaded connects. It creates
 * a listener object if either the dual test or tradeoff were
 * specified. It also creates settings structures for all the
 * threads and arranges them so they can be managed and started
 * via the one settings structure that was passed in.
 */
void client_init( thread_Settings *clients ) {
    thread_Settings *itr = NULL;
    thread_Settings *next = NULL;

    itr = clients;
#ifdef HAVE_CLOCK_NANOSLEEP
#ifdef HAVE_CLOCK_GETTIME
    if (isTxStartTime(clients)) {
        struct timespec t1;
	clock_gettime(CLOCK_REALTIME, &t1);
	fprintf(stdout, "Client thread(s) traffic start time %ld.%.9ld current time is %ld.%.9ld (epoch/unix format)\n",clients->txstart.tv_sec, clients->txstart.tv_nsec, t1.tv_sec, t1.tv_nsec);
	fflush(stdout);
    }
#endif
#endif
    setReport(clients);
    // See if we need to start a listener as well
    Settings_GenerateListenerSettings( clients, &next );

    // Create a multiple report header to handle reporting the
    // sum of multiple client threads
    Mutex_Lock( &groupCond );
    groupID--;
    clients->multihdr = InitMulti( clients, groupID );
    Mutex_Unlock( &groupCond );

#ifdef HAVE_THREAD
    if ( next != NULL ) {
        // We have threads and we need to start a listener so
        // have it ran before the client is launched
        itr->runNow = next;
        itr = next;
    }
#endif
    // For each of the needed threads create a copy of the
    // provided settings, unsetting the report flag and add
    // to the list of threads to start
    for (int i = 1; i < clients->mThreads; i++) {
        Settings_Copy( clients, &next );
	if (isIncrDstIP(clients))
	    next->incrdstip = i;

        itr->runNow = next;
        itr = next;
    }

#ifndef HAVE_THREAD
    if ( next != NULL ) {
        // We don't have threads and we need to start a listener so
        // have it ran after the client is finished
        itr->runNext = next;
    }
#endif
}
