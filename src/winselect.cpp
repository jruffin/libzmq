#include "winselect.hpp"
#include "err.hpp"
#include "signaler.hpp"

// From the CE5 sources, wsock.h
#define FD_FAILED_CONNECT   0x0100

// Set when the "socket" is actually a signaller and it has been triggered
#define FD_TRIGGERED        0x10000 
// Set when the "socket" is actually a signaller and we're waiting on its internal event
#define FD_SIGNALER_EVENT   0x20000 
// Set when the "socket" is actually a signaller and we're registered with it
#define FD_SIGNALER_LIST    0x40000

#ifdef ZMQ_HAVE_WINCE
struct sockInfo_t
{
    SOCKET s;
    long events;
};

static sockInfo_t& sockInfo(sockInfo_t* sockets, SOCKET& s, size_t& socketCount, size_t arraySize)
{
    // Dummy default structure returned when no index could be
    // allocated or found
    static sockInfo_t dummy = {0};

    size_t idx = -1;

    size_t i = 0;
    for (; i < socketCount; ++i) {
        if (sockets[i].s == s) {
            // Existing index found, that's it, our job's done.
            idx = i;
            break;
        }
    }

    if ( i == socketCount && (socketCount < arraySize) )
    {
        // No index found and there still are some free:
        // allocate a new one!
        idx = socketCount++;
        sockets[idx].s = s;
    }
    // else: no index found but there are none left free.
    // fail with -1.

    if (idx >= 0) {
        return sockets[idx];
    } else {
        // XXX this means some sockets might be ignored
        // in some circumstances if the sockets array
        // is smaller than 3*FD_SETSIZE...
        return dummy;
    }
}
#endif

int winselect (
        int nfds,
        fd_set* readfds,
        fd_set* writefds,
        fd_set* exceptfds,
        const struct timeval FAR * timeout
    )
{
#ifdef ZMQ_HAVE_WINCE
    // We use as many events as we can legally wait for
    WSAEVENT eventsToWaitFor[MAXIMUM_WAIT_OBJECTS];
    // The first event is the one we bind all sockets to,
    // and the one we give to signallers we cannot
    // directly wait for
    eventsToWaitFor[0] = WSACreateEvent();
    size_t eventCount = 1;

    // Any signaller above the event count limit
    // gets treated differently. Instead of taking
    // its internal event, we register our first event
    // with the signaller so that it can trigger us
    // whenever it gets set.
    zmq::signaler_t* signalers[FD_SETSIZE];
    size_t signalerCount = 0;

    sockInfo_t sockets[FD_SETSIZE] = {0};
    size_t socketCount = 0;

    size_t i;
    if (readfds) {
        for (i=0; i < readfds->fd_count; ++i) {
            SOCKET sock = readfds->fd_array[i];
            sockInfo(sockets, sock, socketCount, FD_SETSIZE).events |= FD_READ | FD_CLOSE | FD_ACCEPT;
        }
    }

    if (writefds) {
        for (i=0; i < writefds->fd_count; ++i) {
            SOCKET sock = writefds->fd_array[i];
            sockInfo(sockets, sock, socketCount, FD_SETSIZE).events |= FD_WRITE | FD_CONNECT;
        }
    }

    if (exceptfds) {
        for (i=0; i < exceptfds->fd_count; ++i) {
            SOCKET sock = exceptfds->fd_array[i];
            sockInfo(sockets, sock, socketCount, FD_SETSIZE).events |= FD_OOB | FD_FAILED_CONNECT;
        }
    }

    for (i = 0; i < socketCount; ++i) {
        // Assume that the entry is a socket. Try associating it to the event
        int rc = WSAEventSelect(sockets[i].s, eventsToWaitFor[0], sockets[i].events);
        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();
            if (err == WSAENOTSOCK) {
                // This is not a socket! Assume it is a signaler.
                // What we do next heavily depends on whether we still have
                // a free event slot to wait on...
                zmq::signaler_t* signaler = (zmq::signaler_t*) sockets[i].s;

                if (eventCount < MAXIMUM_WAIT_OBJECTS) {
                    // Okay, we do. In this case we wait on the signaller's
                    // internal event directly.
                    eventsToWaitFor[eventCount++] = signaler->getInternalEvent();
                    // Also note we are using this technique as a flag, it's important later
                    sockets[i].events |= FD_SIGNALER_EVENT;
                } else {
                    // No free event slot, we have to use the indirect way.
                    // Tell the signaller to add ourselves to the list of
                    // people who'd like to get  a heads-up when it wakes up.
                    signaler->addWaitingEvent((zmq::fd_t) eventsToWaitFor[0]);
                    signalers[signalerCount++] = signaler;
                    zmq_assert(signalerCount <= FD_SETSIZE);

                    // Also note we are using this technique as a flag, it's important later
                    sockets[i].events |= FD_SIGNALER_LIST;
                }
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

    // Wait for any of the events...
    DWORD ret = WSAWaitForMultipleEvents(eventCount,
            &eventsToWaitFor[0], FALSE, timeoutMs, FALSE);

    DWORD err = WSAGetLastError();

    // Deregister ourselves from the signalers we are registered with
    for (i=0; i < signalerCount; ++i) {
        // If the method returns true, we were still in the event list
        // of the signaler. That's a sign that this is not the one that triggered us.
        // If the method returns false the exact opposite is true:
        // the signaler in signalers[] that does NOT have us in its list anymore has triggered us!
        bool signalerDidNotTrigger = signalers[i]->removeWaitingEvent((zmq::fd_t) eventsToWaitFor[0]);

        SOCKET s = (zmq::fd_t) signalers[i];
        long& flags = sockInfo(sockets, s, socketCount, FD_SETSIZE).events |= FD_OOB | FD_FAILED_CONNECT;

        if (!signalerDidNotTrigger) {
            // This signaler triggered us, note this down in the FD flags.
            flags |= FD_TRIGGERED;
        }
    }

    if ( ret >= WSA_WAIT_EVENT_0 && ret < (WSA_WAIT_EVENT_0 + eventCount) ) {

        size_t newReadFdCount = 0;
        size_t newWriteFdCount = 0;
        size_t newExceptFdCount = 0;
        size_t triggeredFdCount = 0;

        // OK! We need to determine which FDs have been triggered, and modify the
        // fd_sets accordingly so they only contain those.

        for (i = 0; i < socketCount; ++i) {

            // Did anything happen to this socket?
            zmq::fd_t fd = (zmq::fd_t) sockets[i].s;
            long flags = sockets[i].events;
            bool hasBeenTriggered = false;

            if (flags & FD_SIGNALER_LIST) {
                // The FD is a signaler we are registered with.
                if (flags & FD_TRIGGERED) {
                    // ... and it has been triggered!
                    hasBeenTriggered = true;
                }
            } else if (flags & FD_SIGNALER_EVENT) {
                // The FD is a signaler whose internal event we used.
                // We must directly ask the event object whether it is currently set.
                zmq::signaler_t* signaler = (zmq::signaler_t*) sockets[i].s;
                if (WaitForSingleObject(signaler->getInternalEvent(), 0) == WAIT_OBJECT_0) {
                    // Yes, the event is set! The event is triggered!
                    hasBeenTriggered = true;
                }
            } else {
                // The FD is a socket, ask whether anything interesting happened to it
                WSANETWORKEVENTS events;
                int rc = WSAEnumNetworkEvents(sockets[i].s, NULL, &events);

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
