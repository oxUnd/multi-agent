#include "client.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <curl/curl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HTTP_SSE_DEFAULT_TIMEOUT_SECONDS 300L
#define HTTP_CONNECT_TIMEOUT_SECONDS 10L

static int http_initialized = 0;
static __thread volatile sig_atomic_t *http_cancel_flag = NULL;
static __thread struct morph_cancel_token *http_cancel_token = NULL;
static __thread int (*http_interrupt_check)(void *);
static __thread void *http_interrupt_user_data;
static volatile sig_atomic_t http_signal_cancelled = 0;

static int curl_debug_cb(CURL *handle, curl_infotype type,
			 char *data, size_t size, void *userp)
{
	(void)handle;
	(void)userp;
	if (type == CURLINFO_HEADER_OUT) {
		log_dbg(">> %.*s", (int)size, data);
	} else if (type == CURLINFO_HEADER_IN) {
		log_dbg("<< %.*s", (int)size, data);
	}
	return 0;
}

static void curl_apply_common_opts(CURL *curl, char *errbuf)
{
#ifdef __ANDROID__
	const char *ca_file;
	const char *ca_dir;
#endif

	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	if (errbuf) {
		errbuf[0] = '\0';
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
	}
#ifdef __ANDROID__
	ca_file = getenv("CURL_CA_BUNDLE");
	if (!ca_file || !*ca_file)
		ca_file = getenv("SSL_CERT_FILE");
	if (ca_file && *ca_file)
		curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
	ca_dir = getenv("SSL_CERT_DIR");
	if (ca_dir && *ca_dir)
		curl_easy_setopt(curl, CURLOPT_CAPATH, ca_dir);
#endif
	if (getenv("MORPH_DEBUG")) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
	}
}

static void curl_log_error(const char *what, CURLcode rc, const char *errbuf)
{
	if (errbuf && *errbuf) {
		log_err("%s failed: curl=%d %s (%s)", what, (int)rc,
			curl_easy_strerror(rc), errbuf);
	} else {
		log_err("%s failed: curl=%d %s", what, (int)rc,
			curl_easy_strerror(rc));
	}
}

void http_set_interrupt_check(int (*check)(void *), void *user_data)
{
	http_interrupt_check = check;
	http_interrupt_user_data = user_data;
}

void http_set_cancel_flag(volatile sig_atomic_t *flag)
{
	http_cancel_flag = flag;
}

void http_set_cancel_token(struct morph_cancel_token *token)
{
	http_cancel_token = token;
}

void http_cancel_from_signal(void)
{
	http_signal_cancelled = 1;
}

void http_clear_signal_cancel(void)
{
	http_signal_cancelled = 0;
}

static int http_cancelled(void)
{
	if (http_signal_cancelled)
		return 1;
	if (morph_cancel_token_is_cancelled(http_cancel_token))
		return 1;
	if (http_cancel_flag && *http_cancel_flag)
		return 1;
	return http_interrupt_check &&
		http_interrupt_check(http_interrupt_user_data);
}

int http_wait_cancelable(unsigned int milliseconds)
{
	const unsigned int slice_ms = 100;
	unsigned int waited = 0;

	while (waited < milliseconds) {
		struct timespec delay;
		unsigned int current = milliseconds - waited;
		if (current > slice_ms)
			current = slice_ms;
		if (http_cancelled())
			return -ECANCELED;
		delay.tv_sec = current / 1000;
		delay.tv_nsec = (long)(current % 1000) * 1000000L;
		while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
			if (http_cancelled())
				return -ECANCELED;
		}
		waited += current;
	}
	return http_cancelled() ? -ECANCELED : 0;
}

static int sse_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
			   curl_off_t ultotal, curl_off_t ulnow)
{
	(void)clientp;
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	return http_cancelled() ? 1 : 0;
}

static void sse_apply_cancel_opts(CURL *curl)
{
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_xferinfo_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
}

static int sse_map_curl_error(CURLcode rc, const char *errbuf)
{
	if (rc == CURLE_OK)
		return 0;
	if (http_cancelled() ||
	    rc == CURLE_WRITE_ERROR ||
	    rc == CURLE_ABORTED_BY_CALLBACK) {
		log_info("http: request cancelled by user");
		return -ECANCELED;
	}
	curl_log_error("sse request", rc, errbuf);
	return MORPH_ERR_NETWORK;
}

int http_init(void)
{
	CURLcode rc;

	if (http_initialized)
		return 0;
	rc = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (rc != CURLE_OK) {
		log_err("curl_global_init failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
	}
	http_initialized = 1;
	log_info("http client initialized");
	return 0;
}

void http_cleanup(void)
{
	if (http_initialized) {
		curl_global_cleanup();
		http_initialized = 0;
	}
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_response *resp = data;
	size_t total = size * nmemb;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	if (resp->body.cap == 0) {
		if (morph_buf_init(&resp->body, 65536) != 0)
			return 0;
	}
	if (morph_buf_append(&resp->body, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_response *resp = data;
	size_t total = size * nmemb;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	if (resp->headers.cap == 0) {
		if (morph_buf_init(&resp->headers, 4096) != 0)
			return 0;
	}
	if (morph_buf_append(&resp->headers, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

static int append_header(struct curl_slist **headers, const char *header)
{
	struct curl_slist *new_headers;

	if (!headers || !header)
		MORPH_RETURN(-EINVAL);
	new_headers = curl_slist_append(*headers, header);
	if (!new_headers)
		MORPH_RETURN(-ENOMEM);
	*headers = new_headers;
	return 0;
}

static int append_content_type_header(struct curl_slist **headers,
				      const char *content_type)
{
	char ct[256];

	if (!content_type)
		return 0;
	snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
	return append_header(headers, ct);
}

static int append_extra_headers(struct curl_slist **headers,
				const char **extra_headers,
				int extra_header_count)
{
	int rc;

	for (int i = 0; i < extra_header_count; i++) {
		if (extra_headers && extra_headers[i]) {
			rc = append_header(headers, extra_headers[i]);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int do_request(const char *url, const char *method, const char *body,
		      size_t body_len, const char *content_type,
		      const char **extra_headers, int extra_header_count,
		      struct http_response *resp, long timeout)
{
	CURLcode curl_rc;
	struct curl_slist *headers = NULL;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int is_post;
	int rc;
	CURL *curl;

	if (!http_initialized)
		rc = http_init();
	else
		rc = 0;
	if (rc != 0)
		return rc;

	curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);

	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	rc = append_content_type_header(&headers, content_type);
	if (rc != 0)
		goto out;
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_apply_common_opts(curl, errbuf);
	sse_apply_cancel_opts(curl);

	is_post = method && strcmp(method, "POST") == 0;
	if (is_post) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
				 (curl_off_t)body_len);
	} else if (method && strcmp(method, "GET") != 0) {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
		if (body) {
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
					 (curl_off_t)body_len);
		}
	}

	curl_rc = curl_easy_perform(curl);
	if (curl_rc != CURLE_OK) {
		if (http_cancelled()) {
			rc = -ECANCELED;
		} else if (curl_rc == CURLE_OPERATION_TIMEDOUT) {
			curl_log_error("http request", curl_rc, errbuf);
			rc = -ETIMEDOUT;
		} else {
			curl_log_error("http request", curl_rc, errbuf);
			rc = MORPH_ERR_NETWORK;
		}
		goto out;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return rc;
}

int http_get(const char *url, struct http_response *resp)
{
	if (!url || !resp)
		MORPH_RETURN(-EINVAL);
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "GET", NULL, 0, NULL, NULL, 0, resp, 30L);
}

int http_get_ex(const char *url, const char **extra_headers,
		int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "GET", NULL, 0, NULL, extra_headers,
			  extra_header_count, resp, 30L);
}

int http_post(const char *url, const char *body, size_t body_len,
	      const char *content_type, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "POST", body, body_len, content_type, NULL, 0,
			  resp, 60L);
}

int http_post_ex(const char *url, const char *body, size_t body_len,
		 const char *content_type, const char **extra_headers,
		 int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "POST", body, body_len, content_type,
			  extra_headers, extra_header_count, resp, 60L);
}

int http_post_ex_timeout(const char *url, const char *body, size_t body_len,
			 const char *content_type,
			 const char **extra_headers, int extra_header_count,
			 long timeout_seconds, struct http_response *resp)
{
	if (!url || !resp)
		MORPH_RETURN(-EINVAL);
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "POST", body, body_len, content_type,
			  extra_headers, extra_header_count, resp,
			  timeout_seconds);
}

int http_post_multipart_ex(const char *url,
			   const struct http_multipart_part *parts,
			   int part_count, const char **extra_headers,
			   int extra_header_count, long timeout_seconds,
			   struct http_response *resp)
{
	struct curl_slist *headers = NULL;
	curl_mime *mime = NULL;
	CURL *curl = NULL;
	CURLcode curl_rc;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int rc = 0;

	if (!url || !parts || part_count <= 0 || !resp)
		MORPH_RETURN(-EINVAL);
	memset(resp, 0, sizeof(*resp));
	if (!http_initialized) {
		rc = http_init();
		if (rc != 0)
			return rc;
	}
	curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	mime = curl_mime_init(curl);
	if (!mime) {
		rc = -ENOMEM;
		goto out;
	}
	for (int i = 0; i < part_count; i++) {
		curl_mimepart *part;

		if (!parts[i].name || !parts[i].value) {
			rc = -EINVAL;
			goto out;
		}
		part = curl_mime_addpart(mime);
		if (!part) {
			rc = -ENOMEM;
			goto out;
		}
		if (curl_mime_name(part, parts[i].name) != CURLE_OK) {
			rc = -ENOMEM;
			goto out;
		}
		if (parts[i].kind == HTTP_MULTIPART_FILE) {
			if (curl_mime_filedata(part, parts[i].value) != CURLE_OK) {
				rc = -EINVAL;
				goto out;
			}
		} else if (curl_mime_data(part, parts[i].value,
					 CURL_ZERO_TERMINATED) != CURLE_OK) {
			rc = -ENOMEM;
			goto out;
		}
		if (parts[i].content_type &&
		    curl_mime_type(part, parts[i].content_type) != CURLE_OK) {
			rc = -EINVAL;
			goto out;
		}
	}
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;
	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
			 HTTP_CONNECT_TIMEOUT_SECONDS);
	curl_apply_common_opts(curl, errbuf);
	sse_apply_cancel_opts(curl);

	curl_rc = curl_easy_perform(curl);
	if (curl_rc != CURLE_OK) {
		if (http_cancelled())
			rc = -ECANCELED;
		else if (curl_rc == CURLE_OPERATION_TIMEDOUT) {
			curl_log_error("http multipart request", curl_rc, errbuf);
			rc = -ETIMEDOUT;
		} else {
			curl_log_error("http multipart request", curl_rc, errbuf);
			rc = MORPH_ERR_NETWORK;
		}
		goto out;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	if (mime)
		curl_mime_free(mime);
	if (curl)
		curl_easy_cleanup(curl);
	return rc;
}

struct sse_write_data {
	http_callback cb;
	void *user_data;
	int callback_rc;
};

static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct sse_write_data *swd = data;
	size_t total;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	total = size * nmemb;
	log_dbg("sse_write_cb: received %zu bytes", total);
	if (http_cancelled())
		return 0;
	if (swd->cb) {
		int rc = swd->cb((const char *)ptr, total, swd->user_data);
		if (rc != 0) {
			swd->callback_rc = rc;
			return 0;
		}
		if (http_cancelled())
			return 0;
	}
	return total;
}

static int do_sse_request(const char *url, const char *body, size_t body_len,
			  const char *content_type,
			  const char **extra_headers, int extra_header_count,
			  long idle_timeout,
			  http_callback cb, void *user_data)
{
	struct curl_slist *headers = NULL;
	char ct[256];
	struct sse_write_data swd;
	CURLcode curl_rc;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int rc;
	CURL *curl;

	if (!url || !cb)
		return -EINVAL;
	if (!http_initialized)
		rc = http_init();
	else
		rc = 0;
	if (rc != 0)
		return rc;

	curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");

	snprintf(ct, sizeof(ct), "Content-Type: %s",
		 content_type ? content_type : "application/json");
	rc = append_header(&headers, ct);
	if (rc != 0)
		goto out;
	rc = append_header(&headers, "Accept: text/event-stream");
	if (rc != 0)
		goto out;
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;

	swd.cb = cb;
	swd.user_data = user_data;
	swd.callback_rc = 0;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &swd);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
			 HTTP_CONNECT_TIMEOUT_SECONDS);
	if (idle_timeout > 0) {
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle_timeout);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	}
	curl_apply_common_opts(curl, errbuf);
	sse_apply_cancel_opts(curl);

	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);

	log_dbg("http sse: request timeout disabled, idle_timeout=%lds",
		idle_timeout);
	curl_rc = curl_easy_perform(curl);

	if (swd.callback_rc != 0) {
		rc = swd.callback_rc;
		goto out;
	}
	if (curl_rc != CURLE_OK) {
		if (http_cancelled())
			rc = -ECANCELED;
		else if (curl_rc == CURLE_OPERATION_TIMEDOUT) {
			log_warn("http sse: curl idle timeout "
				 "(idle_timeout=%lds)", idle_timeout);
			rc = -ETIMEDOUT;
		} else {
			rc = sse_map_curl_error(curl_rc, errbuf);
		}
		goto out;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	rc = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return rc;
}

int http_post_sse(const char *url, const char *body, size_t body_len,
		   const char *content_type, http_callback cb, void *user_data)
{
	return do_sse_request(url, body, body_len, content_type, NULL, 0,
			      HTTP_SSE_DEFAULT_TIMEOUT_SECONDS, cb, user_data);
}

int http_post_sse_ex(const char *url, const char *body, size_t body_len,
		     const char *content_type, const char **extra_headers,
		     int extra_header_count, http_callback cb, void *user_data)
{
	return do_sse_request(url, body, body_len, content_type, extra_headers,
			      extra_header_count, HTTP_SSE_DEFAULT_TIMEOUT_SECONDS,
			      cb, user_data);
}

int http_post_sse_ex_timeout(const char *url, const char *body, size_t body_len,
			     const char *content_type, const char **extra_headers,
			     int extra_header_count, long timeout_seconds,
			     http_callback cb, void *user_data)
{
	long idle_timeout = timeout_seconds > 0 ? timeout_seconds :
		HTTP_SSE_DEFAULT_TIMEOUT_SECONDS;

	return do_sse_request(url, body, body_len, content_type, extra_headers,
			      extra_header_count, idle_timeout, cb, user_data);
}

void http_response_free(struct http_response *resp)
{
	if (!resp)
		return;
	morph_buf_cleanup(&resp->body);
	morph_buf_cleanup(&resp->headers);
	memset(resp, 0, sizeof(*resp));
}

/* ---- HTTP Session ---- */

static size_t session_write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_session *s = data;
	size_t total = size * nmemb;

	if (morph_buf_append(&s->resp_body, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

static size_t session_header_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_session *s = data;
	size_t total = size * nmemb;

	if (morph_buf_append(&s->resp_headers, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

int http_session_init(struct http_session *s)
{
	if (!s)
		return -EINVAL;
	if (!http_initialized) {
		int rc = http_init();
		if (rc != 0)
			return rc;
	}
	memset(s, 0, sizeof(*s));
	s->curl = curl_easy_init();
	if (!s->curl)
		MORPH_RETURN(-ENOMEM);
	morph_buf_init(&s->resp_body, 4096);
	morph_buf_init(&s->resp_headers, 2048);
	s->status_code = 0;
	s->initialized = 1;
	return 0;
}

void http_session_cleanup(struct http_session *s)
{
	if (!s)
		return;
	if (s->curl) {
		curl_easy_cleanup(s->curl);
		s->curl = NULL;
	}
	morph_buf_cleanup(&s->resp_body);
	morph_buf_cleanup(&s->resp_headers);
	s->initialized = 0;
}

void http_session_reset(struct http_session *s)
{
	if (!s)
		return;
	morph_buf_reset(&s->resp_body);
	morph_buf_reset(&s->resp_headers);
	s->status_code = 0;
}

int http_session_post(struct http_session *s, const char *url,
		      const char *body, size_t body_len,
		      const char *content_type,
		      const char **extra_headers, int extra_header_count,
		      long timeout_seconds)
{
	struct curl_slist *headers = NULL;
	CURLcode curl_rc;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int rc;

	if (!s || !s->curl || !url)
		return -EINVAL;

	http_session_reset(s);

	curl_easy_setopt(s->curl, CURLOPT_PROXY, "");

	rc = append_content_type_header(&headers, content_type);
	if (rc != 0)
		goto out;
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;
	if (headers)
		curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_setopt(s->curl, CURLOPT_URL, url);
	curl_easy_setopt(s->curl, CURLOPT_POST, 1L);
	curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, body ? body : "");
	curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
	curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, session_write_cb);
	curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, s);
	curl_easy_setopt(s->curl, CURLOPT_HEADERFUNCTION, session_header_cb);
	curl_easy_setopt(s->curl, CURLOPT_HEADERDATA, s);
	curl_easy_setopt(s->curl, CURLOPT_TIMEOUT, timeout_seconds > 0 ? timeout_seconds : 30L);
	curl_easy_setopt(s->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_apply_common_opts(s->curl, errbuf);
	sse_apply_cancel_opts(s->curl);

	curl_rc = curl_easy_perform(s->curl);
	if (curl_rc != CURLE_OK) {
		if (http_cancelled()) {
			rc = -ECANCELED;
		} else {
			curl_log_error("http session request", curl_rc,
				       errbuf);
			rc = MORPH_ERR_NETWORK;
		}
		goto out;
	}
	curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &status);
	s->status_code = status;
	rc = 0;

out:
	curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, NULL);
	if (headers)
		curl_slist_free_all(headers);
	return rc;
}

const char *http_session_body(struct http_session *s, size_t *len)
{
	if (!s)
		return NULL;
	if (len)
		*len = s->resp_body.len;
	return morph_buf_cstr(&s->resp_body);
}

long http_session_status(struct http_session *s)
{
	if (!s)
		return 0;
	return s->status_code;
}

char *http_session_header_get(struct http_session *s, const char *name)
{
	if (!s || !name)
		return NULL;
	const char *hdrs = morph_buf_cstr(&s->resp_headers);
	if (!hdrs)
		return NULL;
	size_t name_len = strlen(name);
	const char *p = hdrs;
	while (*p) {
		const char *eol = strstr(p, "\r\n");
		if (!eol)
			eol = p + strlen(p);
		size_t line_len = (size_t)(eol - p);
		if (line_len > name_len + 1) {
			size_t i;
			for (i = 0; i < name_len; i++) {
				char c = (p[i] >= 'A' && p[i] <= 'Z') ? (p[i] + 32) : p[i];
				char n = (name[i] >= 'A' && name[i] <= 'Z') ? (name[i] + 32) : name[i];
				if (c != n)
					break;
			}
			if (i == name_len && p[name_len] == ':') {
				const char *val = p + name_len + 1;
				while (*val == ' ')
					val++;
				size_t val_end = (size_t)(eol - val);
				char *result = malloc(val_end + 1);
				if (!result)
					return NULL;
				memcpy(result, val, val_end);
				result[val_end] = '\0';
				return result;
			}
		}
		p = (*eol == '\0') ? eol : eol + 2;
	}
	return NULL;
}
