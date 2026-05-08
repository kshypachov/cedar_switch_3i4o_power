#include "firmware_updater.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/printk.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/bbram.h>

#include "ca_certificate.h"

#define HTTP_HOST "raw.githubusercontent.com"
#define HTTP_PORT "443"
#define HTTP_PATH "/kshypachov/cedar_switch_3i3o_power/refs/heads/main/zephyr.signed.bin"

#define SSTRLEN(s) (sizeof(s) - 1)
#define CHECK(r) { if (r < 0) { printf("Error: %d\n", (int)r); return -1; } }

static char response[1024];
static uint8_t verify_buf[256];

/* Provided by MCUboot bootutil library when CONFIG_MCUBOOT_BOOTUTIL_LIB=y */
extern int boot_set_pending(int permanent);

#define FWUPD_CONFIRM_RTC_PATTERN 0xC0DEF00DU

struct download_ctx {
	const struct flash_area *fa;
	off_t flash_offset;
	int write_error;
};

static int http_download_cb(struct http_response *rsp, enum http_final_call final_data,
			    void *user_data)
{
	struct download_ctx *ctx = user_data;

	if (rsp == NULL) {
		return -EINVAL;
	}

	if (rsp->body_frag_len > 0) {
		if ((ctx->flash_offset + (off_t)rsp->body_frag_len) > ctx->fa->fa_size) {
			printf("Image does not fit in slot1\n");
			ctx->write_error = -EFBIG;
			return -EFBIG;
		}

		ctx->write_error = flash_area_write(ctx->fa, ctx->flash_offset,
						    rsp->body_frag_start, rsp->body_frag_len);
		if (ctx->write_error < 0) {
			printf("Failed to write to slot1 flash area: %d\n", ctx->write_error);
			return ctx->write_error;
		}

		ctx->flash_offset += (off_t)rsp->body_frag_len;
	}

	if (final_data == HTTP_DATA_FINAL) {
		printf("HTTP download complete, written %ld bytes\n", (long)ctx->flash_offset);
	}

	return 0;
}



void dump_addrinfo(const struct addrinfo *ai)
{
	printf("addrinfo @%p: ai_family=%d, ai_socktype=%d, ai_protocol=%d, "
		   "sa_family=%d, sin_port=%x\n",
		   ai, ai->ai_family, ai->ai_socktype, ai->ai_protocol, ai->ai_addr->sa_family,
		   ntohs(((struct sockaddr_in *)ai->ai_addr)->sin_port));
}

int firmware_updater_fetch_and_print_prjconf(void)
{
	static struct addrinfo hints;
	struct addrinfo *res;
	const struct flash_area *slot1_fa;
	int st, sock;
	struct download_ctx dl_ctx = { 0 };

	// tls_credential_add(CA_CERTIFICATE_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
	// 	   ca_certificate, sizeof(ca_certificate));

	printf("Preparing HTTP GET request for http://" HTTP_HOST
	   ":" HTTP_PORT HTTP_PATH "\n");

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	st = getaddrinfo(HTTP_HOST, HTTP_PORT, &hints, &res);
	printf("getaddrinfo status: %d\n", st);

	if (st != 0) {
		printf("Unable to resolve address, quitting\n");
		return 0;
	}

	dump_addrinfo(res);

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	sock = socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_3);
#else
	sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
#endif

	CHECK(sock);
	printf("sock = %d\n", sock);

	struct timeval recv_timeout = {
		.tv_sec = 300,
		.tv_usec = 0,
	};
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) < 0) {
		printf("Failed to set recv timeout: %d\n", errno);
		close(sock);
		freeaddrinfo(res);
		return -1;
	}

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	int peer_verify = TLS_PEER_VERIFY_NONE;
	int ret = setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &peer_verify, sizeof(peer_verify));
	if (ret < 0) {
		printf("Failed to set peer verify: %d\n", ret);
		close(sock);
		freeaddrinfo(res);
		return -1;
	}
#endif

	st = flash_area_open(FIXED_PARTITION_ID(slot1_partition), &slot1_fa);
	if (st < 0) {
		printf("Failed to open slot1 flash area: %d\n", st);
		close(sock);
		freeaddrinfo(res);
		return -1;
	}
	dl_ctx.fa = slot1_fa;

	st = flash_area_erase(slot1_fa, 0, slot1_fa->fa_size);
	if (st < 0) {
		printf("Failed to erase slot1 flash area: %d\n", st);
		flash_area_close(slot1_fa);
		close(sock);
		freeaddrinfo(res);
		return -1;
	}

	if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
		printf("Connect failed: %d\n", errno);
		flash_area_close(slot1_fa);
		close(sock);
		freeaddrinfo(res);
		return -1;
	}

	struct http_request req = { 0 };
	req.method = HTTP_GET;
	req.url = HTTP_PATH;
	req.host = HTTP_HOST;
	req.protocol = "HTTP/1.1";
	req.response = http_download_cb;
	req.recv_buf = (uint8_t *)response;
	req.recv_buf_len = sizeof(response);

	st = http_client_req(sock, &req, 300000, &dl_ctx);
	if (st < 0) {
		printf("HTTP request failed: %d\n", st);
		close(sock);
		freeaddrinfo(res);
		flash_area_close(slot1_fa);
		return -1;
	}
	if (dl_ctx.write_error < 0) {
		close(sock);
		freeaddrinfo(res);
		flash_area_close(slot1_fa);
		return -1;
	}

	printf("\nClose socket\n");

	off_t verify_offset = 0;
	while (verify_offset < dl_ctx.flash_offset) {
		size_t chunk = MIN(sizeof(verify_buf), (size_t)(dl_ctx.flash_offset - verify_offset));

		st = flash_area_read(slot1_fa, verify_offset, verify_buf, chunk);
		if (st < 0) {
			printf("Failed to read back slot1 at offset %ld: %d\n",
			       (long)verify_offset, st);
			break;
		}

		verify_offset += chunk;
	}

	if (verify_offset == dl_ctx.flash_offset) {
		printf("Readback OK: %ld bytes\n", (long)verify_offset);
	}

	flash_area_close(slot1_fa);
	(void)close(sock);
	freeaddrinfo(res);

	int rc = boot_set_pending(BOOT_UPGRADE_TEST);
	if (rc != 0) {
		printk("boot_set_pending failed: %d\n", rc);
		return rc;
	}

	return 0;

}

int firmware_updater_confirm_image(void)
{
	const struct device *bbram = DEVICE_DT_GET_ANY(st_stm32_bbram);
	size_t bbram_size = 0;
	uint32_t pattern = FWUPD_CONFIRM_RTC_PATTERN;
	int rc;

	if (bbram == NULL) {
		printk("BBRAM device not found in devicetree\n");
		return -ENODEV;
	}

	if (!device_is_ready(bbram)) {
		printk("BBRAM device is not ready\n");
		return -ENODEV;
	}

	rc = bbram_get_size(bbram, &bbram_size);
	if (rc != 0) {
		printk("bbram_get_size failed: %d\n", rc);
		return rc;
	}

	if (bbram_size < sizeof(pattern)) {
		printk("BBRAM size too small: %u\n", (unsigned)bbram_size);
		return -ENOSPC;
	}

	rc = bbram_write(bbram, 0, sizeof(pattern), (const uint8_t *)&pattern);
	if (rc != 0) {
		printk("bbram_write failed: %d\n", rc);
		return rc;
	}

	return 0;

}
