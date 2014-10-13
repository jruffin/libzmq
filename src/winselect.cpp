#include "winselect.hpp"
#include "err.hpp"
#include <map>
#include "signaler.hpp"

// From the CE5 sources, wsock.h
#define FD_FAILED_CONNECT   0x0100

// Made-up "triggered" flag we use internally
#define FD_TRIGGERED 0x10000
#define FD_SIGNALER  0x20000

int winselect (
        int nfds,
        fd_set* readfds,
        fd_set* writefds,
        fd_set* exceptfds,
        const struct timeval FAR * timeout
    )
{
#ifdef ZMQ_HAVE_WINCE
    WSAEVENT eventsToWaitFor[1] = {0};
    size_t eventCount = 1;
    zmq::signaler_t* signalers[FD_SETSIZE];
    size_t signalerCount = 0;

    // SOCKETS EVENT
    eventsToWaitFor[0] = WSACreateEvent();

    std::map<SOCKET, long> sockEvents;

    size_t i;
    if (readfds) {
        for (i=0; i < readfds->fd_count; ++i) {
            SOCKET sock = readfds->fd_array[i];
            sockEvents[sock] |= FD_READ | FD_CLOSE | FD_ACCEPT;
        }
    }

    if (writefds) {
        for (i=0; i < writefds->fd_count; ++i) {
            SOCKET sock = writefds->fd_array[i];
            sockEvents[sock] |= FD_WRITE | FD_CONNECT;
        }
    }

    if (exceptfds) {
        for (i=0; i < exceptfds->fd_count; ++i) {
            SOCKET sock = exceptfds->fd_array[i];
            sockEvents[sock] |= FD_OOB | FD_FAILED_CONNECT;
        }
    }

    std::map<SOCKET, long>::iterator it;
    for (it = sockEvents.begin(); it != sockEvents.end(); ++it) {
        // Assume that the entry is a socket. Try associating it to the event
        int rc = WSAEventSelect(it->first, eventsToWaitFor[0], it->second);
        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();
            if (err == WSAENOTSOCK) {
                // This is not a socket! Assume it is a signaler, so
                // add ourselves to the list of people who'd like to get
                // a heads-up when it wakes up
                zmq::signaler_t* signaler = (zmq::signaler_t*) it->first;
                signaler->addWaitingEvent((zmq::fd_t) eventsToWaitFor[0]);
                signalers[signalerCount++] = signaler;
                zmq_assert(signalerCount <= FD_SETSIZE);
            } else {
                // Some other type of error that should definitely not happen.
                wsa_assert_no(err);
            }
        }
    }

    DWORD timeoutMs = WSA_INFINITE;
    if (timeout) {
        timeoutMs = (timeout->tv_sec*1000) + (timeout->tv_usec/1000);
    }

    // If the timeout is zero, there is no real waiting and no
    // context switch, only checking whether the event flags are set.
    // In this case, no need for yielding, so no need to shuffle
    // the priorities.
    int priority = 0;
    if (timeoutMs > 0) {
        priority = CeGetThreadPriority(GetCurrentThread());
        CeSetThreadPriority(GetCurrentThread(), 247);
    }

    // Wait for any of the events...
    DWORD ret = WSAWaitForMultipleEvents(eventCount,
            eventsToWaitFor, FALSE, timeoutMs, FALSE);

    // If the timeout is larger than zero, we are waiting and
    // there is a context switch involved. Should we return
    // with WSA_WAIT_EVENT_0 or so, somebody triggered our event
    // and got its execution time stolen by us as a consequence.
    // We want to yield back to the caller, which can be done with
    // Sleep(0) as long as the caller has the same priority as us.
    // This is why we set it to 247 earlier - the signaler_t class
    // sets its thread priority to 247 prior to setting the events,
    // so we have a fair guarantee to yield to our signaler with
    // Sleep(0) - though we might yield to another signaler like this,
    // which is not all that bad.
    if (timeoutMs > 0) {
        if (ret >= WSA_WAIT_EVENT_0 && ret < WSA_WAIT_EVENT_0 + eventCount) {
            // Yield, because the thread that has signalled us is now inactive.
            // We want to return to it!
            Sleep(0);
        }

        CeSetThreadPriority(GetCurrentThread(), priority);
    }

    DWORD err = WSAGetLastError();

    // Deregister ourselves from the signalers
    for (i=0; i < signalerCount; ++i) {
        // If the method returns true, we were still in the event list
        // of the signaler. That's a sign that this is not the one that triggered us.
        // If the method returns false the exact opposite is true:
        // the signaler in signalers[] that does NOT have us in its list anymore has triggered us!
        bool signalerDidNotTrigger = signalers[i]->removeWaitingEvent((zmq::fd_t) eventsToWaitFor[0]);

        long& flags = sockEvents[(zmq::fd_t) signalers[i]];

        // Mark the FD as a signaler, useful later.
        flags |= FD_SIGNALER;

        if (!signalerDidNotTrigger) {
            // This signaler triggered us, note this down in the FD flags.
            flags |= FD_TRIGGERED;
        }
    }

    if (ret >= WSA_WAIT_EVENT_0 && ret < WSA_WAIT_EVENT_0 + eventCount) {

        size_t newReadFdCount = 0;
        size_t newWriteFdCount = 0;
        size_t newExceptFdCount = 0;
        size_t triggeredFdCount = 0;

        // OK! We need to determine which FDs have been triggered, and modify the
        // fd_sets accordingly so they only contain those.

        std::map<SOCKET, long>::iterator it;
        for (it = sockEvents.begin(); it != sockEvents.end(); ++it) {

            // Did anything happen to this socket?
            zmq::fd_t fd = (zmq::fd_t) it->first;
            long flags = it->second;
            bool hasBeenTriggered = false;

            if (flags & FD_SIGNALER) {
                // The FD is a signaler...
                if (flags & FD_TRIGGERED) {
                    // ... and it has been triggered!
                    hasBeenTriggered = true;
                }
            } else {
                // The FD is a socket, ask whether anything interesting happened to it
                WSANETWORKEVENTS events;
                int rc = WSAEnumNetworkEvents(it->first, NULL, &events);

                if (rc == 0) {
                    if (events.lNetworkEvents != 0) {
                        // Yes, something happened, the socket has been triggered.
                        hasBeenTriggered = true;
                    }
                } else {
                    wsa_assert(rc);
                }
            }

            if (hasBeenTriggered) {
                // The FD has been triggered. Move it into the corresponding fd_sets.
                if (flags & FD_READ) {
                    readfds->fd_array[newReadFdCount++] = fd;
                }
                if (flags & FD_WRITE) {
                    writefds->fd_array[newWriteFdCount++] = fd;
                }
                if (flags & FD_FAILED_CONNECT) {
                    exceptfds->fd_array[newExceptFdCount++] = fd;
                }

                ++triggeredFdCount;
            }
        }

        if (readfds) {
            readfds->fd_count = newReadFdCount;
        }

        if (writefds) {
            writefds->fd_count = newWriteFdCount;
        }

        if (exceptfds) {
            exceptfds->fd_count = newExceptFdCount;
        }

        WSACloseEvent(eventsToWaitFor[0]);
        WSASetLastError(err);

        return triggeredFdCount;

    } else if (ret == WSA_WAIT_TIMEOUT) {
        // Timeout.
        WSACloseEvent(eventsToWaitFor[0]);
        WSASetLastError(err);
        return 0;
    } else {
        // Error.
        WSACloseEvent(eventsToWaitFor[0]);
        WSASetLastError(err);
        return SOCKET_ERROR;
    }
#else
    return select(nfds, readfds, writefds, exceptfds, timeout);
#endif
}
