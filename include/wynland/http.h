/*
 * WynlandOS - HTTP Client Header
 *
 * Phase 5.11: Minimal HTTP/1.0 client for the package manager
 */
#pragma once

#include <wynland/types.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define HTTP_PORT              80
#define HTTP_MAX_RESPONSE      32768
#define HTTP_MAX_HEADER_SIZE   2048

/* ============================================================
 * HTTP Response Info
 * ============================================================ */

typedef struct {
    int      status_code;       /* e.g. 200, 404, 301 */
    uint32_t content_length;    /* From Content-Length header, or 0 if unknown */
    uint32_t header_len;        /* Byte offset where body starts */
    uint32_t body_len;          /* Actual body bytes received */
} HttpResponse;

/* ============================================================
 * HTTP Public API
 * ============================================================ */

/*
 * http_get — Perform an HTTP GET request
 *
 * @hostname   : Target hostname (will be DNS-resolved)
 * @port       : Target port (usually 80)
 * @path       : Request path (e.g. "/index.html")
 * @resp_buf   : Buffer to store the response body
 * @buf_size   : Size of resp_buf
 * @resp       : Optional HttpResponse struct filled with metadata (may be NULL)
 *
 * Returns: number of body bytes written to resp_buf, or -1 on error
 */
int http_get(const char *hostname, uint16_t port, const char *path,
             char *resp_buf, uint32_t buf_size, HttpResponse *resp);

/*
 * http_get_ip — Same as http_get but takes an IP address directly
 *               (skips DNS resolution)
 */
int http_get_ip(uint32_t ip, uint16_t port, const char *hostname,
                const char *path, char *resp_buf, uint32_t buf_size,
                HttpResponse *resp);
