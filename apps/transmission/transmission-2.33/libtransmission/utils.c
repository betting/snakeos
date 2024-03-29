/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: utils.c 12551 2011-07-17 14:15:02Z jordan $
 */

#ifdef HAVE_MEMMEM
 #define _GNU_SOURCE /* glibc's string.h needs this to pick up memmem */
#endif

#if defined(SYS_DARWIN)
 #define HAVE_GETPAGESIZE
 #define HAVE_ICONV_OPEN
 #define HAVE_VALLOC
 #undef HAVE_POSIX_MEMALIGN /* not supported on OS X 10.5 and lower */
#endif

#include <assert.h>
#include <ctype.h> /* isdigit(), isalpha(), tolower() */
#include <errno.h>
#include <float.h> /* DBL_EPSILON */
#include <locale.h> /* localeconv() */
#include <math.h> /* pow(), fabs(), floor() */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* strerror(), memset(), memmem() */
#include <time.h> /* nanosleep() */

#ifdef HAVE_ICONV_OPEN
 #include <iconv.h>
#endif
#include <libgen.h> /* basename() */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h> /* stat(), getcwd(), getpagesize(), unlink() */

#include <event2/buffer.h>
#include <event2/event.h>

#ifdef WIN32
 #include <w32api.h>
 #define WINVER WindowsXP /* freeaddrinfo(), getaddrinfo(), getnameinfo() */
 #include <direct.h> /* _getcwd() */
 #include <windows.h> /* Sleep() */
#endif

#include "transmission.h"
#include "bencode.h"
#include "fdlimit.h"
#include "ConvertUTF.h"
#include "list.h"
#include "utils.h"
#include "platform.h" /* tr_lockLock(), TR_PATH_MAX */
#include "version.h"


time_t       __tr_current_time   = 0;
tr_msg_level __tr_message_level  = TR_MSG_ERR;

static bool           messageQueuing = false;
static tr_msg_list *  messageQueue = NULL;
static tr_msg_list ** messageQueueTail = &messageQueue;
static int            messageQueueCount = 0;

#ifndef WIN32
    /* make null versions of these win32 functions */
    static inline int IsDebuggerPresent( void ) { return false; }
    static inline void OutputDebugString( const void * unused UNUSED ) { }
#endif

/***
****
***/

static tr_lock*
getMessageLock( void )
{
    static tr_lock * l = NULL;

    if( !l )
        l = tr_lockNew( );

    return l;
}

void*
tr_getLog( void )
{
    static bool initialized = false;
    static FILE * file = NULL;

    if( !initialized )
    {
        const char * str = getenv( "TR_DEBUG_FD" );
        int          fd = 0;
        if( str && *str )
            fd = atoi( str );
        switch( fd )
        {
            case 1:
                file = stdout; break;

            case 2:
                file = stderr; break;

            default:
                file = NULL; break;
        }
        initialized = true;
    }

    return file;
}

void
tr_setMessageLevel( tr_msg_level level )
{
    __tr_message_level = level;
}

void
tr_setMessageQueuing( bool enabled )
{
    messageQueuing = enabled;
}

bool
tr_getMessageQueuing( void )
{
    return messageQueuing != 0;
}

tr_msg_list *
tr_getQueuedMessages( void )
{
    tr_msg_list * ret;
    tr_lockLock( getMessageLock( ) );

    ret = messageQueue;
    messageQueue = NULL;
    messageQueueTail = &messageQueue;

    messageQueueCount = 0;

    tr_lockUnlock( getMessageLock( ) );
    return ret;
}

void
tr_freeMessageList( tr_msg_list * list )
{
    tr_msg_list * next;

    while( NULL != list )
    {
        next = list->next;
        free( list->message );
        free( list->name );
        free( list );
        list = next;
    }
}

/**
***
**/

struct tm *
tr_localtime_r( const time_t *_clock, struct tm *_result )
{
#ifdef HAVE_LOCALTIME_R
    return localtime_r( _clock, _result );
#else
    struct tm *p = localtime( _clock );
    if( p )
        *(_result) = *p;
    return p;
#endif
}

char*
tr_getLogTimeStr( char * buf, int buflen )
{
    char           tmp[64];
    struct tm      now_tm;
    struct timeval tv;
    time_t         seconds;
    int            milliseconds;

    gettimeofday( &tv, NULL );

    seconds = tv.tv_sec;
    tr_localtime_r( &seconds, &now_tm );
    strftime( tmp, sizeof( tmp ), "%H:%M:%S", &now_tm );
    milliseconds = tv.tv_usec / 1000;
    tr_snprintf( buf, buflen, "%s.%03d", tmp, milliseconds );

    return buf;
}

bool
tr_deepLoggingIsActive( void )
{
    static int8_t deepLoggingIsActive = -1;

    if( deepLoggingIsActive < 0 )
        deepLoggingIsActive = IsDebuggerPresent() || (tr_getLog()!=NULL);

    return deepLoggingIsActive != 0;
}

void
tr_deepLog( const char  * file,
            int           line,
            const char  * name,
            const char  * fmt,
            ... )
{
    FILE * fp = tr_getLog( );
    if( fp || IsDebuggerPresent( ) )
    {
        va_list           args;
        char              timestr[64];
        struct evbuffer * buf = evbuffer_new( );
        char *            base = tr_basename( file );

        evbuffer_add_printf( buf, "[%s] ",
                            tr_getLogTimeStr( timestr, sizeof( timestr ) ) );
        if( name )
            evbuffer_add_printf( buf, "%s ", name );
        va_start( args, fmt );
        evbuffer_add_vprintf( buf, fmt, args );
        va_end( args );
        evbuffer_add_printf( buf, " (%s:%d)\n", base, line );
        /* FIXME(libevent2) ifdef this out for nonwindows platforms */
        OutputDebugString( evbuffer_pullup( buf, -1 ) );
        if( fp )
            fputs( (const char*)evbuffer_pullup( buf, -1 ), fp );

        tr_free( base );
        evbuffer_free( buf );
    }
}

/***
****
***/

void
tr_msg( const char * file, int line,
        tr_msg_level level,
        const char * name,
        const char * fmt, ... )
{
    const int err = errno; /* message logging shouldn't affect errno */
    char buf[1024];
    va_list ap;
    tr_lockLock( getMessageLock( ) );

    /* build the text message */
    *buf = '\0';
    va_start( ap, fmt );
    evutil_vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    OutputDebugString( buf );

    if( *buf )
    {
        if( messageQueuing )
        {
            tr_msg_list * newmsg;
            newmsg = tr_new0( tr_msg_list, 1 );
            newmsg->level = level;
            newmsg->when = tr_time( );
            newmsg->message = tr_strdup( buf );
            newmsg->file = file;
            newmsg->line = line;
            newmsg->name = tr_strdup( name );

            *messageQueueTail = newmsg;
            messageQueueTail = &newmsg->next;
            ++messageQueueCount;

            if( messageQueueCount > TR_MAX_MSG_LOG )
            {
                tr_msg_list * old = messageQueue;
                messageQueue = old->next;
                old->next = NULL;
                tr_freeMessageList(old);

                --messageQueueCount;

                assert( messageQueueCount == TR_MAX_MSG_LOG );
            }
        }
        else
        {
            char timestr[64];
            FILE * fp;

            fp = tr_getLog( );
            if( fp == NULL )
                fp = stderr;

            tr_getLogTimeStr( timestr, sizeof( timestr ) );

            if( name )
                fprintf( fp, "[%s] %s: %s\n", timestr, name, buf );
            else
                fprintf( fp, "[%s] %s\n", timestr, buf );
            fflush( fp );
        }
    }

    tr_lockUnlock( getMessageLock( ) );
    errno = err;
}

/***
****
***/

void*
tr_malloc( size_t size )
{
    return size ? malloc( size ) : NULL;
}

void*
tr_malloc0( size_t size )
{
    return size ? calloc( 1, size ) : NULL;
}

void
tr_free( void * p )
{
    if( p != NULL )
        free( p );
}

void*
tr_memdup( const void * src, size_t byteCount )
{
    return memcpy( tr_malloc( byteCount ), src, byteCount );
}

/***
****
***/

const char*
tr_strip_positional_args( const char* str )
{
    const char * in = str;
    static size_t bufsize = 0;
    static char * buf = NULL;
    const size_t  len = str ? strlen( str ) : 0;
    char *        out;

    if( !buf || ( bufsize < len ) )
    {
        bufsize = len * 2 + 1;
        buf = tr_renew( char, buf, bufsize );
    }

    for( out = buf; str && *str; ++str )
    {
        *out++ = *str;

        if( ( *str == '%' ) && isdigit( str[1] ) )
        {
            const char * tmp = str + 1;
            while( isdigit( *tmp ) )
                ++tmp;
            if( *tmp == '$' )
                str = tmp[1]=='\'' ? tmp+1 : tmp;
        }

        if( ( *str == '%' ) && ( str[1] == '\'' ) )
            str = str + 1;

    }
    *out = '\0';

    return !in || strcmp( buf, in ) ? buf : in;
}

/**
***
**/

void
tr_timerAdd( struct event * timer, int seconds, int microseconds )
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = microseconds;

    assert( tv.tv_sec >= 0 );
    assert( tv.tv_usec >= 0 );
    assert( tv.tv_usec < 1000000 );

    evtimer_add( timer, &tv );
}

void
tr_timerAddMsec( struct event * timer, int msec )
{
    const int seconds =  msec / 1000;
    const int usec = (msec%1000) * 1000;
    tr_timerAdd( timer, seconds, usec );
}

/**
***
**/

uint8_t *
tr_loadFile( const char * path,
             size_t *     size )
{
    uint8_t * buf;
    struct stat  sb;
    int fd;
    ssize_t n;
    const char * const err_fmt = _( "Couldn't read \"%1$s\": %2$s" );

    /* try to stat the file */
    errno = 0;
    if( stat( path, &sb ) )
    {
        const int err = errno;
        tr_dbg( err_fmt, path, tr_strerror( errno ) );
        errno = err;
        return NULL;
    }

    if( ( sb.st_mode & S_IFMT ) != S_IFREG )
    {
        tr_err( err_fmt, path, _( "Not a regular file" ) );
        errno = EISDIR;
        return NULL;
    }

    /* Load the torrent file into our buffer */
    fd = tr_open_file_for_scanning( path );
    if( fd < 0 )
    {
        const int err = errno;
        tr_err( err_fmt, path, tr_strerror( errno ) );
        errno = err;
        return NULL;
    }
    buf = tr_malloc( sb.st_size + 1 );
    if( !buf )
    {
        const int err = errno;
        tr_err( err_fmt, path, _( "Memory allocation failed" ) );
        tr_close_file( fd );
        errno = err;
        return NULL;
    }
    n = read( fd, buf, (size_t)sb.st_size );
    if( n == -1 )
    {
        const int err = errno;
        tr_err( err_fmt, path, tr_strerror( errno ) );
        tr_close_file( fd );
        free( buf );
        errno = err;
        return NULL;
    }

    tr_close_file( fd );
    buf[ sb.st_size ] = '\0';
    *size = sb.st_size;
    return buf;
}

char*
tr_basename( const char * path )
{
    char * tmp = tr_strdup( path );
    char * ret = tr_strdup( basename( tmp ) );
    tr_free( tmp );
    return ret;
}

char*
tr_dirname( const char * path )
{
    char * tmp = tr_strdup( path );
    char * ret = tr_strdup( dirname( tmp ) );
    tr_free( tmp );
    return ret;
}

int
tr_mkdir( const char * path,
          int permissions
#ifdef WIN32
                       UNUSED
#endif
        )
{
#ifdef WIN32
    if( path && isalpha( path[0] ) && path[1] == ':' && !path[2] )
        return 0;
    return mkdir( path );
#else
    return mkdir( path, permissions );
#endif
}

int
tr_mkdirp( const char * path_in,
           int          permissions )
{
    char *      path = tr_strdup( path_in );
    char *      p, * pp;
    struct stat sb;
    int         done;

    /* walk past the root */
    p = path;
    while( *p == TR_PATH_DELIMITER )
        ++p;

    pp = p;
    done = 0;
    while( ( p =
                strchr( pp, TR_PATH_DELIMITER ) ) || ( p = strchr( pp, '\0' ) ) )
    {
        if( !*p )
            done = 1;
        else
            *p = '\0';

        if( stat( path, &sb ) )
        {
            /* Folder doesn't exist yet */
            if( tr_mkdir( path, permissions ) )
            {
                const int err = errno;
                tr_err( _(
                           "Couldn't create \"%1$s\": %2$s" ), path,
                       tr_strerror( err ) );
                tr_free( path );
                errno = err;
                return -1;
            }
        }
        else if( ( sb.st_mode & S_IFMT ) != S_IFDIR )
        {
            /* Node exists but isn't a folder */
            char * buf = tr_strdup_printf( _( "File \"%s\" is in the way" ), path );
            tr_err( _( "Couldn't create \"%1$s\": %2$s" ), path_in, buf );
            tr_free( buf );
            tr_free( path );
            errno = ENOTDIR;
            return -1;
        }

        if( done )
            break;

        *p = TR_PATH_DELIMITER;
        p++;
        pp = p;
    }

    tr_free( path );
    return 0;
}

char*
tr_buildPath( const char *first_element, ... )
{
    size_t bufLen = 0;
    const char * element;
    char * buf;
    char * pch;
    va_list vl;

    /* pass 1: allocate enough space for the string */
    va_start( vl, first_element );
    element = first_element;
    while( element ) {
        bufLen += strlen( element ) + 1;
        element = va_arg( vl, const char* );
    }
    pch = buf = tr_new( char, bufLen );
    va_end( vl );

    /* pass 2: build the string piece by piece */
    va_start( vl, first_element );
    element = first_element;
    while( element ) {
        const size_t elementLen = strlen( element );
        memcpy( pch, element, elementLen );
        pch += elementLen;
        *pch++ = TR_PATH_DELIMITER;
        element = va_arg( vl, const char* );
    }
    va_end( vl );

    /* terminate the string. if nonempty, eat the unwanted trailing slash */
    if( pch != buf )
        --pch;
    *pch++ = '\0';

    /* sanity checks & return */
    assert( pch - buf == (off_t)bufLen );
    return buf;
}

/****
*****
****/

char*
evbuffer_free_to_str( struct evbuffer * buf )
{
    const size_t n = evbuffer_get_length( buf );
    char * ret = tr_new( char, n + 1 );
    evbuffer_copyout( buf, ret, n );
    evbuffer_free( buf );
    ret[n] = '\0';
    return ret;
}

char*
tr_strdup( const void * in )
{
    return tr_strndup( in, in ? (int)strlen((const char *)in) : 0 );
}

char*
tr_strndup( const void * in, int len )
{
    char * out = NULL;

    if( len < 0 )
    {
        out = tr_strdup( in );
    }
    else if( in )
    {
        out = tr_malloc( len + 1 );
        memcpy( out, in, len );
        out[len] = '\0';
    }

    return out;
}

const char*
tr_memmem( const char * haystack, size_t haystacklen,
           const char * needle, size_t needlelen )
{
#ifdef HAVE_MEMMEM
    return memmem( haystack, haystacklen, needle, needlelen );
#else
    size_t i;
    if( !needlelen )
        return haystack;
    if( needlelen > haystacklen || !haystack || !needle )
        return NULL;
    for( i=0; i<=haystacklen-needlelen; ++i )
        if( !memcmp( haystack+i, needle, needlelen ) )
            return haystack+i;
    return NULL;
#endif
}

char*
tr_strdup_printf( const char * fmt, ... )
{
    va_list ap;
    char * ret;
    size_t len;
    char statbuf[2048];

    va_start( ap, fmt );
    len = evutil_vsnprintf( statbuf, sizeof( statbuf ), fmt, ap );
    va_end( ap );
    if( len < sizeof( statbuf ) )
        ret = tr_strndup( statbuf, len );
    else {
        ret = tr_new( char, len + 1 );
        va_start( ap, fmt );
        evutil_vsnprintf( ret, len + 1, fmt, ap );
        va_end( ap );
    }

    return ret;
}

const char*
tr_strerror( int i )
{
    const char * ret = strerror( i );

    if( ret == NULL )
        ret = "Unknown Error";
    return ret;
}

int
tr_strcmp0( const char * str1, const char * str2 )
{
    if( str1 && str2 ) return strcmp( str1, str2 );
    if( str1 ) return 1;
    if( str2 ) return -1;
    return 0;
}

/****
*****
****/

/* https://bugs.launchpad.net/percona-patches/+bug/526863/+attachment/1160199/+files/solaris_10_fix.patch */
char*
tr_strsep( char ** str, const char * delims )
{
#ifdef HAVE_STRSEP
    return strsep( str, delims );
#else
    char *token;

    if (*str == NULL) {
        /* No more tokens */
        return NULL;
    }

    token = *str;
    while (**str != '\0') {
        if (strchr(delims, **str) != NULL) {
            **str = '\0';
            (*str)++;
            return token;
        }
        (*str)++;
    }

    /* There is not another token */
    *str = NULL;

    return token;
#endif
}

char*
tr_strstrip( char * str )
{
    if( str != NULL )
    {
        size_t pos;
        size_t len = strlen( str );

        while( len && isspace( str[len - 1] ) )
            --len;

        for( pos = 0; pos < len && isspace( str[pos] ); )
            ++pos;

        len -= pos;
        memmove( str, str + pos, len );
        str[len] = '\0';
    }

    return str;
}

bool
tr_str_has_suffix( const char *str, const char *suffix )
{
    size_t str_len;
    size_t suffix_len;

    if( !str )
        return false;
    if( !suffix )
        return true;

    str_len = strlen( str );
    suffix_len = strlen( suffix );
    if( str_len < suffix_len )
        return false;

    return !evutil_ascii_strncasecmp( str + str_len - suffix_len, suffix, suffix_len );
}

/****
*****
****/

uint64_t
tr_time_msec( void )
{
    struct timeval tv;

    gettimeofday( &tv, NULL );
    return (uint64_t) tv.tv_sec * 1000 + ( tv.tv_usec / 1000 );
}

void
tr_wait_msec( long int msec )
{
#ifdef WIN32
    Sleep( (DWORD)msec );
#else
    struct timespec ts;
    ts.tv_sec = msec / 1000;
    ts.tv_nsec = ( msec % 1000 ) * 1000000;
    nanosleep( &ts, NULL );
#endif
}

/***
****
***/

int
tr_snprintf( char * buf, size_t buflen, const char * fmt, ... )
{
    int     len;
    va_list args;

    va_start( args, fmt );
    len = evutil_vsnprintf( buf, buflen, fmt, args );
    va_end( args );
    return len;
}

/*
 * Copy src to string dst of size siz. At most siz-1 characters
 * will be copied. Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
size_t
tr_strlcpy( char * dst, const void * src, size_t siz )
{
#ifdef HAVE_STRLCPY
    return strlcpy( dst, src, siz );
#else
    char *      d = dst;
    const char *s = src;
    size_t      n = siz;

    assert( s );
    assert( d );

    /* Copy as many bytes as will fit */
    if( n != 0 )
    {
        while( --n != 0 )
        {
            if( ( *d++ = *s++ ) == '\0' )
                break;
        }
    }

    /* Not enough room in dst, add NUL and traverse rest of src */
    if( n == 0 )
    {
        if( siz != 0 )
            *d = '\0'; /* NUL-terminate dst */
        while( *s++ )
            ;
    }

    return s - (char*)src - 1;  /* count does not include NUL */
#endif
}

/***
****
***/

double
tr_getRatio( uint64_t numerator, uint64_t denominator )
{
    double ratio;

    if( denominator > 0 )
        ratio = numerator / (double)denominator;
    else if( numerator > 0 )
        ratio = TR_RATIO_INF;
    else
        ratio = TR_RATIO_NA;

    return ratio;
}

void
tr_sha1_to_hex( char * out, const uint8_t * sha1 )
{
    int i;
    static const char hex[] = "0123456789abcdef";

    for( i=0; i<20; ++i )
    {
        const unsigned int val = *sha1++;
        *out++ = hex[val >> 4];
        *out++ = hex[val & 0xf];
    }

    *out = '\0';
}

void
tr_hex_to_sha1( uint8_t * out, const char * in )
{
    int i;
    static const char hex[] = "0123456789abcdef";

    for( i=0; i<20; ++i )
    {
        const int hi = strchr( hex, tolower( *in++ ) ) - hex;
        const int lo = strchr( hex, tolower( *in++ ) ) - hex;
        *out++ = (uint8_t)( (hi<<4) | lo );
    }
}

/***
****
***/

static bool
isValidURLChars( const char * url, int url_len )
{
    const char * c;
    const char * end;
    static const char * rfc2396_valid_chars =
        "abcdefghijklmnopqrstuvwxyz" /* lowalpha */
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ" /* upalpha */
        "0123456789"                 /* digit */
        "-_.!~*'()"                  /* mark */
        ";/?:@&=+$,"                 /* reserved */
        "<>#%<\""                    /* delims */
        "{}|\\^[]`";                 /* unwise */

    if( url == NULL )
        return false;

    for( c=url, end=c+url_len; c && *c && c!=end; ++c )
        if( !strchr( rfc2396_valid_chars, *c ) )
            return false;

    return true;
}

/** @brief return true if the URL is a http or https or UDP one that Transmission understands */
bool
tr_urlIsValidTracker( const char * url )
{
    bool valid;
    const int len = url ? strlen(url) : 0;

    valid = isValidURLChars( url, len )
         && !tr_urlParse( url, len, NULL, NULL, NULL, NULL )
         && ( !memcmp(url,"http://",7) || !memcmp(url,"https://",8) || !memcmp(url,"udp://",6) );

    return valid;
}

/** @brief return true if the URL is a http or https or ftp or sftp one that Transmission understands */
bool
tr_urlIsValid( const char * url, int url_len )
{
    bool valid;
    if( ( url_len < 0 ) && ( url != NULL ) )
        url_len = strlen( url );

    valid = isValidURLChars( url, url_len )
         && !tr_urlParse( url, url_len, NULL, NULL, NULL, NULL )
         && ( !memcmp(url,"http://",7) || !memcmp(url,"https://",8) || !memcmp(url,"ftp://",6) || !memcmp(url,"sftp://",7) );

    return valid;
}

bool
tr_addressIsIP( const char * str )
{
    tr_address tmp;
    return tr_address_from_string( &tmp, str );
}

int
tr_urlParse( const char * url_in,
             int          len,
             char **      setme_protocol,
             char **      setme_host,
             int *        setme_port,
             char **      setme_path )
{
    int          err;
    int          port = 0;
    int          n;
    char *       tmp;
    char *       pch;
    size_t       host_len;
    size_t       protocol_len;
    const char * host = NULL;
    const char * protocol = NULL;
    const char * path = NULL;

    tmp = tr_strndup( url_in, len );
    if( ( pch = strstr( tmp, "://" ) ) )
    {
        *pch = '\0';
        protocol = tmp;
        protocol_len = pch - protocol;
        pch += 3;
/*fprintf( stderr, "protocol is [%s]... what's left is [%s]\n", protocol, pch);*/
        if( ( n = strcspn( pch, ":/" ) ) )
        {
            const int havePort = pch[n] == ':';
            host = pch;
            host_len = n;
            pch += n;
            if( pch && *pch )
                *pch++ = '\0';
/*fprintf( stderr, "host is [%s]... what's left is [%s]\n", host, pch );*/
            if( havePort )
            {
                char * end;
                port = strtol( pch, &end, 10 );
                pch = end;
/*fprintf( stderr, "port is [%d]... what's left is [%s]\n", port, pch );*/
            }
            path = pch;
/*fprintf( stderr, "path is [%s]\n", path );*/
        }
    }

    err = !host || !path || !protocol;

    if( !err && !port )
    {
        if( !strcmp( protocol, "udp" ) ) port = 80;
        else if( !strcmp( protocol, "ftp" ) ) port = 21;
        else if( !strcmp( protocol, "sftp" ) ) port = 22;
        else if( !strcmp( protocol, "http" ) ) port = 80;
        else if( !strcmp( protocol, "https" ) ) port = 443;
    }

    if( !err )
    {
        if( setme_protocol ) *setme_protocol = tr_strndup( protocol, protocol_len );

        if( setme_host ){ ( (char*)host )[-3] = ':'; *setme_host =
                              tr_strndup( host, host_len ); }
        if( setme_path ){ if( !*path ) *setme_path = tr_strdup( "/" );
                          else if( path[0] == '/' ) *setme_path = tr_strdup( path );
                          else { ( (char*)path )[-1] = '/'; *setme_path = tr_strdup( path - 1 ); } }
        if( setme_port ) *setme_port = port;
    }


    tr_free( tmp );
    return err;
}

#include <string.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

char *
tr_base64_encode( const void * input, int length, int * setme_len )
{
    int retlen = 0;
    char * ret = NULL;

    if( input != NULL )
    {
        BIO * b64;
        BIO * bmem;
        BUF_MEM * bptr;

        if( length < 1 )
            length = (int)strlen( input );

        bmem = BIO_new( BIO_s_mem( ) );
        b64 = BIO_new( BIO_f_base64( ) );
        BIO_set_flags( b64, BIO_FLAGS_BASE64_NO_NL );
        b64 = BIO_push( b64, bmem );
        BIO_write( b64, input, length );
        (void) BIO_flush( b64 );
        BIO_get_mem_ptr( b64, &bptr );
        ret = tr_strndup( bptr->data, bptr->length );
        retlen = bptr->length;
        BIO_free_all( b64 );
    }

    if( setme_len )
        *setme_len = retlen;

    return ret;
}

char *
tr_base64_decode( const void * input,
                  int          length,
                  int *        setme_len )
{
    char * ret;
    BIO *  b64;
    BIO *  bmem;
    int    retlen;

    if( length < 1 )
        length = strlen( input );

    ret = tr_new0( char, length );
    b64 = BIO_new( BIO_f_base64( ) );
    bmem = BIO_new_mem_buf( (unsigned char*)input, length );
    bmem = BIO_push( b64, bmem );
    retlen = BIO_read( bmem, ret, length );
    if( !retlen )
    {
        /* try again, but with the BIO_FLAGS_BASE64_NO_NL flag */
        BIO_free_all( bmem );
        b64 = BIO_new( BIO_f_base64( ) );
        BIO_set_flags( b64, BIO_FLAGS_BASE64_NO_NL );
        bmem = BIO_new_mem_buf( (unsigned char*)input, length );
        bmem = BIO_push( b64, bmem );
        retlen = BIO_read( bmem, ret, length );
    }

    if( setme_len )
        *setme_len = retlen;

    BIO_free_all( bmem );
    return ret;
}

/***
****
***/

void
tr_removeElementFromArray( void         * array,
                           unsigned int   index_to_remove,
                           size_t         sizeof_element,
                           size_t         nmemb )
{
    char * a = array;

    memmove( a + sizeof_element * index_to_remove,
             a + sizeof_element * ( index_to_remove  + 1 ),
             sizeof_element * ( --nmemb - index_to_remove ) );
}

int
tr_lowerBound( const void * key,
               const void * base,
               size_t       nmemb,
               size_t       size,
               int       (* compar)(const void* key, const void* arrayMember),
               bool       * exact_match )
{
    size_t first = 0;
    const char * cbase = base;
    bool exact = false;

    while( nmemb != 0 )
    {
        const size_t half = nmemb / 2;
        const size_t middle = first + half;
        const int c = compar( key, cbase + size*middle );

        if( c <= 0 ) {
            if( c == 0 )
                exact = true;
            nmemb = half;
        } else {
            first = middle + 1;
            nmemb = nmemb - half - 1;
        }
    }

    *exact_match = exact;

    return first;
}

/***
****
***/

static char*
strip_non_utf8( const char * in, size_t inlen )
{
    const char * end;
    const char zero = '\0';
    struct evbuffer * buf = evbuffer_new( );

    while( !tr_utf8_validate( in, inlen, &end ) )
    {
        const int good_len = end - in;

        evbuffer_add( buf, in, good_len );
        inlen -= ( good_len + 1 );
        in += ( good_len + 1 );
        evbuffer_add( buf, "?", 1 );
    }

    evbuffer_add( buf, in, inlen );
    evbuffer_add( buf, &zero, 1 );
    return evbuffer_free_to_str( buf );
}

static char*
to_utf8( const char * in, size_t inlen )
{
    char * ret = NULL;

#ifdef HAVE_ICONV_OPEN
    int i;
    const char * encodings[] = { "CURRENT", "ISO-8859-15" };
    const int encoding_count = sizeof(encodings) / sizeof(encodings[1]);
    const size_t buflen = inlen*4 + 10;
    char * out = tr_new( char, buflen );

    for( i=0; !ret && i<encoding_count; ++i )
    {
        char * inbuf = (char*) in;
        char * outbuf = out;
        size_t inbytesleft = inlen;
        size_t outbytesleft = buflen;
        const char * test_encoding = encodings[i];

        iconv_t cd = iconv_open( "UTF-8", test_encoding );
        if( cd != (iconv_t)-1 ) {
            if( iconv( cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft ) != (size_t)-1 )
                ret = tr_strndup( out, buflen-outbytesleft );
            iconv_close( cd );
        }
    }

    tr_free( out );
#endif

    if( ret == NULL )
        ret = strip_non_utf8( in, inlen );

    return ret;
}

char*
tr_utf8clean( const char * str, int max_len )
{
    char * ret;
    const char * end;

    if( max_len < 0 )
        max_len = (int) strlen( str );

    if( tr_utf8_validate( str, max_len, &end  ) )
        ret = tr_strndup( str, max_len );
    else
        ret = to_utf8( str, max_len );

    assert( tr_utf8_validate( ret, -1, NULL ) );
    return ret;
}

/***
****
***/

struct number_range
{
    int low;
    int high;
};

/**
 * This should be a single number (ex. "6") or a range (ex. "6-9").
 * Anything else is an error and will return failure.
 */
static bool
parseNumberSection( const char * str, int len, struct number_range * setme )
{
    long a, b;
    bool success;
    char * end;
    const int error = errno;
    char * tmp = tr_strndup( str, len );

    errno = 0;
    a = b = strtol( tmp, &end, 10 );
    if( errno || ( end == tmp ) ) {
        success = false;
    } else if( *end != '-' ) {
        success = true;
    } else {
        const char * pch = end + 1;
        b = strtol( pch, &end, 10 );
        if( errno || ( pch == end ) )
            success = false;
        else if( *end ) /* trailing data */
            success = false;
        else
            success = true;
    }
    tr_free( tmp );

    setme->low = MIN( a, b );
    setme->high = MAX( a, b );

    errno = error;
    return success;
}

int
compareInt( const void * va, const void * vb )
{
    const int a = *(const int *)va;
    const int b = *(const int *)vb;
    return a - b;
}

/**
 * Given a string like "1-4" or "1-4,6,9,14-51", this allocates and returns an
 * array of setmeCount ints of all the values in the array.
 * For example, "5-8" will return [ 5, 6, 7, 8 ] and setmeCount will be 4.
 * It's the caller's responsibility to call tr_free() on the returned array.
 * If a fragment of the string can't be parsed, NULL is returned.
 */
int*
tr_parseNumberRange( const char * str_in, int len, int * setmeCount )
{
    int n = 0;
    int * uniq = NULL;
    char * str = tr_strndup( str_in, len );
    const char * walk;
    tr_list * ranges = NULL;
    bool success = true;

    walk = str;
    while( walk && *walk && success ) {
        struct number_range range;
        const char * pch = strchr( walk, ',' );
        if( pch ) {
            success = parseNumberSection( walk, pch-walk, &range );
            walk = pch + 1;
        } else {
            success = parseNumberSection( walk, strlen( walk ), &range );
            walk += strlen( walk );
        }
        if( success )
            tr_list_append( &ranges, tr_memdup( &range, sizeof( struct number_range ) ) );
    }

    if( !success )
    {
        *setmeCount = 0;
        uniq = NULL;
    }
    else
    {
        int i;
        int n2;
        tr_list * l;
        int * sorted = NULL;

        /* build a sorted number array */
        n = n2 = 0;
        for( l=ranges; l!=NULL; l=l->next ) {
            const struct number_range * r = l->data;
            n += r->high + 1 - r->low;
        }
        sorted = tr_new( int, n );
        for( l=ranges; l!=NULL; l=l->next ) {
            const struct number_range * r = l->data;
            int i;
            for( i=r->low; i<=r->high; ++i )
                sorted[n2++] = i;
        }
        qsort( sorted, n, sizeof( int ), compareInt );
        assert( n == n2 );

        /* remove duplicates */
        uniq = tr_new( int, n );
        for( i=n=0; i<n2; ++i )
            if( !n || uniq[n-1] != sorted[i] )
                uniq[n++] = sorted[i];

        tr_free( sorted );
    }

    /* cleanup */
    tr_list_free( &ranges, tr_free );
    tr_free( str );

    /* return the result */
    *setmeCount = n;
    return uniq;
}

/***
****
***/

double
tr_truncd( double x, int precision )
{
    char * pt;
    char buf[128];
    const int max_precision = (int) log10( 1.0 / DBL_EPSILON ) - 1;
    tr_snprintf( buf, sizeof( buf ), "%.*f", max_precision, x );
    if(( pt = strstr( buf, localeconv()->decimal_point )))
        pt[precision ? precision+1 : 0] = '\0';
    return atof(buf);
}

/* return a truncated double as a string */
static char*
tr_strtruncd( char * buf, double x, int precision, size_t buflen )
{
    tr_snprintf( buf, buflen, "%.*f", precision, tr_truncd( x, precision ) );
    return buf;
}

char*
tr_strpercent( char * buf, double x, size_t buflen )
{
    if( x < 10.0 )
        tr_strtruncd( buf, x, 2, buflen );
    else if( x < 100.0 )
        tr_strtruncd( buf, x, 1, buflen );
    else
        tr_strtruncd( buf, x, 0, buflen );
    return buf;
}

char*
tr_strratio( char * buf, size_t buflen, double ratio, const char * infinity )
{
    if( (int)ratio == TR_RATIO_NA )
        tr_strlcpy( buf, _( "None" ), buflen );
    else if( (int)ratio == TR_RATIO_INF )
        tr_strlcpy( buf, infinity, buflen );
    else
        tr_strpercent( buf, ratio, buflen );
    return buf;
}

/***
****
***/

int
tr_moveFile( const char * oldpath, const char * newpath, bool * renamed )
{
    int in;
    int out;
    char * buf;
    struct stat st;
    off_t bytesLeft;
    const size_t buflen = 1024 * 128; /* 128 KiB buffer */

    /* make sure the old file exists */
    if( stat( oldpath, &st ) ) {
        const int err = errno;
        errno = err;
        return -1;
    }
    if( !S_ISREG( st.st_mode ) ) {
        errno = ENOENT;
        return -1;
    }
    bytesLeft = st.st_size;

    /* make sure the target directory exists */
    {
        char * newdir = tr_dirname( newpath );
        int i = tr_mkdirp( newdir, 0777 );
        tr_free( newdir );
        if( i )
            return i;
    }

    /* they might be on the same filesystem... */
    {
        const int i = rename( oldpath, newpath );
        if( renamed != NULL )
            *renamed = i == 0;
        if( !i )
            return 0;
    }

    /* copy the file */
    in = tr_open_file_for_scanning( oldpath );
    out = tr_open_file_for_writing( newpath );
    buf = tr_valloc( buflen );
    while( bytesLeft > 0 )
    {
        ssize_t bytesWritten;
        const off_t bytesThisPass = MIN( bytesLeft, (off_t)buflen );
        const int numRead = read( in, buf, bytesThisPass );
        if( numRead < 0 )
            break;
        bytesWritten = write( out, buf, numRead );
        if( bytesWritten < 0 )
            break;
        bytesLeft -= bytesWritten;
    }

    /* cleanup */
    tr_free( buf );
    tr_close_file( out );
    tr_close_file( in );
    if( bytesLeft != 0 )
        return -1;

    unlink( oldpath );
    return 0;
}

bool
tr_is_same_file( const char * filename1, const char * filename2 )
{
    struct stat sb1, sb2;

    return !stat( filename1, &sb1 )
        && !stat( filename2, &sb2 )
        && ( sb1.st_dev == sb2.st_dev )
        && ( sb1.st_ino == sb2.st_ino );
}

/***
****
***/

void*
tr_valloc( size_t bufLen )
{
    size_t allocLen;
    void * buf = NULL;
    static size_t pageSize = 0;

    if( !pageSize ) {
#ifdef HAVE_GETPAGESIZE
        pageSize = (size_t) getpagesize();
#else /* guess */
        pageSize = 4096;
#endif
    }

    allocLen = pageSize;
    while( allocLen < bufLen )
        allocLen += pageSize;

#ifdef HAVE_POSIX_MEMALIGN
    if( !buf )
        if( posix_memalign( &buf, pageSize, allocLen ) )
            buf = NULL; /* just retry with valloc/malloc */
#endif
#ifdef HAVE_VALLOC
    if( !buf )
        buf = valloc( allocLen );
#endif
    if( !buf )
        buf = tr_malloc( allocLen );

    return buf;
}

char *
tr_realpath( const char * path, char * resolved_path )
{
#ifdef WIN32
    /* From a message to the Mingw-msys list, Jun 2, 2005 by Mark Junker. */
    if( GetFullPathNameA( path, TR_PATH_MAX, resolved_path, NULL ) == 0 )
        return NULL;
    return resolved_path;
#else
    return realpath( path, resolved_path );
#endif
}

/***
****
***/

uint64_t
tr_htonll( uint64_t x )
{
#ifdef HAVE_HTONLL
    return htonll( x );
#else
    /* fallback code by Runner and Juan Carlos Cobas at
     * http://www.codeproject.com/KB/cpp/endianness.aspx */
    return (((uint64_t)(htonl((int)((x << 32) >> 32))) << 32) |
                     (unsigned int)htonl(((int)(x >> 32))));
#endif
}

uint64_t
tr_ntohll( uint64_t x )
{
#ifdef HAVE_NTOHLL
    return ntohll( x );
#else
    /* fallback code by Runner and Juan Carlos Cobas at
     * http://www.codeproject.com/KB/cpp/endianness.aspx */
    return (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) |
                     (unsigned int)ntohl(((int)(x >> 32))));
#endif
}

/***
****
****
****
***/

struct formatter_unit
{
    char * name;
    int64_t value;
};

struct formatter_units
{
    struct formatter_unit units[4];
};

enum { TR_FMT_KB, TR_FMT_MB, TR_FMT_GB, TR_FMT_TB };

static void
formatter_init( struct formatter_units * units,
                unsigned int kilo,
                const char * kb, const char * mb,
                const char * gb, const char * tb )
{
    uint64_t value = kilo;
    units->units[TR_FMT_KB].name = tr_strdup( kb );
    units->units[TR_FMT_KB].value = value;

    value *= kilo;
    units->units[TR_FMT_MB].name = tr_strdup( mb );
    units->units[TR_FMT_MB].value = value;

    value *= kilo;
    units->units[TR_FMT_GB].name = tr_strdup( gb );
    units->units[TR_FMT_GB].value = value;

    value *= kilo;
    units->units[TR_FMT_TB].name = tr_strdup( tb );
    units->units[TR_FMT_TB].value = value;
}

static char*
formatter_get_size_str( const struct formatter_units * u,
                        char * buf, int64_t bytes, size_t buflen )
{
    int precision;
    double value;
    const char * units;
    const struct formatter_unit * unit;

         if( bytes < u->units[1].value ) unit = &u->units[0];
    else if( bytes < u->units[2].value ) unit = &u->units[1];
    else if( bytes < u->units[3].value ) unit = &u->units[2];
    else                                 unit = &u->units[3];

    value = (double)bytes / unit->value;
    units = unit->name;
    if( unit->value == 1 )
        precision = 0;
    else if( value < 100 )
        precision = 2;
    else
        precision = 1;
    tr_snprintf( buf, buflen, "%.*f %s", precision, value, units );
    return buf;
}

static struct formatter_units size_units;

void
tr_formatter_size_init( unsigned int kilo,
                        const char * kb, const char * mb,
                        const char * gb, const char * tb )
{
    formatter_init( &size_units, kilo, kb, mb, gb, tb );
}

char*
tr_formatter_size_B( char * buf, int64_t bytes, size_t buflen )
{
    return formatter_get_size_str( &size_units, buf, bytes, buflen );
}

static struct formatter_units speed_units;

unsigned int tr_speed_K = 0u;

void
tr_formatter_speed_init( unsigned int kilo,
                         const char * kb, const char * mb,
                         const char * gb, const char * tb )
{
    tr_speed_K = kilo;
    formatter_init( &speed_units, kilo, kb, mb, gb, tb );
}

char*
tr_formatter_speed_KBps( char * buf, double KBps, size_t buflen )
{
    const double K = speed_units.units[TR_FMT_KB].value;
    double speed = KBps;

    if( speed <= 999.95 ) /* 0.0 KB to 999.9 KB */
        tr_snprintf( buf, buflen, "%d %s", (int)speed, speed_units.units[TR_FMT_KB].name );
    else {
        speed /= K;
        if( speed <= 99.995 ) /* 0.98 MB to 99.99 MB */
            tr_snprintf( buf, buflen, "%.2f %s", speed, speed_units.units[TR_FMT_MB].name );
        else if (speed <= 999.95) /* 100.0 MB to 999.9 MB */
            tr_snprintf( buf, buflen, "%.1f %s", speed, speed_units.units[TR_FMT_MB].name );
        else {
            speed /= K;
            tr_snprintf( buf, buflen, "%.1f %s", speed, speed_units.units[TR_FMT_GB].name );
        }
    }

    return buf;
}

static struct formatter_units mem_units;

unsigned int tr_mem_K = 0u;

void
tr_formatter_mem_init( unsigned int kilo,
                       const char * kb, const char * mb,
                       const char * gb, const char * tb )
{
    tr_mem_K = kilo;
    formatter_init( &mem_units, kilo, kb, mb, gb, tb );
}

char*
tr_formatter_mem_B( char * buf, int64_t bytes_per_second, size_t buflen )
{
    return formatter_get_size_str( &mem_units, buf, bytes_per_second, buflen );
}

void
tr_formatter_get_units( tr_benc * d )
{
    int i;
    tr_benc * l;

    tr_bencDictReserve( d, 6 );

    tr_bencDictAddInt( d, "memory-bytes", mem_units.units[TR_FMT_KB].value );
    l = tr_bencDictAddList( d, "memory-units", 4 );
    for( i=0; i<4; i++ ) tr_bencListAddStr( l, mem_units.units[i].name );

    tr_bencDictAddInt( d, "size-bytes",   size_units.units[TR_FMT_KB].value );
    l = tr_bencDictAddList( d, "size-units", 4 );
    for( i=0; i<4; i++ ) tr_bencListAddStr( l, size_units.units[i].name );

    tr_bencDictAddInt( d, "speed-bytes",  speed_units.units[TR_FMT_KB].value );
    l = tr_bencDictAddList( d, "speed-units", 4 );
    for( i=0; i<4; i++ ) tr_bencListAddStr( l, speed_units.units[i].name );
}

