/**
 *    Copyright (C) 2010-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/signal_handlers_synchronous.h"

#include <boost/thread/mutex.hpp>
#include <exception>
#include <iostream>
#include <memory>
#include <signal.h>
#include <streambuf>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/logger/log_domain.h"
#include "mongo/logger/logger.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/text.h"

namespace mongo {

namespace {

#if defined(_WIN32)
    const char* strsignal(int signalNum) {
        // should only see SIGABRT on windows
        switch (signalNum) {
        case SIGABRT: return "SIGABRT";
        default: return "UNKNOWN";
        }
    }
#endif

    // This should only be used with MallocFreeOSteam
    class MallocFreeStreambuf : public std::streambuf {
        MONGO_DISALLOW_COPYING(MallocFreeStreambuf);
    public:
        MallocFreeStreambuf() {
            setp(_buffer, _buffer + maxLogLineSize);
        }

        StringData str() const { return StringData(pbase(), pptr() - pbase()); }
        void rewind() { setp(pbase(), epptr()); }

    private:
        static const size_t maxLogLineSize = 16*1000;
        char _buffer[maxLogLineSize];
    };

    class MallocFreeOStream : public std::ostream {
        MONGO_DISALLOW_COPYING(MallocFreeOStream);
    public:
        MallocFreeOStream() : std::ostream(&_buf) {}

        StringData str() const { return _buf.str(); }
        void rewind() { _buf.rewind(); }
    private:
        MallocFreeStreambuf _buf;
    };

    MallocFreeOStream mallocFreeOStream;

    // This guards mallocFreeOStream. While locking a pthread_mutex isn't guaranteed to be
    // signal-safe, this file does it anyway. The assumption is that the main safety risk to locking
    // a mutex is that you could deadlock with yourself. That risk is protected against by only
    // locking the mutex in fatal functions that log then exit. There is a remaining risk that one
    // of these functions recurses (possible if logging segfaults while handing a segfault). This is
    // currently acceptable because if things are that broken, there is little we can do about it.
    //
    // If in the future, we decide to be more strict about posix signal safety, we could switch to
    // an atomic test-and-set loop, possibly with a mechanism for detecting signals raised while
    // handling other signals.
    boost::mutex streamMutex;

    // must hold streamMutex to call
    void writeMallocFreeStreamToLog() {
        logger::globalLogDomain()->append(
            logger::MessageEventEphemeral(curTimeMillis64(),
                                          logger::LogSeverity::Severe(),
                                          getThreadName(),
                                          mallocFreeOStream.str()));
        mallocFreeOStream.rewind();
    }

    // must hold streamMutex to call
    void printSignalAndBacktrace(int signalNum) {
        mallocFreeOStream << "Got signal: " << signalNum << " (" << strsignal(signalNum) << ").\n";
        printStackTrace(mallocFreeOStream);
        writeMallocFreeStreamToLog();
    }

    // this will be called in certain c++ error cases, for example if there are two active
    // exceptions
    void myTerminate() {
        boost::mutex::scoped_lock lk(streamMutex);
        printStackTrace(mallocFreeOStream << "terminate() called.\n");
        writeMallocFreeStreamToLog();

#if defined(_WIN32)
        doMinidump();
#endif

        ::_exit(EXIT_ABRUPT);
    }

    // this gets called when new fails to allocate memory
    void myNewHandler() {
        boost::mutex::scoped_lock lk(streamMutex);
        printStackTrace(mallocFreeOStream << "out of memory.\n");
        writeMallocFreeStreamToLog();
        ::_exit(EXIT_ABRUPT);
    }

    void abruptQuit(int signalNum) {
        boost::mutex::scoped_lock lk(streamMutex);
        printSignalAndBacktrace(signalNum);

        // Don't go through normal shutdown procedure. It may make things worse.
        ::_exit(EXIT_ABRUPT);
    }

#if defined(_WIN32)

    void myInvalidParameterHandler(
        const wchar_t* expression,
        const wchar_t* function,
        const wchar_t* file,
        unsigned int line,
        uintptr_t pReserved) {
        severe() << "Invalid parameter detected in function " << toUtf8String(function) <<
            " File: " << toUtf8String(file) << " Line: " << line;
        severe() << "Expression: " << toUtf8String(expression);

        doMinidump();

        severe() << "immediate exit due to invalid parameter";

        abruptQuit(SIGABRT);
    }

    void myPureCallHandler() {
        severe() << "Pure call handler invoked";

        doMinidump();

        severe() << "immediate exit due to invalid pure call";

        abruptQuit(SIGABRT);
    }

#else

    void abruptQuitWithAddrSignal( int signalNum, siginfo_t *siginfo, void * ) {
        boost::mutex::scoped_lock lk(streamMutex);

        const char* action = (signalNum == SIGSEGV || signalNum == SIGBUS) ? "access" : "operation";
        mallocFreeOStream << "Invalid " << action << " at address: " << siginfo->si_addr;

        // Writing out message to log separate from the stack trace so at least that much gets
        // logged. This is important because we may get here by jumping to an invalid address which
        // could cause unwinding the stack to break.
        writeMallocFreeStreamToLog();

        printSignalAndBacktrace(signalNum);
        ::_exit(EXIT_ABRUPT);
    }

#endif

}  // namespace

    void setupSynchronousSignalHandlers() {
        std::set_terminate(myTerminate);
        std::set_new_handler(myNewHandler);

        // SIGABRT is the only signal we want handled by signal handlers on both windows and posix.
        invariant(signal(SIGABRT, abruptQuit) != SIG_ERR);

#if defined(_WIN32)
        _set_purecall_handler(myPureCallHandler);
        _set_invalid_parameter_handler(myInvalidParameterHandler);
        setWindowsUnhandledExceptionFilter();
#else
        invariant(signal(SIGHUP, SIG_IGN ) != SIG_ERR);
        invariant(signal(SIGUSR2, SIG_IGN ) != SIG_ERR);
        invariant(signal(SIGPIPE, SIG_IGN) != SIG_ERR);

        struct sigaction addrSignals;
        memset(&addrSignals, 0, sizeof(struct sigaction));
        addrSignals.sa_sigaction = abruptQuitWithAddrSignal;
        sigemptyset(&addrSignals.sa_mask);
        addrSignals.sa_flags = SA_SIGINFO;

        invariant(sigaction(SIGSEGV, &addrSignals, 0) == 0);
        invariant(sigaction(SIGBUS, &addrSignals, 0) == 0);
        invariant(sigaction(SIGILL, &addrSignals, 0) == 0);
        invariant(sigaction(SIGFPE, &addrSignals, 0) == 0);

        setupSIGTRAPforGDB();
#endif
    }
}  // namespace mongo
