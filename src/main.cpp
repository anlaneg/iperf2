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
 * main.cpp
 * by Mark Gates <mgates@nlanr.net>
 * &  Ajay Tirumala <tirumala@ncsa.uiuc.edu>
 * -------------------------------------------------------------------
 * main does initialization and creates the various objects that will
 * actually run the iperf program, then waits in the Joinall().
 * -------------------------------------------------------------------
 * headers
 * uses
 *   <stdlib.h>
 *   <string.h>
 *
 *   <signal.h>
 * ------------------------------------------------------------------- */

#define HEADERS()

#include "headers.h"

#include "Settings.hpp"
#include "PerfSocket.hpp"
#include "Locale.h"
#include "Condition.h"
#include "Timestamp.hpp"
#include "Listener.hpp"
#include "List.h"
#include "util.h"

#ifdef WIN32
#include "service.h"
#endif

/* -------------------------------------------------------------------
 * prototypes
 * ------------------------------------------------------------------- */
// Function called at exit to clean up as much as possible
void cleanup( void );

/* -------------------------------------------------------------------
 * global variables
 * ------------------------------------------------------------------- */
extern "C" {
    // Global flag to signal a user interrupt
    int sInterupted = 0;
    // Global ID that we increment to be used
    // as identifier for SUM reports
    int groupID = 0;
    // Mutex to protect access to the above ID
    Mutex groupCond;
    // Condition used to signal the reporter thread
    // when a packet ring is full.  Shouldn't really
    // be needed but is "belts and suspeners"
    Condition ReportCond;
}

// global variables only accessed within this file

// Thread that received the SIGTERM or SIGINT signal
// Used to ensure that if multiple threads receive the
// signal we do not prematurely exit
nthread_t sThread;
// The main thread uses this function to wait
// for all other threads to complete
void waitUntilQuit( void );

/* -------------------------------------------------------------------
 * main()
 *      Entry point into Iperf
 *
 * sets up signal handlers
 * initialize global locks and conditions
 * parses settings from environment and command line
 * starts up server or client thread
 * waits for all threads to complete
 * ------------------------------------------------------------------- */
int main( int argc, char **argv ) {

    // Set SIGTERM and SIGINT to call our user interrupt function
    my_signal( SIGTERM, Sig_Interupt );
    my_signal( SIGINT,  Sig_Interupt );
#ifndef WIN32
    my_signal( SIGALRM,  Sig_Interupt );

    // Ignore broken pipes
    signal(SIGPIPE,SIG_IGN);
#else
    // Start winsock
    WSADATA wsaData;
    int rc = WSAStartup( 0x202, &wsaData );
    WARN_errno( rc == SOCKET_ERROR, "WSAStartup" );
	if (rc == SOCKET_ERROR)
		return 0;

    // Tell windows we want to handle our own signals
    SetConsoleCtrlHandler( sig_dispatcher, true );
#endif

    // Initialize global mutexes and conditions
    Condition_Initialize ( &ReportCond );
    Mutex_Initialize( &groupCond );
    Mutex_Initialize( &clients_mutex );

    // Initialize the thread subsystem
    thread_init( );

    // Initialize the interrupt handling thread to 0
    sThread = thread_zeroid();

    // perform any cleanup when quitting Iperf
    atexit( cleanup );

    // Allocate the "global" settings
    thread_Settings* ext_gSettings = new thread_Settings;
    // Default reporting mode here to avoid unitialized warnings
    // this won't be the actual mode
    ThreadMode ReporterThreadMode = kMode_Reporter;
    // Initialize settings to defaults
    Settings_Initialize( ext_gSettings );
    // read settings from environment variables
    Settings_ParseEnvironment( ext_gSettings );
    // read settings from command-line parameters
    //解析命令行参数
    Settings_ParseCommandLine( argc, argv, ext_gSettings );

    // Check for either having specified client or server
    //拒绝掉非client又非server的ThreadMode
    if ((ext_gSettings->mThreadMode != kMode_Client) && (ext_gSettings->mThreadMode != kMode_Listener)) {
        // neither server nor client mode was specified
        // print usage and exit

#ifdef WIN32
        // In Win32 we also attempt to start a previously defined service
        // Starting in 2.0 to restart a previously defined service
        // you must call iperf with "iperf -D" or using the environment variable
        SERVICE_TABLE_ENTRY dispatchTable[] =
	    {
		{ (LPSTR)TEXT(SZSERVICENAME), (LPSERVICE_MAIN_FUNCTION)service_main},
		{ NULL, NULL}
	    };

	// starting the service by SCM, there is no arguments will be passed in.
	// the arguments will pass into Service_Main entry.
        if (!StartServiceCtrlDispatcher(dispatchTable) )
            // If the service failed to start then print usage
#endif
	    fprintf( stderr, usage_short, argv[0], argv[0] );
	return 0;
    }

    unsetReport(ext_gSettings);
    switch (ext_gSettings->mThreadMode) {
    case kMode_Client :
	if ( isDaemon( ext_gSettings ) ) {
	    fprintf(stderr, "Iperf client cannot be run as a daemon\n");
	    return 0;
	}
        // initialize client(s)
		//	初始化客户端
        client_init( ext_gSettings );
	ReporterThreadMode = kMode_ReporterClient;
	break;
    case kMode_Listener :
#ifdef WIN32
     // Remove the Windows service if requested
	if ( isRemoveService( ext_gSettings ) ) {
	    // remove the service
	    if ( CmdRemoveService() ) {
		fprintf(stderr, "IPerf Service is removed.\n");
	    }
	}
	if ( isDaemon( ext_gSettings ) ) {
	    CmdInstallService(argc, argv);
	} else if (isRemoveService(ext_gSettings)) {
	    return 0;
	}
#else // *nix system
	if ( isDaemon( ext_gSettings ) ) {
		//生成daemon
	    fprintf( stderr, "Running Iperf Server as a daemon\n");
	    // Start the server as a daemon
	    fflush(stderr);
	    // redirect stdin, stdout and sterr to /dev/null (see dameon and no close flag)
	    if (daemon(1, 0) < 0) {
	        perror("daemon");
	    }
	}
#endif
	break;
    default :
	fprintf( stderr, "unknown mode");
	break;
    }
#ifdef HAVE_THREAD
    // Last step is to initialize the reporter then start all threads
    {
    //创建一个reportThread
	thread_Settings *into = NULL;
	// Create the settings structure for the reporter thread
	Settings_Copy( ext_gSettings, &into );
	into->mThreadMode = ReporterThreadMode;
	// Have the reporter launch the client or listener
	into->runNow = ext_gSettings;

	// Start all the threads that are ready to go
	thread_start( into );
    }
#else
    // No need to make a reporter thread because we don't have threads
    thread_start( ext_gSettings );
#endif

    // wait for other (client, server) threads to complete
    thread_joinall();

    // all done!
    return 0;
} // end main

/* -------------------------------------------------------------------
 * Signal handler sets the sInterupted flag, so the object can
 * respond appropriately.. [static]
 * ------------------------------------------------------------------- */

void Sig_Interupt( int inSigno ) {
#ifdef HAVE_THREAD
    // We try to not allow a single interrupt handled by multiple threads
    // to completely kill the app so we save off the first thread ID
    // then that is the only thread that can supply the next interrupt
    if ( (inSigno == SIGINT) && thread_equalid( sThread, thread_zeroid() ) ) {
        sThread = thread_getid();
    } else if ( thread_equalid( sThread, thread_getid() ) ) {
        sig_exit( inSigno );
    }
    // global variable used by threads to see if they were interrupted
    sInterupted = inSigno;

    // Note:  ignore alarms per setitimer
#if HAVE_DECL_SIGALRM
    if (inSigno != SIGALRM)
#endif
	// with threads, stop waiting for non-terminating threads
	// (ie Listener Thread)
	thread_release_nonterm( inSigno );

#else
    // without threads, just exit quietly, same as sig_exit()
    sig_exit( inSigno );
#endif
}

/* -------------------------------------------------------------------
 * Any necesary cleanup before Iperf quits. Called at program exit,
 * either by exit() or terminating main().
 * ------------------------------------------------------------------- */

void cleanup( void ) {
#ifdef WIN32
    // Shutdown Winsock
    WSACleanup();
#endif
    // clean up the list of clients
    Iperf_destroy ( &clients );

    // shutdown the thread subsystem
    thread_destroy( );
} // end cleanup

#ifdef WIN32
/*--------------------------------------------------------------------
 * ServiceStart
 *
 * each time starting the service, this is the entry point of the service.
 * Start the service, certainly it is on server-mode
 *
 *-------------------------------------------------------------------- */
VOID ServiceStart (DWORD dwArgc, LPTSTR *lpszArgv) {
    thread_Settings* ext_gSettings;

    // report the status to the service control manager.
    //
    if ( !ReportStatusToSCMgr(
                             SERVICE_START_PENDING, // service state
                             NO_ERROR,              // exit code
                             3000) )                 // wait hint
        goto clean;

    ext_gSettings = new thread_Settings;

    // Initialize settings to defaults
    Settings_Initialize( ext_gSettings );
    // read settings from environment variables
    Settings_ParseEnvironment( ext_gSettings );
    // read settings from command-line parameters
    Settings_ParseCommandLine( dwArgc, lpszArgv, ext_gSettings );

    // Arguments will be lost when the service is started by SCM, but
    // we need to be at least a listener
    ext_gSettings->mThreadMode = kMode_Listener;

    // report the status to the service control manager.
    //
    if ( !ReportStatusToSCMgr(
                             SERVICE_START_PENDING, // service state
                             NO_ERROR,              // exit code
                             3000) )                 // wait hint
        goto clean;

    // if needed, redirect the output into a specified file
    if ( !isSTDOUT( ext_gSettings ) ) {
        redirect( ext_gSettings->mOutputFileName );
    }

    // report the status to the service control manager.
    //
    if ( !ReportStatusToSCMgr(
                             SERVICE_START_PENDING, // service state
                             NO_ERROR,              // exit code
                             3000) )                 // wait hint
        goto clean;

    // initialize client(s)
    if ( ext_gSettings->mThreadMode == kMode_Client ) {
        client_init( ext_gSettings );
    }

    // start up the reporter and client(s) or listener
    {
        thread_Settings *into = NULL;
#ifdef HAVE_THREAD
        Settings_Copy( ext_gSettings, &into );
        into->mThreadMode = kMode_Reporter;
        into->runNow = ext_gSettings;
#else
        into = ext_gSettings;
#endif
        thread_start( into );
    }

    // report the status to the service control manager.
    //
    if ( !ReportStatusToSCMgr(
                             SERVICE_RUNNING,       // service state
                             NO_ERROR,              // exit code
                             0) )                    // wait hint
        goto clean;

    clean:
    // wait for other (client, server) threads to complete
    thread_joinall();
}


//
//  FUNCTION: ServiceStop
//
//  PURPOSE: Stops the service
//
//  PARAMETERS:
//    none
//
//  RETURN VALUE:
//    none
//
//  COMMENTS:
//    If a ServiceStop procedure is going to
//    take longer than 3 seconds to execute,
//    it should spawn a thread to execute the
//    stop code, and return.  Otherwise, the
//    ServiceControlManager will believe that
//    the service has stopped responding.
//
VOID ServiceStop() {
#ifdef HAVE_THREAD
    Sig_Interupt( 1 );
#else
    sig_exit(1);
#endif
}

#endif
