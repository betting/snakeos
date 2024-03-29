/*
 * Copyright (c) 2009-2010 by Juliusz Chroboczek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * $Id: tr-dht.c 12543 2011-07-12 12:26:24Z jordan $
 *
 */

/* ansi */
#include <errno.h>
#include <stdio.h>
#include <string.h> /* memcpy(), memset(), memchr(), strlen() */
#include <stdlib.h> /* atoi() */

/* posix */
#include <signal.h> /* sig_atomic_t */
#include <sys/time.h>
#include <unistd.h> /* close() */
#ifdef WIN32
  #include <inttypes.h>
  #define _WIN32_WINNT  0x0501	/* freeaddrinfo(),getaddrinfo(),getnameinfo() */
  #include <ws2tcpip.h>
#else
  #include <sys/types.h>
  #include <sys/socket.h> /* socket(), bind() */
  #include <netdb.h>
  #include <netinet/in.h> /* sockaddr_in */
#endif

/* third party */
#include <event2/event.h>
#include <dht/dht.h>

/* libT */
#include "transmission.h"
#include "bencode.h"
#include "crypto.h"
#include "net.h"
#include "peer-mgr.h" /* tr_peerMgrCompactToPex() */
#include "platform.h" /* tr_threadNew() */
#include "session.h"
#include "torrent.h" /* tr_torrentFindFromHash() */
#include "tr-dht.h"
#include "trevent.h" /* tr_runInEventThread() */
#include "utils.h"

static struct event *dht_timer = NULL;
static unsigned char myid[20];
static tr_session *session = NULL;

static void timer_callback(int s, short type, void *ignore);

struct bootstrap_closure {
    tr_session *session;
    uint8_t *nodes;
    uint8_t *nodes6;
    size_t len, len6;
};

static int
bootstrap_done( tr_session *session, int af )
{
    int status;

    if(af == 0)
        return
            bootstrap_done(session, AF_INET) &&
            bootstrap_done(session, AF_INET6);

    status = tr_dhtStatus(session, af, NULL);
    return status == TR_DHT_STOPPED || status >= TR_DHT_FIREWALLED;
}

static void
nap( int roughly_sec )
{
    const int roughly_msec = roughly_sec * 1000;
    const int msec = roughly_msec/2 + tr_cryptoWeakRandInt(roughly_msec);
    tr_wait_msec( msec );
}

static int
bootstrap_af(tr_session *session)
{
    if( bootstrap_done(session, AF_INET6) )
        return AF_INET;
    else if ( bootstrap_done(session, AF_INET) )
        return AF_INET6;
    else
        return 0;
}

static void
bootstrap_from_name( const char *name, tr_port port, int af )
{
    struct addrinfo hints, *info, *infop;
    char pp[10];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = af;
    /* No, just passing p + 1 to gai won't work. */
    tr_snprintf(pp, sizeof(pp), "%d", (int)port);

    rc = getaddrinfo(name, pp, &hints, &info);
    if(rc != 0) {
        tr_nerr("DHT", "%s:%s: %s", name, pp, gai_strerror(rc));
        return;
    }

    infop = info;
    while(infop) {
        dht_ping_node(infop->ai_addr, infop->ai_addrlen);

        nap(15);

        if(bootstrap_done(session, af))
            break;
        infop = infop->ai_next;
    }
    freeaddrinfo(info);
}

static void
dht_bootstrap(void *closure)
{
    struct bootstrap_closure *cl = closure;
    int i;
    int num = cl->len / 6, num6 = cl->len6 / 18;

    if(session != cl->session)
        return;

    if(cl->len > 0)
        tr_ninf( "DHT", "Bootstrapping from %d nodes", num );

    if(cl->len6 > 0)
        tr_ninf( "DHT", "Bootstrapping from %d IPv6 nodes", num6 );


    for(i = 0; i < MAX(num, num6); i++) {
        if( i < num && !bootstrap_done(cl->session, AF_INET) ) {
            tr_port port;
            struct tr_address addr;

            memset(&addr, 0, sizeof(addr));
            addr.type = TR_AF_INET;
            memcpy(&addr.addr.addr4, &cl->nodes[i * 6], 4);
            memcpy(&port, &cl->nodes[i * 6 + 4], 2);
            port = ntohs(port);
            tr_dhtAddNode(cl->session, &addr, port, 1);
        }
        if( i < num6 && !bootstrap_done(cl->session, AF_INET6) ) {
            tr_port port;
            struct tr_address addr;

            memset(&addr, 0, sizeof(addr));
            addr.type = TR_AF_INET6;
            memcpy(&addr.addr.addr6, &cl->nodes6[i * 18], 16);
            memcpy(&port, &cl->nodes6[i * 18 + 16], 2);
            port = ntohs(port);
            tr_dhtAddNode(cl->session, &addr, port, 1);
        }

        /* Our DHT code is able to take up to 9 nodes in a row without
           dropping any. After that, it takes some time to split buckets.
           So ping the first 8 nodes quickly, then slow down. */
        if(i < 8)
            nap(2);
        else
            nap(15);

        if(bootstrap_done( session, 0 ))
            break;
    }

    if(!bootstrap_done(cl->session, 0)) {
        char *bootstrap_file;
        FILE *f = NULL;

        bootstrap_file =
            tr_buildPath(cl->session->configDir, "dht.bootstrap", NULL);

        if(bootstrap_file)
            f = fopen(bootstrap_file, "rb");
        if(f != NULL) {
            tr_ninf("DHT", "Attempting manual bootstrap");
            for(;;) {
                char buf[201];
                char *p;
                int port = 0;

                p = fgets(buf, 200, f);
                if( p == NULL )
                    break;

                p = memchr(buf, ' ', strlen(buf));
                if(p != NULL)
                    port = atoi(p + 1);
                if(p == NULL || port <= 0 || port >= 0x10000) {
                    tr_nerr("DHT", "Couldn't parse %s", buf);
                    continue;
                }

                *p = '\0';

                bootstrap_from_name( buf, port, bootstrap_af(session) );

                if(bootstrap_done(cl->session, 0))
                    break;
            }
            fclose( f );
        }

        tr_free( bootstrap_file );
    }

    if(!bootstrap_done(cl->session, 0)) {
        for(i = 0; i < 6; i++) {
            /* We don't want to abuse our bootstrap nodes, so be very
               slow.  The initial wait is to give other nodes a chance
               to contact us before we attempt to contact a bootstrap
               node, for example because we've just been restarted. */
            nap(40);
            if(bootstrap_done(cl->session, 0))
                break;
            if(i == 0)
                tr_ninf("DHT",
                        "Attempting bootstrap from dht.transmissionbt.com");
            bootstrap_from_name( "dht.transmissionbt.com", 6881,
                                 bootstrap_af(session) );
        }
    }

    if( cl->nodes )
        tr_free( cl->nodes );
    if( cl->nodes6 )
        tr_free( cl->nodes6 );
    tr_free( closure );
    tr_ndbg( "DHT", "Finished bootstrapping" );
}

int
tr_dhtInit(tr_session *ss)
{
    tr_benc benc;
    int rc;
    bool have_id = false;
    char * dat_file;
    uint8_t * nodes = NULL, * nodes6 = NULL;
    const uint8_t * raw;
    size_t len, len6;
    struct bootstrap_closure * cl;

    if( session ) /* already initialized */
        return -1;

    tr_ndbg( "DHT", "Initializing DHT" );

    if( getenv( "TR_DHT_VERBOSE" ) != NULL )
        dht_debug = stderr;

    dat_file = tr_buildPath( ss->configDir, "dht.dat", NULL );
    rc = tr_bencLoadFile( &benc, TR_FMT_BENC, dat_file );
    tr_free( dat_file );
    if(rc == 0) {
        have_id = tr_bencDictFindRaw(&benc, "id", &raw, &len);
        if( have_id && len==20 )
            memcpy( myid, raw, len );
        if( ss->udp_socket >= 0 &&
            tr_bencDictFindRaw( &benc, "nodes", &raw, &len ) && !(len%6) ) {
                nodes = tr_memdup( raw, len );
        }
        if( ss->udp6_socket > 0 &&
            tr_bencDictFindRaw( &benc, "nodes6", &raw, &len6 ) && !(len6%18) ) {
            nodes6 = tr_memdup( raw, len6 );
        }
        tr_bencFree( &benc );
    }

    if(nodes == NULL)
        len = 0;
    if(nodes6 == NULL)
        len6 = 0;

    if( have_id )
        tr_ninf( "DHT", "Reusing old id" );
    else {
        /* Note that DHT ids need to be distributed uniformly,
         * so it should be something truly random. */
        tr_ninf( "DHT", "Generating new id" );
        tr_cryptoRandBuf( myid, 20 );
    }

    rc = dht_init( ss->udp_socket, ss->udp6_socket, myid, NULL );
    if( rc < 0 )
        goto fail;

    session = ss;

    cl = tr_new( struct bootstrap_closure, 1 );
    cl->session = session;
    cl->nodes = nodes;
    cl->nodes6 = nodes6;
    cl->len = len;
    cl->len6 = len6;
    tr_threadNew( dht_bootstrap, cl );

    dht_timer = evtimer_new( session->event_base, timer_callback, session );
    tr_timerAdd( dht_timer, 0, tr_cryptoWeakRandInt( 1000000 ) );

    tr_ndbg( "DHT", "DHT initialized" );

    return 1;

 fail:
    tr_ndbg( "DHT", "DHT initialization failed (errno = %d)", errno );
    session = NULL;
    return -1;
}

void
tr_dhtUninit(tr_session *ss)
{
    if(session != ss)
        return;

    tr_ndbg( "DHT", "Uninitializing DHT" );

    if( dht_timer != NULL ) {
        event_free( dht_timer );
        dht_timer = NULL;
    }

    /* Since we only save known good nodes, avoid erasing older data if we
       don't know enough nodes. */
    if(tr_dhtStatus(ss, AF_INET, NULL) < TR_DHT_FIREWALLED)
        tr_ninf( "DHT", "Not saving nodes, DHT not ready" );
    else {
        tr_benc benc;
        struct sockaddr_in sins[300];
        struct sockaddr_in6 sins6[300];
        char compact[300 * 6], compact6[300 * 18];
        char *dat_file;
        int i, j, num = 300, num6 = 300;
        int n = dht_get_nodes(sins, &num, sins6, &num6);

        tr_ninf( "DHT", "Saving %d (%d + %d) nodes", n, num, num6 );

        j = 0;
        for( i=0; i<num; ++i ) {
            memcpy( compact + j, &sins[i].sin_addr, 4 );
            memcpy( compact + j + 4, &sins[i].sin_port, 2 );
            j += 6;
        }
        j = 0;
        for( i=0; i<num6; ++i ) {
            memcpy( compact6 + j, &sins6[i].sin6_addr, 16 );
            memcpy( compact6 + j + 16, &sins6[i].sin6_port, 2 );
            j += 18;
        }
        tr_bencInitDict( &benc, 3 );
        tr_bencDictAddRaw( &benc, "id", myid, 20 );
        if(num > 0)
            tr_bencDictAddRaw( &benc, "nodes", compact, num * 6 );
        if(num6 > 0)
            tr_bencDictAddRaw( &benc, "nodes6", compact6, num6 * 18 );
        dat_file = tr_buildPath( ss->configDir, "dht.dat", NULL );
        tr_bencToFile( &benc, TR_FMT_BENC, dat_file );
        tr_bencFree( &benc );
        tr_free( dat_file );
    }

    dht_uninit();
    tr_ndbg("DHT", "Done uninitializing DHT");

    session = NULL;
}

bool
tr_dhtEnabled( const tr_session * ss )
{
    return ss && ( ss == session );
}

struct getstatus_closure
{
    int af;
    sig_atomic_t status;
    sig_atomic_t count;
};

static void
getstatus( void * cl )
{
    struct getstatus_closure * closure = cl;
    int good, dubious, incoming;

    dht_nodes( closure->af, &good, &dubious, NULL, &incoming );

    closure->count = good + dubious;

    if( good < 4 || good + dubious <= 8 )
        closure->status = TR_DHT_BROKEN;
    else if( good < 40 )
        closure->status = TR_DHT_POOR;
    else if( incoming < 8 )
        closure->status = TR_DHT_FIREWALLED;
    else
        closure->status = TR_DHT_GOOD;
}

int
tr_dhtStatus( tr_session * session, int af, int * nodes_return )
{
    struct getstatus_closure closure = { af, -1, -1 };

    if( !tr_dhtEnabled( session ) ||
        (af == AF_INET && session->udp_socket < 0) ||
        (af == AF_INET6 && session->udp6_socket < 0) ) {
        if( nodes_return )
            *nodes_return = 0;
        return TR_DHT_STOPPED;
    }

    tr_runInEventThread( session, getstatus, &closure );
    while( closure.status < 0 )
        tr_wait_msec( 50 /*msec*/ );

    if( nodes_return )
        *nodes_return = closure.count;

    return closure.status;
}

tr_port
tr_dhtPort( tr_session *ss )
{
    return tr_dhtEnabled( ss ) ? ss->udp_port : 0;
}

int
tr_dhtAddNode( tr_session       * ss,
               const tr_address * address,
               tr_port            port,
               bool            bootstrap )
{
    int af = address->type == TR_AF_INET ? AF_INET : AF_INET6;

    if( !tr_dhtEnabled( ss ) )
        return 0;

    /* Since we don't want to abuse our bootstrap nodes,
     * we don't ping them if the DHT is in a good state. */

    if(bootstrap) {
        if(tr_dhtStatus(ss, af, NULL) >= TR_DHT_FIREWALLED)
            return 0;
    }

    if( address->type == TR_AF_INET ) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        memcpy(&sin.sin_addr, &address->addr.addr4, 4);
        sin.sin_port = htons(port);
        dht_ping_node((struct sockaddr*)&sin, sizeof(sin));
        return 1;
    } else if( address->type == TR_AF_INET6 ) {
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        memcpy(&sin6.sin6_addr, &address->addr.addr6, 16);
        sin6.sin6_port = htons(port);
        dht_ping_node((struct sockaddr*)&sin6, sizeof(sin6));
        return 1;
    }

    return 0;
}

const char *
tr_dhtPrintableStatus(int status)
{
    switch(status) {
    case TR_DHT_STOPPED: return "stopped";
    case TR_DHT_BROKEN: return "broken";
    case TR_DHT_POOR: return "poor";
    case TR_DHT_FIREWALLED: return "firewalled";
    case TR_DHT_GOOD: return "good";
    default: return "???";
    }
}

static void
callback( void *ignore UNUSED, int event,
          unsigned char *info_hash, void *data, size_t data_len )
{
    if( event == DHT_EVENT_VALUES || event == DHT_EVENT_VALUES6 ) {
        tr_torrent *tor;
        tr_sessionLock( session );
        tor = tr_torrentFindFromHash( session, info_hash );
        if( tor && tr_torrentAllowsDHT( tor ))
        {
            size_t i, n;
            tr_pex * pex;
            if( event == DHT_EVENT_VALUES )
                pex = tr_peerMgrCompactToPex(data, data_len, NULL, 0, &n);
            else
                pex = tr_peerMgrCompact6ToPex(data, data_len, NULL, 0, &n);
            for( i=0; i<n; ++i )
                tr_peerMgrAddPex( tor, TR_PEER_FROM_DHT, pex+i, -1 );
            tr_free(pex);
            tr_tordbg(tor, "Learned %d%s peers from DHT",
                      (int)n,
                      event == DHT_EVENT_VALUES6 ? " IPv6" : "");
        }
        tr_sessionUnlock( session );
    } else if( event == DHT_EVENT_SEARCH_DONE ||
               event == DHT_EVENT_SEARCH_DONE6) {
        tr_torrent * tor = tr_torrentFindFromHash( session, info_hash );
        if( tor ) {
            if( event == DHT_EVENT_SEARCH_DONE ) {
                tr_torinf(tor, "DHT announce done");
                tor->dhtAnnounceInProgress = 0;
            } else {
                tr_torinf(tor, "IPv6 DHT announce done");
                tor->dhtAnnounce6InProgress = 0;
            }
        }
    }
}

static int
tr_dhtAnnounce(tr_torrent *tor, int af, bool announce)
{
    int rc, status, numnodes, ret = 0;

    if( !tr_torrentAllowsDHT( tor ) )
        return -1;

    status = tr_dhtStatus( tor->session, af, &numnodes );

    if( status == TR_DHT_STOPPED ) {
        /* Let the caller believe everything is all right. */
        return 1;
    }

    if(status >= TR_DHT_POOR ) {
        rc = dht_search( tor->info.hash,
                         announce ? tr_sessionGetPeerPort(session) : 0,
                         af, callback, NULL);
        if( rc >= 1 ) {
            tr_torinf(tor, "Starting%s DHT announce (%s, %d nodes)",
                      af == AF_INET6 ? " IPv6" : "",
                      tr_dhtPrintableStatus(status), numnodes);
            if(af == AF_INET)
                tor->dhtAnnounceInProgress = true;
            else
                tor->dhtAnnounce6InProgress = true;
            ret = 1;
        } else {
            tr_torerr(tor, "%sDHT announce failed (%s, %d nodes): %s",
                      af == AF_INET6 ? "IPv6 " : "",
                      tr_dhtPrintableStatus(status), numnodes,
                      tr_strerror( errno ) );
        }
    } else {
        tr_tordbg(tor, "%sDHT not ready (%s, %d nodes)",
                  af == AF_INET6 ? "IPv6 " : "",
                  tr_dhtPrintableStatus(status), numnodes);
    }

    return ret;
}

void
tr_dhtUpkeep( tr_session * session )
{
    tr_torrent * tor = NULL;
    const time_t now = tr_time( );

    while(( tor = tr_torrentNext( session, tor )))
    {
        if( !tor->isRunning || !tr_torrentAllowsDHT( tor ) )
            continue;

        if( tor->dhtAnnounceAt <= now )
        {
            const int rc = tr_dhtAnnounce(tor, AF_INET, 1);

            tor->dhtAnnounceAt = now + ((rc == 0)
                                     ? 5 + tr_cryptoWeakRandInt( 5 )
                                     : 25 * 60 + tr_cryptoWeakRandInt( 3*60 ));
        }

        if( tor->dhtAnnounce6At <= now )
        {
            const int rc = tr_dhtAnnounce(tor, AF_INET6, 1);

            tor->dhtAnnounce6At = now + ((rc == 0)
                                      ? 5 + tr_cryptoWeakRandInt( 5 )
                                      : 25 * 60 + tr_cryptoWeakRandInt( 3*60 ));
        }
    }
}

void
tr_dhtCallback(unsigned char *buf, int buflen,
               struct sockaddr *from, socklen_t fromlen,
               void *sv )
{
    time_t tosleep;
    int rc;

    assert(tr_isSession(sv));

    if(sv != session)
        return;

    rc = dht_periodic( buf, buflen, from, fromlen,
                       &tosleep, callback, NULL);
    if(rc < 0) {
        if(errno == EINTR) {
            tosleep = 0;
        } else {
            tr_nerr( "DHT", "dht_periodic failed: %s", tr_strerror( errno ) );
            if(errno == EINVAL || errno == EFAULT)
                    abort();
            tosleep = 1;
        }
    }

    /* Being slightly late is fine,
       and has the added benefit of adding some jitter. */
    tr_timerAdd( dht_timer, tosleep, tr_cryptoWeakRandInt( 1000000 ) );
}

static void
timer_callback(int s UNUSED, short type UNUSED, void *session )
{
    tr_dhtCallback(NULL, 0, NULL, 0, session);
}


void
dht_hash(void *hash_return, int hash_size,
         const void *v1, int len1,
         const void *v2, int len2,
         const void *v3, int len3)
{
    unsigned char sha1[SHA_DIGEST_LENGTH];
    tr_sha1( sha1, v1, len1, v2, len2, v3, len3, NULL );
    memset( hash_return, 0, hash_size );
    memcpy( hash_return, sha1, MIN( hash_size, SHA_DIGEST_LENGTH ) );
}

int
dht_random_bytes( void * buf, size_t size )
{
    tr_cryptoRandBuf( buf, size );
    return size;
}
