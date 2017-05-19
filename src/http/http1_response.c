#include "http1_response.h"
#include "spnlock.inc"

#include <string.h>
/**
The padding for the status line (62 + 2 for the extra \r\n after the headers).
*/
#define H1P_HEADER_START 80
#define H1P_OVERFLOW_PADDING 128

/* *****************************************************************************
Response object & Initialization
***************************************************************************** */

typedef struct {
  http_response_s response;
  size_t buffer_start;
  size_t buffer_end;
  spn_lock_i lock;
  uint8_t use_count;
  char buffer[HTTP1_MAX_HEADER_SIZE];
} http1_response_s;

static struct {
  spn_lock_i lock;
  uint8_t init;
  http1_response_s *next;
  http1_response_s pool_mem[HTTP1_POOL_SIZE];
} http1_response_pool = {.lock = SPN_LOCK_INIT, .init = 0};

static inline void http1_response_clear(http1_response_s *rs,
                                        http_request_s *request) {
  rs->response = (http_response_s){
      .http_version = HTTP_V1, .request = request, .fd = request->fd,
  };
  rs->buffer_end = rs->buffer_start = H1P_HEADER_START;
  rs->use_count = 1;
  rs->lock = SPN_LOCK_INIT;
}
/** Creates / allocates a protocol version's response object. */
http_response_s *http1_response_create(http_request_s *request) {
  http1_response_s *rs;
  spn_lock(&http1_response_pool.lock);
  if (!http1_response_pool.next)
    goto use_malloc;
  rs = http1_response_pool.next;
  http1_response_pool.next = (void *)rs->response.request;
  spn_unlock(&http1_response_pool.lock);
  http1_response_clear(rs, request);
  return (http_response_s *)rs;
use_malloc:
  if (http1_response_pool.init == 0)
    goto initialize;
  spn_unlock(&http1_response_pool.lock);
  rs = malloc(sizeof(*rs));
  http1_response_clear(rs, request);
  return (http_response_s *)rs;
initialize:
  for (size_t i = 1; i < (HTTP1_POOL_SIZE - 1); i++) {
    http1_response_pool.pool_mem[i].response.request =
        (void *)(http1_response_pool.pool_mem + (i + 1));
  }
  http1_response_pool.next = http1_response_pool.pool_mem + 1;
  spn_unlock(&http1_response_pool.lock);
  http1_response_clear(http1_response_pool.pool_mem, request);
  return (http_response_s *)http1_response_pool.pool_mem;
}

static void http1_response_deffered_destroy(void *rs_, void *ignr) {
  (void)(ignr);
  http1_response_s *rs = rs_;
  spn_lock(&rs->lock);
  rs->use_count--;
  if (rs->use_count) {
    spn_unlock(&rs->lock);
    return;
  }

  if (rs->response.request_dupped)
    http_request_destroy(rs->response.request);

  if ((uintptr_t)rs < (uintptr_t)http1_response_pool.pool_mem ||
      (uintptr_t)rs >=
          (uintptr_t)(http1_response_pool.pool_mem + HTTP1_POOL_SIZE))
    goto use_free;
  spn_lock(&http1_response_pool.lock);
  rs->response.request = (void *)http1_response_pool.next;
  http1_response_pool.next = (void *)rs;
  return;
use_free:
  free(rs);
  return;
}

/** Destroys the response object. No data is sent.*/
void http1_response_destroy(http_response_s *rs) {
  defer(http1_response_deffered_destroy, rs, NULL);
}

/* *****************************************************************************
Writing and Finishing Helpers + `http1_response_finish`
***************************************************************************** */

static int h1p_protected_copy(http1_response_s *rs, void *buff, size_t len) {
  if (len + rs->buffer_end >= HTTP1_MAX_HEADER_SIZE - H1P_OVERFLOW_PADDING)
    return -1;
  memcpy(rs->buffer + rs->buffer_end, buff, len);
  rs->buffer_end += len;
}

static void http1_response_finalize_headers(http1_response_s *rs) {
  if (rs->response.headers_sent)
    return;
  rs->response.headers_sent = 1;
  const char *status = http_response_status_str(rs->response.status);
  if (!status) {
    rs->response.status = 500;
    status = http_response_status_str(rs->response.status);
  }

  /* write the content length header, unless forced not to (<0) */
  if (rs->response.content_length_written == 0 &&
      !(rs->response.content_length < 0) && rs->response.status >= 200 &&
      rs->response.status != 204 && rs->response.status != 304) {
    h1p_protected_copy(rs, "Content-Length:", 15);
    rs->buffer_end +=
        http_ul2a(rs->buffer + rs->buffer_end, rs->response.content_length);
    /* write the header seperator (`\r\n`) */
    rs->buffer[rs->buffer_end++] = '\r';
    rs->buffer[rs->buffer_end++] = '\n';
  }
  /* write the date, if missing */
  if (!rs->response.date_written) {
    if (rs->response.date < rs->response.last_modified)
      rs->response.date = rs->response.last_modified;
    struct tm t;
    /* date header */
    http_gmtime(&rs->response.date, &t);
    h1p_protected_copy(rs, "Date:", 5);
    rs->buffer_end += http_date2str(rs->buffer + rs->buffer_end, &t);
    rs->buffer[rs->buffer_end++] = '\r';
    rs->buffer[rs->buffer_end++] = '\n';
    /* last-modified header */
    http_gmtime(&rs->response.last_modified, &t);
    h1p_protected_copy(rs, "Last-Modified:", 14);
    rs->buffer_end += http_date2str(rs->buffer + rs->buffer_end, &t);
    rs->buffer[rs->buffer_end++] = '\r';
    rs->buffer[rs->buffer_end++] = '\n';
  }
  /* write the keep-alive (connection) header, if missing */
  if (!rs->response.connection_written) {
    if (rs->response.should_close) {
      h1p_protected_copy(rs, "Connection:close\r\n", 18);
    } else {
      h1p_protected_copy(rs,
                         "Connection:keep-alive\r\n"
                         "Keep-Alive:timeout=2\r\n",
                         45);
    }
  }
  /* write the headers completion marker (empty line - `\r\n`) */
  rs->buffer[rs->buffer_end++] = '\r';
  rs->buffer[rs->buffer_end++] = '\n';

  /* write the status string is "HTTP/1.1 xxx <...>\r\n" length == 15 +
   * strlen(status) */

  size_t tmp = strlen(status);
  int start = H1P_HEADER_START - (15 + tmp);
  memcpy(rs->buffer + start, "HTTP/1.1 ### ", 13);
  memcpy(rs->buffer + start + 13, status, tmp);
  rs->buffer[H1P_HEADER_START - 1] = '\n';
  rs->buffer[H1P_HEADER_START - 2] = '\r';
  tmp = rs->response.status / 10;
  *(rs->buffer + start + 9) = '0' + (tmp / 10);
  *(rs->buffer + start + 11) = '0' + (rs->response.status - (10 * tmp));
  *(rs->buffer + start + 10) = '0' + (tmp - (10 * (tmp / 10)));
}

void http1_response_send_headers(http1_response_s *rs) {
  if (!rs->buffer_end)
    return;
  http1_response_finalize_headers(rs);
  spn_lock(&rs->lock);
  rs->use_count++;
  spn_unlock(&rs->lock);
  sock_write2(.uuid = rs->response.fd, .buffer = rs,
              .offset =
                  ((uintptr_t)(rs->buffer + rs->buffer_start) - (uintptr_t)rs),
              .length = rs->buffer_end - rs->buffer_start, .move = 1,
              .dealloc = (void (*)(void *))http1_response_destroy);
}

/** Sends the data and destroys the response object.*/
void http1_response_finish(http_response_s *rs) {
  if (!rs->headers_sent)
    http1_response_send_headers((http1_response_s *)rs);
  defer(http1_response_deffered_destroy, rs, NULL);
}

/* *****************************************************************************
Writing data to the response object
***************************************************************************** */

/**
Writes a header to the response. This function writes only the requested
number of bytes from the header name and the requested number of bytes from
the header value. It can be used even when the header name and value don't
contain NULL terminating bytes by passing the `.name_len` or `.value_len` data
in the `http_headers_s` structure.

If the header buffer is full or the headers were already sent (new headers
cannot be sent), the function will return -1.

On success, the function returns 0.
*/
int http1_response_write_header_fn(http_response_s *rs_, http_header_s header) {
  http1_response_s *rs = (http1_response_s *)rs_;
  if (rs->buffer_end + header.name_len + header.data_len >=
      HTTP1_MAX_HEADER_SIZE - H1P_OVERFLOW_PADDING - 5)
    return -1;
  size_t org_pos = rs->buffer_end;
  if (h1p_protected_copy(rs, (void *)header.name, header.name_len))
    goto error;
  rs->buffer_end += header.name_len;
  rs->buffer[rs->buffer_end++] = ':';
  if (h1p_protected_copy(rs, (void *)header.data, header.data_len))
    goto error;
  rs->buffer[rs->buffer_end++] = '\r';
  rs->buffer[rs->buffer_end++] = '\n';
  return 0;
error:
  rs->buffer_end = org_pos;
  return -1;
}

/**
Set / Delete a cookie using this helper function.

This function writes a cookie header to the response. Only the requested
number of bytes from the cookie value and name are written (if none are
provided, a terminating NULL byte is assumed).

Both the name and the value of the cookie are checked for validity (legal
characters), but other properties aren't reviewed (domain/path) - please make
sure to use only valid data, as HTTP imposes restrictions on these things.

If the header buffer is full or the headers were already sent (new headers
cannot be sent), the function will return -1.

On success, the function returns 0.
*/
int http1_response_set_cookie(http_response_s *, http_cookie_s);

/**
Sends the headers (if they weren't previously sent) and writes the data to the
underlying socket.

The body will be copied to the server's outgoing buffer.

If the connection was already closed, the function will return -1. On success,
the function returns 0.
*/
int http1_response_write_body(http_response_s *rs, const char *body,
                              size_t length) {
  if (!sock_isvalid(rs->fd))
    return -1;
  http1_response_s *rs1 = (http1_response_s *)rs;
  size_t tmp;
  if (!rs->headers_sent) {
    http1_response_finalize_headers(rs1);
    tmp = (length + rs1->buffer_end >= HTTP1_MAX_HEADER_SIZE)
              ? HTTP1_MAX_HEADER_SIZE - rs1->buffer_end
              : length;
    memcpy(rs1->buffer + rs1->buffer_end, body, tmp);
    http1_response_send_headers(rs1);
    length -= tmp;
    body += tmp;
  }
  if (length)
    return (sock_write(rs->fd, body, length) >= 0);
  return 0;
}

/**
Sends the headers (if they weren't previously sent) and writes the data to the
underlying socket.

The server's outgoing buffer will take ownership of the file and close it
using `fclose` once the data was sent.

If the connection was already closed, the function will return -1. On success,
the function returns 0.
*/
int http1_response_sendfile(http_response_s *, int source_fd, off_t offset,
                            size_t length);
