/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_http.c
 * @brief The standard layer's HTTP: get and post with the answer as text.
 */
#include "kode.h"

#include <string.h>

#include "kdrv_wifi.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_entry.h"
#include "kode_http.h"
#include "kode_net.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_task.h"

/** @brief The scheme of a plain address. */
#define KODE_HTTP_SCHEME "http://"

/** @brief The scheme of a verified address. */
#define KODE_HTTP_SCHEME_TLS "https://"

/** @brief Content-Type of a body that starts a JSON object or array. */
#define KODE_HTTP_CTYPE_JSON "application/json"

/** @brief Content-Type of any other body. */
#define KODE_HTTP_CTYPE_TEXT "text/plain; charset=utf-8"

/** @brief What the error text shows for a URL that is NULL. */
#define KODE_HTTP_NULL_SHOWN "(null)"

_Static_assert(KODE_HTTP_BODY_MAX == (64U * 1024U),
               "the error text of an answer over the ceiling says 64 KiB");

/** @brief What a task keeps for its HTTP calls. */
struct kode_http_state {
    char    *body;    /**< The last answer's text, KODE_HTTP_BODY_MAX + 1 bytes; NULL until the first request. */
    uint16_t status;  /**< The last request's status; 0 for none.                                               */
};

/** @brief What a failed call returns. */
static const char s_no_text[] = "";

/** @brief The lock every public HTTP call holds; made on the first call. */
static khal_mutex_t s_http_lock;

/** @brief Make the HTTP lock. @return true when it exists. */
static bool http_lock_make(void)
{
    s_http_lock = khal_mutex_create();
    return NULL != s_http_lock;
}

/**
 * @brief kode_enter(), then the HTTP turn; http_leave() gives it back.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool http_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &http_lock_make);
        if ( ok )
        {
            khal_mutex_lock(s_http_lock);
        }
        else
        {
            kode_err_set("%s: no memory for the HTTP lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call http_enter() let in. */
static void http_leave(void)
{
    khal_mutex_unlock(s_http_lock);
}

void kode_http_state_free(struct kode_http_state *s)
{
    if ( NULL != s )
    {
        khal_free(s->body);
        khal_free(s);
    }
}

/**
 * @brief The calling task's HTTP state, taken from PSRAM on first use.
 * @return The state; NULL if PSRAM has none to give.
 */
static struct kode_http_state *http_state_ensure(void)
{
    kode_task_t *const      task = kode_task_ensure();
    struct kode_http_state *s    = NULL;

    if ( NULL != task )
    {
        if ( NULL == task->http )
        {
            /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a kode_http_state */
            s = (struct kode_http_state *)khal_alloc_psram(sizeof(void *), sizeof(*s));
            if ( NULL != s )
            {
                (void)memset(s, 0, sizeof(*s));
                task->http = s;
            }
        }
        s = task->http;
    }
    return s;
}

/**
 * @brief Whether @p url is an address a request may go to.
 * @param url What the caller typed; may be NULL.
 * @return true for http:// or https:// and under KODE_HTTP_URL_MAX bytes.
 */
static bool url_ok(const char *url)
{
    bool ok = (NULL != url);

    if ( ok )
    {
        uint32_t n = 0U;

        while ( (KODE_HTTP_URL_MAX > n) && ('\0' != url[n]) )
        {
            n++;
        }
        ok = (KODE_HTTP_URL_MAX > n) && (kode_has_prefix(url, KODE_HTTP_SCHEME) || kode_has_prefix(url, KODE_HTTP_SCHEME_TLS));
    }
    return ok;
}

/**
 * @brief The Content-Type for @p body.
 * @param body The text to send.
 * @return JSON when its first character that is not white space is '{' or '['; text otherwise.
 */
static const char *ctype_for(const char *body)
{
    const char *at    = body;
    const char *ctype = KODE_HTTP_CTYPE_TEXT;

    while ( (' ' == *at) || ('\t' == *at) || ('\r' == *at) || ('\n' == *at) )
    {
        at++;
    }
    if ( ('{' == *at) || ('[' == *at) )
    {
        ctype = KODE_HTTP_CTYPE_JSON;
    }
    return ctype;
}

/**
 * @brief The body block, taken from PSRAM on the task's first request and kept.
 * @param s The task's state.
 * @return true when the block exists; false with the error set.
 */
static bool body_ensure(struct kode_http_state *s)
{
    if ( NULL == s->body )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block holds text */
        s->body = (char *)khal_alloc_psram(sizeof(void *), KODE_HTTP_BODY_MAX + 1U);
    }
    if ( NULL == s->body )
    {
        kode_err_set("no memory for the answer");
    }
    return NULL != s->body;
}

/**
 * @brief Keep why the exchange failed as the task's error.
 * @param url The address asked.
 * @param rc  The facade's answer.
 */
static void http_fail(const char *url, kode_status_t rc)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("this firmware has no network");
        break;
    case KODE_TIMEOUT:
        kode_err_set("%s: no answer in time", url);
        break;
    default:
        kode_err_set("%s: could not connect", url);
        break;
    }
}

/**
 * @brief Can @p headers go out as written? Sets the error when not.
 * @param headers The app's own, ended by a NULL name; NULL for none.
 * @param ranged  They go with a range.
 * @return true when every one of them goes out.
 */
static bool headers_ok(const kode_http_header_t *headers, bool ranged)
{
    uint32_t   bad = 0U;
    bool const ok  = kode_sys_http_headers_ok(headers, ranged, &bad);

    if ( ok )
    {
        /* every one goes out */
    }
    else if ( KODE_HTTP_HEADERS_MAX <= bad )
    {
        kode_err_set("more than %u headers", (unsigned)KODE_HTTP_HEADERS_MAX);
    }
    else
    {
        kode_err_set("header %u cannot be sent as written", (unsigned)bad);
    }
    return ok;
}

/**
 * @brief The exchange itself, under the HTTP lock.
 * @param s       The task's state; its body block exists.
 * @param url     The address.
 * @param body    The text to post; NULL for a GET.
 * @param headers The app's own; NULL for none.
 * @return The answer's text; "" with the error set when there is none.
 */
static const char *http_text_exchange(struct kode_http_state *s, const char *url,
                                      const char *body, const kode_http_header_t *headers)
{
    kode_sys_http_result_t res  = { .content_length = -1, .range_total = -1 };
    const char            *text = s_no_text;
    kode_status_t          rc;

    if ( NULL == body )
    {
        rc = kode_block_http_get_buffer(url, s->body, KODE_HTTP_BODY_MAX, headers, &res);
    }
    else
    {
        rc = kode_block_http_post_buffer(url, body, (uint32_t)strlen(body), ctype_for(body),
                                         s->body, KODE_HTTP_BODY_MAX, headers, &res);
    }
    if ( KODE_OK != rc )
    {
        http_fail(url, rc);
    }
    else if ( res.truncated )
    {
        s->status = res.status;
        kode_err_set("%s: the answer is over 64 KiB", url);
    }
    else
    {
        s->status         = res.status;
        s->body[res.kept] = '\0';
        text              = s->body;
    }
    return text;
}

/**
 * @brief One request, from the checks to the answer.
 * @param name    The public name of the call.
 * @param url     The address.
 * @param body    The text to post; NULL for a GET.
 * @param post    true for a POST, which refuses a NULL @p body.
 * @param headers The app's own; NULL for none.
 * @return The answer's text; "" with the error set when there is none.
 */
static const char *http_request(const char *name, const char *url, const char *body, bool post,
                                const kode_http_header_t *headers)
{
    const char *text = s_no_text;

    if ( http_enter(name) )
    {
        struct kode_http_state *const s = http_state_ensure();

        if ( NULL == s )
        {
            kode_err_set("%s: no memory for the answer", name);
        }
        else
        {
            s->status = 0U;
            if ( NULL != s->body )
            {
                s->body[0] = '\0';
            }
            if ( !url_ok(url) )
            {
                const char *shown = KODE_HTTP_NULL_SHOWN;

                if ( NULL != url )
                {
                    shown = url;
                }
                kode_err_set("bad URL '%s'", shown);
            }
            else if ( post && (NULL == body) )
            {
                kode_err_set("no body to post");
            }
            else if ( !headers_ok(headers, false) )
            {
                /* the error is set */
            }
            else if ( kode_net_ready() && body_ensure(s) )
            {
                text = http_text_exchange(s, url, body, headers);
            }
            else
            {
                /* the error is set */
            }
        }
        http_leave();
    }
    return text;
}

const char *kode_http_get(const char *url, const kode_http_header_t *headers)
{
    return http_request("kode_http_get", url, NULL, false, headers);
}

const char *kode_http_post(const char *url, const char *body, const kode_http_header_t *headers)
{
    return http_request("kode_http_post", url, body, true, headers);
}

/**
 * @brief The length of a whole resource as the caller's type holds it.
 * @param total The server's word; negative for none.
 * @return 0 for none; KODE_HTTP_OFF_MAX for one longer than the type holds.
 */
static kode_http_off_t total_of(int64_t total)
{
    kode_http_off_t out = 0U;

    if ( 0 < total )
    {
        if ( (uint64_t)total > (uint64_t)KODE_HTTP_OFF_MAX )
        {
            out = KODE_HTTP_OFF_MAX;
        }
        else
        {
            out = (kode_http_off_t)total;
        }
    }
    return out;
}

/**
 * @brief The ranged exchange itself, under the HTTP lock.
 * @param s      The task's state.
 * @param url    The address.
 * @param offset The first byte wanted.
 * @param buf    Where the bytes go.
 * @param cap    Its room.
 * @param got    Out, may be NULL: the bytes written.
 * @param total  Out, may be NULL: the length of the whole, 0 for none.
 * @param headers The app's own; NULL for none.
 * @return true when the server answered; false with the error set.
 */
static bool range_exchange(struct kode_http_state *s, const char *url, kode_http_off_t offset,
                           void *buf, uint32_t cap, uint32_t *got, kode_http_off_t *total,
                           const kode_http_header_t *headers)
{
    kode_sys_http_result_t res = { .content_length = -1, .range_total = -1 };
    kode_status_t const    rc  = kode_block_http_get_range(url, (uint64_t)offset, buf, cap,
                                                           headers, &res);

    if ( KODE_OK != rc )
    {
        http_fail(url, rc);
    }
    else
    {
        s->status = res.status;
        if ( NULL != got )
        {
            *got = res.kept;
        }
        if ( NULL != total )
        {
            *total = total_of(res.range_total);
        }
    }
    return KODE_OK == rc;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_http_get_range(const char *url, kode_http_off_t offset, void *buf, uint32_t cap,
                         uint32_t *got, kode_http_off_t *total,
                         const kode_http_header_t *headers)
{
    bool answered = false;

    if ( NULL != got )
    {
        *got = 0U;
    }
    if ( NULL != total )
    {
        *total = 0U;
    }
    if ( http_enter("kode_http_get_range") )
    {
        struct kode_http_state *const s = http_state_ensure();

        if ( NULL == s )
        {
            kode_err_set("kode_http_get_range: no memory for the answer");
        }
        else
        {
            s->status = 0U;
            if ( !url_ok(url) )
            {
                const char *shown = KODE_HTTP_NULL_SHOWN;

                if ( NULL != url )
                {
                    shown = url;
                }
                kode_err_set("bad URL '%s'", shown);
            }
            else if ( (NULL == buf) || (0U == cap) )
            {
                kode_err_set("no buffer for the range");
            }
            else if ( offset > (KODE_HTTP_OFF_MAX - ((kode_http_off_t)cap - 1U)) )
            {
                kode_err_set("the range runs past the last offset");
            }
            else if ( !headers_ok(headers, true) )
            {
                /* the error is set */
            }
            else if ( kode_net_ready() )
            {
                answered = range_exchange(s, url, offset, buf, cap, got, total, headers);
            }
            else
            {
                /* the error is set */
            }
        }
        http_leave();
    }
    return answered;
}

uint16_t kode_http_status(void)
{
    const kode_task_t *const task   = kode_task_get();
    uint16_t                 status = 0U;

    if ( (NULL != task) && (NULL != task->http) )
    {
        status = task->http->status;
    }
    return status;
}
