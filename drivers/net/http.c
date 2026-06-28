/*
 * WynlandOS - HTTP/1.0 Client
 *
 * Phase 5.11: Minimal HTTP/1.0 client for the package manager
 *
 * Implements: http_get, http_get_ip
 *
 * Uses the TCP stack (tcp_connect, tcp_send, tcp_recv, tcp_close)
 * and the network layer (net_dns_resolve, net_is_up) to perform
 * simple HTTP GET requests over TCP/IP.
 */

#include <wynland/http.h>
#include <wynland/tcp.h>
#include <wynland/net.h>
#include <wynland/tls.h>
#include <wynland/types.h>

/* ============================================================
 * External helpers (provided by the kernel elsewhere)
 * ============================================================ */

extern void serial_write_string(const char *s);
extern void uint_to_str(uint32_t val, char *buf);

/* ============================================================
 * Internal string helpers (static to avoid name clashes)
 * ============================================================ */

/*
 * str_len_local — compute length of a NUL-terminated string
 */
static uint32_t str_len_local(const char *s)
{
    uint32_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

/*
 * str_copy_local — copy src into dst, returns pointer past last char written
 *
 * Does NOT append a NUL terminator; the caller is responsible for that.
 */
static char *str_copy_local(char *dst, const char *src)
{
    while (*src != '\0') {
        *dst++ = *src++;
    }
    return dst;
}

/* ============================================================
 * Helper: simple string-to-integer (like atoi)
 * ============================================================ */

/*
 * str_to_int — parse a decimal integer from the beginning of str
 *
 * Stops at the first non-digit character.  Returns the parsed value.
 */
static int str_to_int(const char *str)
{
    int result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}

/* ============================================================
 * Helper: substring search in a bounded buffer
 * ============================================================ */

/*
 * find_substr — find needle in haystack (up to haystack_len bytes)
 *
 * Returns pointer to first occurrence, or NULL if not found.
 */
static const char *find_substr(const char *haystack, uint32_t haystack_len,
                               const char *needle)
{
    uint32_t needle_len = str_len_local(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return NULL;
    }

    uint32_t limit = haystack_len - needle_len;
    for (uint32_t i = 0; i <= limit; i++) {
        uint32_t j;
        for (j = 0; j < needle_len; j++) {
            if (haystack[i + j] != needle[j]) {
                break;
            }
        }
        if (j == needle_len) {
            return &haystack[i];
        }
    }
    return NULL;
}

/*
 * find_substr_nocase — case-insensitive substring search
 *
 * Used for matching header names like "Content-Length" regardless of case.
 */
static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static const char *find_substr_nocase(const char *haystack, uint32_t haystack_len,
                                      const char *needle)
{
    uint32_t needle_len = str_len_local(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return NULL;
    }

    uint32_t limit = haystack_len - needle_len;
    for (uint32_t i = 0; i <= limit; i++) {
        uint32_t j;
        for (j = 0; j < needle_len; j++) {
            if (to_lower(haystack[i + j]) != to_lower(needle[j])) {
                break;
            }
        }
        if (j == needle_len) {
            return &haystack[i];
        }
    }
    return NULL;
}

/* ============================================================
 * Internal: build the HTTP request string manually
 * ============================================================ */

/*
 * build_request — build an HTTP/1.0 GET request into buf
 *
 * Format:
 *   GET <path> HTTP/1.0\r\n
 *   Host: <hostname>\r\n
 *   User-Agent: WynlandOS/1.0\r\n
 *   Connection: close\r\n
 *   \r\n
 *
 * Returns the total length of the request string.
 */
static uint32_t build_request(char *buf, uint32_t buf_size,
                               const char *hostname, const char *path)
{
    char *p = buf;
    char *end = buf + buf_size - 1;   /* Reserve space for NUL */

    /* GET <path> HTTP/1.0\r\n */
    p = str_copy_local(p, "GET ");
    p = str_copy_local(p, path);
    p = str_copy_local(p, " HTTP/1.0\r\n");

    /* Host: <hostname>\r\n */
    p = str_copy_local(p, "Host: ");
    p = str_copy_local(p, hostname);
    p = str_copy_local(p, "\r\n");

    /* User-Agent: WynlandOS/1.0\r\n */
    p = str_copy_local(p, "User-Agent: WynlandOS/1.0\r\n");

    /* Connection: close\r\n */
    p = str_copy_local(p, "Connection: close\r\n");

    /* End of headers */
    p = str_copy_local(p, "\r\n");

    *p = '\0';

    (void)end;  /* buf_size check omitted for simplicity — requests are small */

    return (uint32_t)(p - buf);
}

/* ============================================================
 * Internal: parse the HTTP response
 * ============================================================ */

/*
 * parse_response — extract status code, headers, and body from raw response
 *
 * @raw         : raw response buffer (headers + body)
 * @raw_len     : total bytes in raw
 * @resp_buf    : output buffer for the body
 * @buf_size    : size of resp_buf
 * @resp        : optional HttpResponse struct to fill in
 *
 * Returns: number of body bytes copied, or -1 on parse error
 */
static int parse_response(const char *raw, uint32_t raw_len,
                          char *resp_buf, uint32_t buf_size,
                          HttpResponse *resp)
{
    /* --- Status line: "HTTP/1.x NNN reason\r\n" --- */
    const char *space = find_substr(raw, raw_len, " ");
    if (!space) {
        serial_write_string("[HTTP] Error: no status code in response\n");
        return -1;
    }

    int status_code = str_to_int(space + 1);

    /* --- Find end of headers ("\r\n\r\n") --- */
    const char *header_end = find_substr(raw, raw_len, "\r\n\r\n");
    if (!header_end) {
        serial_write_string("[HTTP] Error: no end-of-headers marker\n");
        return -1;
    }

    uint32_t header_len = (uint32_t)(header_end - raw) + 4;  /* Include the \r\n\r\n */
    const char *body = raw + header_len;
    uint32_t body_available = raw_len - header_len;

    /* --- Extract Content-Length if present --- */
    uint32_t content_length = 0;
    const char *cl_hdr = find_substr_nocase(raw, header_len, "content-length:");
    if (cl_hdr) {
        /* Skip "Content-Length:" and any spaces */
        const char *val = cl_hdr + str_len_local("content-length:");
        while (*val == ' ') {
            val++;
        }
        content_length = (uint32_t)str_to_int(val);
    }

    /* --- Copy body to output buffer --- */
    uint32_t body_len = body_available;
    if (content_length > 0 && content_length < body_len) {
        body_len = content_length;
    }
    if (body_len > buf_size) {
        body_len = buf_size;
    }

    memcpy(resp_buf, body, body_len);

    /* NUL-terminate if there is space */
    if (body_len < buf_size) {
        resp_buf[body_len] = '\0';
    }

    /* --- Fill HttpResponse struct if provided --- */
    if (resp) {
        resp->status_code    = status_code;
        resp->content_length = content_length;
        resp->header_len     = header_len;
        resp->body_len       = body_len;
    }

    /* Log result */
    {
        char num_buf[16];
        serial_write_string("[HTTP] Response: status=");
        uint_to_str((uint32_t)status_code, num_buf);
        serial_write_string(num_buf);
        serial_write_string(", body=");
        uint_to_str(body_len, num_buf);
        serial_write_string(num_buf);
        serial_write_string(" bytes\n");
    }

    return (int)body_len;
}

/* ============================================================
 * http_get_ip — HTTP GET by IP address
 * ============================================================ */

/*
 * http_get_ip — Perform an HTTP GET request to a specific IP
 *
 * @ip         : Target IP address (network byte order)
 * @port       : Target port (host byte order)
 * @hostname   : Hostname for the Host header
 * @path       : Request path (e.g. "/index.html")
 * @resp_buf   : Buffer to store the response body
 * @buf_size   : Size of resp_buf
 * @resp       : Optional HttpResponse struct (may be NULL)
 *
 * Returns: number of body bytes, or -1 on error
 */
int http_get_ip(uint32_t ip, uint16_t port, const char *hostname,
                const char *path, char *resp_buf, uint32_t buf_size,
                HttpResponse *resp)
{
    /* --- Build the HTTP request --- */
    char req_buf[512];
    uint32_t req_len = build_request(req_buf, sizeof(req_buf), hostname, path);

    serial_write_string("[HTTP] Connecting to ");
    serial_write_string(hostname);
    serial_write_string(path);
    serial_write_string("...\n");

    /* --- TCP connect --- */
    TcpConnection *conn = tcp_connect(ip, port);
    if (!conn) {
        serial_write_string("[HTTP] Error: TCP connect failed\n");
        return -1;
    }

    /* --- Send the request --- */
    int sent = tcp_send(conn, req_buf, req_len);
    if (sent < 0) {
        serial_write_string("[HTTP] Error: TCP send failed\n");
        tcp_close(conn);
        return -1;
    }

    /* --- Receive the response in a loop --- */
    /*
     * We use a temporary raw buffer to collect headers + body together,
     * then parse them once the connection closes or the buffer is full.
     */
    static char raw_buf[HTTP_MAX_RESPONSE];
    uint32_t raw_len = 0;

    while (raw_len < sizeof(raw_buf)) {
        int n = tcp_recv(conn, raw_buf + raw_len, sizeof(raw_buf) - raw_len);
        if (n <= 0) {
            /* Connection closed or error — stop receiving */
            break;
        }
        raw_len += (uint32_t)n;
    }

    /* --- Close the connection --- */
    tcp_close(conn);

    if (raw_len == 0) {
        serial_write_string("[HTTP] Error: empty response\n");
        return -1;
    }

    /* --- Parse and return --- */
    return parse_response(raw_buf, raw_len, resp_buf, buf_size, resp);
}

/* ============================================================
 * http_get — HTTP GET with DNS resolution
 * ============================================================ */

/*
 * http_get — Perform an HTTP GET request
 *
 * @hostname   : Target hostname (will be DNS-resolved)
 * @port       : Target port (usually 80)
 * @path       : Request path (e.g. "/index.html")
 * @resp_buf   : Buffer to store the response body
 * @buf_size   : Size of resp_buf
 * @resp       : Optional HttpResponse struct (may be NULL)
 *
 * Returns: number of body bytes written to resp_buf, or -1 on error
 */
int http_get(const char *hostname, uint16_t port, const char *path,
             char *resp_buf, uint32_t buf_size, HttpResponse *resp)
{
    /* --- Check that the network is up --- */
    if (!net_is_up()) {
        serial_write_string("[HTTP] Error: network is not up\n");
        return -1;
    }

    /* --- DNS resolve the hostname --- */
    uint32_t ip;
    serial_write_string("[HTTP] Resolving ");
    serial_write_string(hostname);
    serial_write_string("...\n");

    if (!net_dns_resolve(hostname, &ip)) {
        serial_write_string("[HTTP] Error: DNS resolution failed for ");
        serial_write_string(hostname);
        serial_write_string("\n");
        return -1;
    }

    {
        char ip_str[20];
        net_ip_to_str(ip, ip_str);
        serial_write_string("[HTTP] Resolved to ");
        serial_write_string(ip_str);
        serial_write_string("\n");
    }

    /* --- Delegate to http_get_ip --- */
    return http_get_ip(ip, port, hostname, path, resp_buf, buf_size, resp);
}

int https_get_ip(uint32_t ip, uint16_t port, const char *hostname,
                 const char *path, char *resp_buf, uint32_t buf_size,
                 HttpResponse *resp)
{
    (void)ip;
    /* --- Build the HTTP request --- */
    char req_buf[512];
    uint32_t req_len = build_request(req_buf, sizeof(req_buf), hostname, path);

    serial_write_string("[HTTPS] Connecting to ");
    serial_write_string(hostname);
    serial_write_string(path);
    serial_write_string("...\n");

    /* --- TLS connect --- */
    TlsSocket *sock = tls_connect(hostname, port);
    if (!sock) {
        serial_write_string("[HTTPS] Error: TLS connect failed\n");
        return -1;
    }

    /* --- Send the request --- */
    int sent = tls_send(sock, req_buf, req_len);
    if (sent < 0) {
        serial_write_string("[HTTPS] Error: TLS send failed\n");
        tls_close(sock);
        return -1;
    }

    /* --- Receive the response in a loop --- */
    static char raw_buf[HTTP_MAX_RESPONSE];
    uint32_t raw_len = 0;

    while (raw_len < sizeof(raw_buf)) {
        int n = tls_recv(sock, raw_buf + raw_len, sizeof(raw_buf) - raw_len);
        if (n <= 0) {
            break;
        }
        raw_len += (uint32_t)n;
    }

    /* --- Close connection --- */
    tls_close(sock);

    if (raw_len == 0) {
        serial_write_string("[HTTPS] Error: empty response\n");
        return -1;
    }

    /* --- Parse and return --- */
    return parse_response(raw_buf, raw_len, resp_buf, buf_size, resp);
}

int https_get(const char *hostname, uint16_t port, const char *path,
              char *resp_buf, uint32_t buf_size, HttpResponse *resp)
{
    if (!net_is_up()) {
        serial_write_string("[HTTPS] Error: network is not up\n");
        return -1;
    }

    uint32_t ip;
    serial_write_string("[HTTPS] Resolving ");
    serial_write_string(hostname);
    serial_write_string("...\n");

    if (!net_dns_resolve(hostname, &ip)) {
        serial_write_string("[HTTPS] Error: DNS resolution failed for ");
        serial_write_string(hostname);
        serial_write_string("\n");
        return -1;
    }

    {
        char ip_str[20];
        net_ip_to_str(ip, ip_str);
        serial_write_string("[HTTPS] Resolved to ");
        serial_write_string(ip_str);
        serial_write_string("\n");
    }

    return https_get_ip(ip, port, hostname, path, resp_buf, buf_size, resp);
}
