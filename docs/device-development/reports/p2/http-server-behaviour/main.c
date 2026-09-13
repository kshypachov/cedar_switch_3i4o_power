/* P2 measurement: how Zephyr's HTTP/1 server treats the cases web-api depends on. */
#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <strings.h>

static uint16_t port = 8080;
HTTP_SERVICE_DEFINE(spike_service, "127.0.0.1", &port, 4, 10, NULL, NULL, NULL);

HTTP_SERVER_REGISTER_HEADER_CAPTURE(cap_cookie, "Cookie");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(cap_csrf, "X-CSRF-Token");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(cap_origin, "Origin");

static atomic_t aborted, completed;
static int hdr_status = -1;
static size_t hdr_count;
static char cookie[300];
static char matched[64];
static int body_callbacks;
static size_t body_total;

static int dyn_cb(struct http_client_ctx *client, enum http_transaction_status status,
		  const struct http_request_ctx *req, struct http_response_ctx *rsp, void *user_data)
{
	static const char hello[] = "hello";
	static const char done[] = "done";

	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		atomic_inc(&aborted);
		printk("[cb] %s ABORTED\n", (char *)user_data);
		return 0;
	}
	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		atomic_inc(&completed);
		return 0;
	}
	if (req->headers_status != HTTP_HEADER_STATUS_NONE) {
		hdr_status = req->headers_status;
		hdr_count = req->header_count;
		cookie[0] = '\0';
		for (size_t i = 0; i < req->header_count; i++) {
			if (strcasecmp(req->headers[i].name, "Cookie") == 0) {
				strncpy(cookie, req->headers[i].value, sizeof(cookie) - 1);
			}
		}
		strncpy(matched, (char *)user_data, sizeof(matched) - 1);
		printk("[cb] %s url=%s first-cb data_len=%zu hdr_status=%d count=%zu\n",
		       (char *)user_data, client->url_buffer, req->data_len, req->headers_status,
		       req->header_count);
	}
	switch (client->method) {
	case HTTP_DELETE:
		rsp->status = HTTP_204_NO_CONTENT;
		rsp->final_chunk = true;
		break;
	case HTTP_GET:
		rsp->body = hello;
		rsp->body_len = sizeof(hello) - 1;
		rsp->final_chunk = true;
		break;
	case HTTP_POST:
		body_callbacks++;
		body_total += req->data_len;
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			rsp->body = done;
			rsp->body_len = sizeof(done) - 1;
			rsp->final_chunk = true;
		}
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

#define DYN(name, tag)                                                                             \
	static struct http_resource_detail_dynamic name = {                                        \
		.common = {.type = HTTP_RESOURCE_TYPE_DYNAMIC,                                     \
			   .bitmask_of_supported_http_methods =                                    \
				   BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_DELETE),              \
			   .content_type = "application/json"},                                    \
		.cb = dyn_cb,                                                                      \
		.user_data = tag}

DYN(d_dyn, "dyn");
DYN(d_cancel, "jobs-cancel");
DYN(d_jobs, "jobs");
HTTP_RESOURCE_DEFINE(r_dyn, spike_service, "/dyn", &d_dyn);
HTTP_RESOURCE_DEFINE(a_cancel, spike_service, "/api/v1/jobs/*/cancel", &d_cancel);
HTTP_RESOURCE_DEFINE(b_jobs, spike_service, "/api/v1/jobs/*", &d_jobs);

static int connect_client(void)
{
	struct net_sockaddr_in sa = {.sin_family = NET_AF_INET, .sin_port = net_htons(8080)};
	struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
	int fd = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);

	zassert_true(fd >= 0);
	zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_inet_pton(NET_AF_INET, "127.0.0.1", &sa.sin_addr);
	zassert_ok(zsock_connect(fd, (struct net_sockaddr *)&sa, sizeof(sa)));
	return fd;
}

static void send_str(int fd, const char *s)
{
	zassert_true(zsock_send(fd, s, strlen(s), 0) >= 0);
}

/* Read until the timeout; report bytes and whether the peer closed. */
static int drain(int fd, const char *label)
{
	char buf[1024];
	int total = 0;
	bool closed = false;

	for (;;) {
		int n = zsock_recv(fd, buf + total, sizeof(buf) - 1 - total, 0);

		if (n == 0) {
			closed = true;
			break;
		}
		if (n < 0) {
			break;
		}
		total += n;
	}
	buf[total] = '\0';
	printk("---- %s: %d bytes, peer %s\n", label, total, closed ? "CLOSED" : "open");
	for (int i = 0; i < total; i++) {
		if (buf[i] == '\r') {
			printk("\\r");
		} else if (buf[i] == '\n') {
			printk("\\n\n");
		} else {
			printk("%c", buf[i]);
		}
	}
	printk("\n---- end %s\n", label);
	return closed ? -1 : total;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	atomic_set(&aborted, 0);
	atomic_set(&completed, 0);
	zassert_ok(http_server_start());
	k_msleep(50);
}

static void after(void *f)
{
	ARG_UNUSED(f);
	http_server_stop();
	k_msleep(50);
}

ZTEST_SUITE(spike, NULL, NULL, before, after, NULL);

ZTEST(spike, test_a_204_keepalive)
{
	int fd = connect_client();

	send_str(fd, "DELETE /dyn HTTP/1.1\r\nHost: x\r\n\r\n");
	drain(fd, "A1 DELETE -> 204");
	send_str(fd, "GET /dyn HTTP/1.1\r\nHost: x\r\n\r\n");
	drain(fd, "A2 GET on same connection after 204");
	zsock_close(fd);
}

ZTEST(spike, test_b_second_client_during_body)
{
	int c1 = connect_client();
	int c2 = connect_client();

	body_callbacks = 0;
	body_total = 0;
	send_str(c1, "POST /dyn HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\n\r\n0123456789");
	k_msleep(200);
	send_str(c2, "GET /dyn HTTP/1.1\r\nHost: x\r\n\r\n");
	drain(c2, "B1 second client GET while first holds body");
	send_str(c1, "abcdefghij");
	drain(c1, "B2 first client after rest of body");
	printk("B body callbacks=%d total=%zu\n", body_callbacks, body_total);
	zsock_close(c1);
	zsock_close(c2);
}

ZTEST(spike, test_c_abort_by_close)
{
	int c1 = connect_client();

	send_str(c1, "POST /dyn HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n0123456789");
	k_msleep(200);
	zsock_close(c1);
	k_msleep(300);
	printk("C aborted after close = %ld\n", (long)atomic_get(&aborted));

	int c2 = connect_client();

	send_str(c2, "GET /dyn HTTP/1.1\r\nHost: x\r\n\r\n");
	drain(c2, "C2 GET after aborted client closed");
	zsock_close(c2);
}

ZTEST(spike, test_d_stalled_client)
{
	int c1 = connect_client();
	int64_t start = k_uptime_get();

	send_str(c1, "POST /dyn HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n0123456789");
	k_msleep(100);
	for (int i = 0; i < 12; i++) {
		int c = connect_client();
		char label[64];

		send_str(c, "GET /dyn HTTP/1.1\r\nHost: x\r\n\r\n");
		snprintk(label, sizeof(label), "D probe at %lld ms", (long long)(k_uptime_get() - start));
		drain(c, label);
		zsock_close(c);
		k_msleep(200);
	}
	printk("D aborted=%ld\n", (long)atomic_get(&aborted));
	zsock_close(c1);
}

ZTEST(spike, test_e_long_cookie)
{
	int fd = connect_client();

	send_str(fd, "GET /dyn HTTP/1.1\r\nHost: x\r\n"
		     "Cookie: cedar_session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n"
		     "X-CSRF-Token: BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\r\n"
		     "Origin: http://192.168.88.14\r\n\r\n");
	drain(fd, "E long cookie");
	printk("E hdr_status=%d count=%zu cookie='%s' (MAX_HEADER_LEN=%d)\n", hdr_status, hdr_count,
	       cookie, CONFIG_HTTP_SERVER_MAX_HEADER_LEN);
	zsock_close(fd);
}

ZTEST(spike, test_f_wildcard_order)
{
	int fd = connect_client();

	send_str(fd, "GET /api/v1/jobs/job_1 HTTP/1.1\r\nHost: x\r\n\r\n");
	drain(fd, "F1");
	printk("F1 /api/v1/jobs/job_1 matched=%s\n", matched);
	send_str(fd, "GET /api/v1/jobs/job_1/cancel HTTP/1.1\r\nHost: x\r\n\r\n");
	drain(fd, "F2");
	printk("F2 /api/v1/jobs/job_1/cancel matched=%s\n", matched);
	send_str(fd, "GET /api/v1/jobs/job_1?x=1 HTTP/1.1\r\nHost: x\r\n\r\n");
	drain(fd, "F3");
	printk("F3 with query matched=%s\n", matched);
	zsock_close(fd);
}
