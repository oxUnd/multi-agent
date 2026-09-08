#ifndef CLIENT_H
#define CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <signal.h>
#include "util/buf.h"
#include "util/cancel.h"

typedef int (*http_callback)(const char *data, size_t len, void *user_data);

#ifndef CURL_TYPES_H
typedef void CURL;
#endif

struct http_response {
	int status_code;
	morph_buf_t body;
	morph_buf_t headers;
};

struct http_request {
	const char *url;
	const char *method;
	const char *body;
	size_t body_len;
	const char **headers;
	int header_count;
	int timeout_seconds;
};

enum http_multipart_kind {
	HTTP_MULTIPART_TEXT,
	HTTP_MULTIPART_FILE,
};

struct http_multipart_part {
	enum http_multipart_kind kind;
	const char *name;
	const char *value;
	const char *content_type;
};

struct http_session {
	CURL *curl;
	morph_buf_t resp_body;
	morph_buf_t resp_headers;
	long status_code;
	int initialized;
};

int http_init(void);
void http_cleanup(void);
/* Thread-local request interruption, without cancelling the enclosing turn. */
void http_set_interrupt_check(int (*check)(void *), void *user_data);
void http_set_cancel_flag(volatile sig_atomic_t *flag);
void http_set_cancel_token(struct morph_cancel_token *token);
void http_cancel_from_signal(void);
void http_clear_signal_cancel(void);
int http_wait_cancelable(unsigned int milliseconds);
int http_get(const char *url, struct http_response *resp);
int http_get_ex(const char *url, const char **extra_headers,
		int extra_header_count, struct http_response *resp);
int http_post(const char *url, const char *body, size_t body_len,
	      const char *content_type, struct http_response *resp);
int http_post_ex(const char *url, const char *body, size_t body_len,
		 const char *content_type, const char **extra_headers,
		 int extra_header_count, struct http_response *resp);
int http_post_ex_timeout(const char *url, const char *body, size_t body_len,
			 const char *content_type,
			 const char **extra_headers, int extra_header_count,
			 long timeout_seconds, struct http_response *resp);
int http_post_multipart_ex(const char *url,
			   const struct http_multipart_part *parts,
			   int part_count, const char **extra_headers,
			   int extra_header_count, long timeout_seconds,
			   struct http_response *resp);
int http_post_sse(const char *url, const char *body, size_t body_len,
		  const char *content_type, http_callback cb, void *user_data);
int http_post_sse_ex(const char *url, const char *body, size_t body_len,
		     const char *content_type, const char **extra_headers,
		     int extra_header_count, http_callback cb, void *user_data);
int http_post_sse_ex_timeout(const char *url, const char *body, size_t body_len,
			     const char *content_type, const char **extra_headers,
			     int extra_header_count, long timeout_seconds,
			     http_callback cb, void *user_data);
void http_response_free(struct http_response *resp);

/* ---- HTTP Session (persistent curl handle) ---- */

int http_session_init(struct http_session *s);
void http_session_cleanup(struct http_session *s);
void http_session_reset(struct http_session *s);
int http_session_post(struct http_session *s, const char *url,
		      const char *body, size_t body_len,
		      const char *content_type,
		      const char **extra_headers, int extra_header_count,
		      long timeout_seconds);
const char *http_session_body(struct http_session *s, size_t *len);
long http_session_status(struct http_session *s);
char *http_session_header_get(struct http_session *s, const char *name);

#ifdef __cplusplus
}
#endif

#endif
