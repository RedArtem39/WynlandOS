#!/usr/bin/env python3
import http.server
import urllib.request
import urllib.error
import sys
import gzip
import zlib

PORT = 8080

class TransparentProxyHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def handle_request(self, method):
        url = self.path
        
        # If the client sent a relative path, construct absolute URL
        if not url.startswith('http'):
            host = self.headers.get('Host', 'localhost')
            # Assume HTTPS by default for all external traffic
            url = f"https://{host}{url}"

        # Prepare request
        headers = {}
        for key, val in self.headers.items():
            kl = key.lower()
            if kl not in ['host', 'connection', 'accept-encoding']:
                headers[key] = val
        
        # Force decompression on host to save guest CPU/memory
        headers['Accept-Encoding'] = 'identity'

        # Read body if present
        body = None
        content_length = int(self.headers.get('Content-Length', 0))
        if content_length > 0:
            body = self.rfile.read(content_length)

        req = urllib.request.Request(url, data=body, headers=headers, method=method)
        
        try:
            # Disable certificate verification to keep it smooth for testing
            import ssl
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            
            with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
                self.send_response(resp.status)
                
                # Copy response headers, stripping encoding and connection controls
                for key, val in resp.headers.items():
                    kl = key.lower()
                    if kl not in ['connection', 'transfer-encoding', 'content-encoding']:
                        self.send_header(key, val)
                
                self.send_header('Connection', 'close')
                
                # Read data
                data = resp.read()
                
                # Double-check decompression if server ignored Accept-Encoding
                content_encoding = resp.headers.get('Content-Encoding', '').lower()
                if 'gzip' in content_encoding:
                    data = gzip.decompress(data)
                elif 'deflate' in content_encoding:
                    data = zlib.decompress(data)
                
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                
        except urllib.error.HTTPError as e:
            try:
                err_data = e.read()
            except Exception:
                err_data = b""
            self.send_response(e.code)
            self.send_header('Content-Length', str(len(err_data)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(err_data)
        except Exception as e:
            err_msg = str(e).encode('utf-8')
            self.send_response(500)
            self.send_header('Content-Length', str(len(err_msg)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(err_msg)

    def do_GET(self):
        self.handle_request('GET')

    def do_POST(self):
        self.handle_request('POST')

    def log_message(self, format, *args):
        # Log to stderr
        sys.stderr.write(f"[Proxy] {format % args}\n")

def run():
    server_address = ('', PORT)
    httpd = http.server.ThreadingHTTPServer(server_address, TransparentProxyHandler)
    sys.stderr.write(f"[Proxy] Running transparent proxy on port {PORT}...\n")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    sys.stderr.write("[Proxy] Stopping proxy.\n")

if __name__ == '__main__':
    run()
