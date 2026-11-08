# eXpServer Viva Examination & Concept Guide: Stages 14 to 16
> **Reference Documentation**: [eXpServer Official Roadmap](https://expserver.github.io/roadmap/) & Lab Specifications (NITC Networks Lab).

---

## 1. High-Level Roadmap & Phase 2 Vision

### The Big Picture: Transition from Phase 1 to Phase 2
* **Where you stopped at Stage 13 (End of Phase 1):**
  * In Phase 1, you built an **event-driven, non-blocking TCP server using Linux `epoll` in Edge-Triggered mode (`EPOLLET`)**.
  * You introduced the **Pipe Architecture** (`xps_pipe_t`, `xps_pipe_source_t`, `xps_pipe_sink_t`) and **Sessions** (`xps_session_t`) to stream data between sockets and disk files.
  * **The Limitation:** The server only dealt with raw, uninterpreted byte streams (like a dumb TCP echo or raw file transmitter). Ports and routes were hardcoded (e.g., port 8001 served a hardcoded `../public/sample.txt`).
* **Phase 2 (Application Layer — HTTP & Dynamic Config):**
  * The goal is to evolve the raw TCP pipeline into a compliant, configurable **HTTP/1.1 Web & Reverse-Proxy Server**.
  * **Stage 14:** Implement the **HTTP Request Parser** (`xps_http_req`) using a character-by-character state machine. The server can now understand what the client is asking for (methods, paths, headers). *Note: Upstream reverse proxying is temporarily paused here because the server cannot yet formulate or parse HTTP responses.*
  * **Stage 15:** Implement the **HTTP Response Serializer** (`xps_http_res`). The server now structures its replies according to RFC specs (status line, response headers, CRLF boundaries, and body).
  * **Stage 16:** Implement the **Dynamic Config Module** (`xps_config`) powered by the **Parson JSON library**. Eliminate all hardcoded ports and routes. Introduce dynamic prefix-matched routing supporting **File Serving**, **HTTP 302 Redirection**, and **HTTP Reverse Proxying**.

---

## 2. Deep Dive: Stage by Stage

```
                      +-------------------------------------------------------+
                      |         STAGE 13 (End of Phase 1)                     |
                      |  - Non-blocking epoll (ET)                            |
                      |  - Raw TCP Streams & Sessions                         |
                      |  - Hardcoded Ports & File Paths                       |
                      +-------------------------------------------------------+
                                                  |
                                                  v
                      +-------------------------------------------------------+
                      |         STAGE 14: HTTP Request Module                 |
                      |  - State Machine Request Parser (xps_http_req)        |
                      |  - Extracts Method, URI, Version, Headers             |
                      |  - Dynamic File Name extraction from request path     |
                      |  - Upstream paused (acts purely as file server)       |
                      +-------------------------------------------------------+
                                                  |
                                                  v
                      +-------------------------------------------------------+
                      |         STAGE 15: HTTP Response Module                |
                      |  - Structured Responses (xps_http_res)                |
                      |  - Status Line (code + reason phrase)                 |
                      |  - Dynamic Header List (xps_http_set_header)          |
                      |  - Strict CRLF (\r\n) Wire Serialization              |
                      +-------------------------------------------------------+
                                                  |
                                                  v
                      +-------------------------------------------------------+
                      |         STAGE 16: Dynamic JSON Config & Routing       |
                      |  - xps_config.json + Parson Library                   |
                      |  - Dynamic Listeners (ports 8001-8004)                |
                      |  - Prefix Routing: File Serve, Redirect, Proxy        |
                      |  - Restores full Reverse Proxy to upstream (port 3000)|
                      +-------------------------------------------------------+
```

---

### STAGE 14: HTTP Request Module

#### 1. Why was Stage 14 needed?
Up to Stage 13, if a client connected and sent `GET /cat.jpg HTTP/1.1`, the server treated the text as raw bytes and couldn't tell which file the client wanted. In Stage 14, we build an RFC-compliant HTTP Request Parser from scratch.

#### 2. Key Structures (`src/http/xps_http_req.h`)
```c
struct xps_http_req_s {
    xps_http_parser_state_t parser_state; // Current state in the parsing state machine
    xps_http_method_t method_n;           // Enum: HTTP_GET, HTTP_POST, etc.
    char *request_line;                   // Full line: "GET /cat.jpg HTTP/1.1"
    char *method;                         // "GET"
    char *uri;                            // "/cat.jpg"
    char *path;                           // Clean path
    char *http_version;                   // "HTTP/1.1"
    vec_void_t headers;                   // Vector of xps_keyval_t (Key: Value)
    xps_buffer_t *body;                   // Request body buffer
};
```

#### 3. How the Parser Works (State Machine)
- The parser processes the byte stream incrementally.
- It transitions through states:
  $$\text{PARSER\_START} \longrightarrow \text{METHOD} \longrightarrow \text{URI} \longrightarrow \text{VERSION} \longrightarrow \text{HEADER\_KEY} \longrightarrow \text{HEADER\_VAL} \longrightarrow \text{DONE}$$
- **Handling Incomplete Packets (`E_AGAIN`):**
  If the client sends partial data (headers haven't terminated with `\r\n\r\n`), `xps_http_req_create()` returns `error = E_AGAIN`. In `client_sink_handler`, the server does **not** fail; it leaves the data in the pipe and waits for the next `EPOLLIN` event.

#### 4. RFC Validation Rules Enforced
1. **Invalid Method**: Verb not supported $\rightarrow$ `400 Bad Request`.
2. **Missing HTTP Version**: Version omitted $\rightarrow$ `400 Bad Request`.
3. **Missing Host Header**: In HTTP/1.1, RFC 7230 strictly requires a `Host:` header. If absent $\rightarrow$ `400 Bad Request`.
4. **Header Format**: Headers must have a colon separator (`Key: Value`). Missing colon $\rightarrow$ `400 Bad Request`.

#### 5. Why Upstream Was Disabled in Stage 14
In Stage 14, we only wrote the **Request** parser. If we connected to an upstream server, the upstream server would send back an **HTTP Response**, which our server could not yet parse. Therefore, in Stage 14, all listeners temporarily operate as file servers.

---

### STAGE 15: HTTP Response Module

#### 1. Why was Stage 15 needed?
In Stage 14, response headers were manually constructed using string formatting (`sprintf(reply, "HTTP/1.1 200 OK\r\n...")`). This was brittle, error-prone, hardcoded, and violated clean separation of concerns. Stage 15 creates a dedicated `xps_http_res` module.

#### 2. Key Structures (`src/http/xps_http_res.h`)
```c
struct xps_http_res_s {
    char response_line[70]; // e.g., "HTTP/1.1 200 OK\r\n"
    vec_void_t headers;     // Vector of xps_keyval_t (Server, Content-Type, Content-Length)
    xps_buffer_t *body;     // Response body payload
};
```

#### 3. Core API Functions
1. `xps_http_res_create(xps_core_t *core, u_int code)`:
   - Formulates the status line matching the integer code (`200 OK`, `302 Found`, `400 Bad Request`, `404 Not Found`, `500 Internal Server Error`, `502 Bad Gateway`).
   - Automatically injects default headers: `Server: eXpServer`, `Date`, etc.
2. `xps_http_set_header(vec_void_t *headers, const char *key, const char *val)`:
   - Appends or updates a key-value header in the response header vector.
3. `xps_http_res_serialize(xps_http_res_t *res)`:
   - Formats the status line and headers into a contiguous memory buffer (`xps_buffer_t`) delimited by `\r\n` and ending with the HTTP header-body delimiter `\r\n\r\n`.

---

### STAGE 16: HTTP Config Module & Dynamic Multi-Routing

#### 1. Why was Stage 16 needed?
Real-world web servers (like Nginx, Apache, Envoy) do not hardcode ports or paths in source code. They use configuration files. Stage 16 introduces `xps_config.json` and a JSON parser library (**Parson**) to make the entire server dynamic.

#### 2. The Configuration Schema (`xps_config.json`)
```json
{
  "server_name": "eXpServer",
  "workers": 4,
  "servers": [
    {
      "listeners": [{ "host": "0.0.0.0", "port": 8001 }],
      "routes": [
        { "req_path": "/", "type": "file_serve", "dir_path": "../public" },
        { "req_path": "/redirect", "type": "redirect", "http_status_code": 302, "redirect_url": "http://localhost:8002/" }
      ]
    },
    {
      "listeners": [{ "host": "0.0.0.0", "port": 8002 }],
      "routes": [
        { "req_path": "/", "type": "reverse_proxy", "upstreams": ["localhost:3000"] }
      ]
    },
    {
      "listeners": [{ "host": "0.0.0.0", "port": 8003 }],
      "routes": [
        { "req_path": "/", "type": "redirect", "http_status_code": 302, "redirect_url": "https://expserver.github.io" }
      ]
    },
    {
      "listeners": [{ "host": "0.0.0.0", "port": 8004 }]
    }
  ]
}
```

#### 3. Core Structures (`src/config/xps_config.h`)
```c
typedef enum xps_req_type_e {
    REQ_FILE_SERVE,
    REQ_REVERSE_PROXY,
    REQ_REDIRECT,
    REQ_METRICS,
    REQ_INVALID
} xps_req_type_t;

struct xps_config_lookup_s {
    xps_req_type_t type;
    char *file_path;            // Absolute resolved path for file serving
    char *dir_path;
    const char *upstream;       // "localhost:3000" for reverse proxy
    u_int http_status_code;     // 302
    const char *redirect_url;   // "http://localhost:8002/"
};
```

#### 4. The Route Lookup Engine (`xps_config_lookup`)
When an HTTP request arrives on a client connection:
1. It matches the client's destination port against all `listeners` in the configuration.
2. It matches the HTTP `Host` header against `hostnames`.
3. It performs **longest prefix matching** between the request URI path (e.g., `/redirect` or `/cat.jpg`) and configured route `req_path` rules.
4. Returns an `xps_config_lookup_t` object instructing `session_process_request()` exactly what to do.

---

## 3. Detailed Dataflow for Each Stage 16 Route

### Route 1: File Serving (`REQ_FILE_SERVE` on port 8001)
```
1. Client sends: GET /cat.jpg HTTP/1.1
2. Lookup resolves: REQ_FILE_SERVE -> "../public/cat.jpg"
3. Security Checks:
   - realpath() canonicalization against "../public" (blocks directory traversal attacks)
   - stat() check for other-read permissions (S_IROTH)
4. Open file: xps_file_create() creates file->source.
5. Send Response Headers:
   - 200 OK
   - Content-Length: <file_size>
   - Content-Type: image/jpeg
   Serialized into session->to_client_buff.
6. Attach Pipe:
   file->source =====[ Pipe ]=====> session->file_sink
7. As file chunks are read from disk:
   file_sink -> to_client_buff -> client_source =====[ Pipe ]=====> client->sink -> socket send()
```

### Route 2: HTTP Redirection (`REQ_REDIRECT` on port 8001 & 8003)
```
1. Client sends: GET /redirect HTTP/1.1
2. Lookup resolves: REQ_REDIRECT -> 302 -> "http://localhost:8002/"
3. Create Response:
   xps_http_res_create(core, 302)
   xps_http_set_header(headers, "Location", "http://localhost:8002/")
4. Serialized into session->to_client_buff and sent to client sink.
```

### Route 3: HTTP Reverse Proxy (`REQ_REVERSE_PROXY` on port 8002)
```
1. Client sends: GET /cat.jpg HTTP/1.1 to port 8002
2. Lookup resolves: REQ_REVERSE_PROXY -> Upstream "localhost:3000"
3. Asynchronous Connection:
   xps_upstream_create(core, "localhost", 3000)
4. Cross-Connect Bidirectional Pipes:
   Client -> Upstream: session->upstream_source ===[ Pipe ]===> upstream->sink
   Upstream -> Client: upstream->source ===[ Pipe ]===> session->upstream_sink
5. Client HTTP request is piped to python server on port 3000.
6. Upstream python server responds with HTTP response; piped directly back to the client socket!
```

---

## 4. Top Viva Questions & In-Depth Technical Answers

### Section A: Architectural & System Calls

#### Q1: What is the event loop mechanism in eXpServer, and why do we use `epoll` instead of threads?
> **Answer:**
> eXpServer uses a **single-threaded, non-blocking, event-driven architecture based on Linux `epoll`**.
> - In a thread-per-connection model, $10{,}000$ concurrent connections require $10{,}000$ OS threads, causing massive memory overhead (each thread stack consumes megabytes) and catastrophic performance degradation due to CPU context switching and lock contention.
> - `epoll` maintains a kernel-level ready list via internal callbacks. It monitors thousands of socket file descriptors with $O(1)$ algorithmic complexity, using negligible memory and zero thread-synchronization primitives.

#### Q2: What is the significance of Edge-Triggered (`EPOLLET`) mode, and what rule must our socket I/O code follow?
> **Answer:**
> In Level-Triggered mode, `epoll_wait` continuously fires as long as data remains in the kernel socket buffer. In **Edge-Triggered (`EPOLLET`) mode**, `epoll_wait` notifies the application **only when the state changes** (e.g., when new data arrives or the socket buffer transitions from full to writable).
> - **The Fundamental Rule:** Sockets must be non-blocking (`O_NONBLOCK`), and all `recv()` and `send()` operations **must loop until they return `EAGAIN` or `EWOULDBLOCK`**. Failing to drain the buffer completely causes the event loop to stop receiving notifications, hanging the connection (starvation).

#### Q3: Why does eXpServer use an abstract "Pipe" layer (`xps_pipe_t`) instead of directly copying socket-to-socket?
> **Answer:**
> 1. **Decoupling Producers & Consumers:** The pipe abstraction standardizes data flow. Sockets, disk files, memory buffers, and HTTP parsers all implement the same `source` and `sink` callbacks. We can connect any source to any sink without custom gluing logic.
> 2. **Backpressure (Flow Control):** If a file source reads from a fast SSD at $500\text{ MB/s}$ but the client is on a slow $3\text{G}$ mobile connection consuming at $100\text{ KB/s}$, directly reading the file would blow up server memory. The pipe enforces a threshold (`DEFAULT_PIPE_BUFF_THRESH = 1MB`). When the pipe buffer exceeds the threshold, `xps_pipe_is_writable()` returns false, stopping the file read until the client socket drains the buffer.

---

### Section B: HTTP Protocols (Stages 14 & 15)

#### Q4: Why must HTTP headers strictly use `\r\n` (CRLF) and not just `\n` (LF)?
> **Answer:**
> **RFC 7230 (HTTP/1.1 Specification, Section 3)** explicitly defines HTTP line termination as CRLF (`\r\n`, ASCII values `13` and `10`):
> $$\text{HTTP-message} = \text{start-line} \; \text{CRLF} \; *(\text{header-field} \; \text{CRLF}) \; \text{CRLF} \; [\text{message-body}]$$
> While simple tools like `nc` or old versions of `curl` might accept bare `\n`, modern HTTP parser libraries (such as Node.js `undici`, Nginx, and modern browsers) strictly enforce the RFC. Sending bare `\n` triggers `HPE_INVALID_HEADER_TOKEN`, causing the client to drop the connection immediately with an undefined error.

#### Q5: How does `xps_http_req_create` handle incomplete HTTP requests that arrive across multiple TCP segments?
> **Answer:**
> TCP provides a streaming byte abstraction without packet boundaries. An HTTP request header may be split across multiple TCP segments.
> When `client_sink_handler` passes data to `xps_http_req_create()`, the parser scans for the terminating double CRLF sequence (`\r\n\r\n`). If it has not arrived yet, the function returns `E_AGAIN`. The session preserves the partial data in the pipe and yields control back to `epoll_wait` to wait for subsequent `EPOLLIN` events.

#### Q6: Why is the `Host` header mandatory in HTTP/1.1, and what status code must be returned if it is missing?
> **Answer:**
> In HTTP/1.0, web servers could only host one website per IP address. HTTP/1.1 introduced **virtual hosting**, allowing multiple domain names to share a single server IP. The `Host` header tells the server which virtual host the client is requesting.
> RFC 7230 mandates that any HTTP/1.1 request without a `Host` header **must be rejected with `400 Bad Request`**.

---

### Section C: Stage 16 Routing, Security & Critical Bugs Solved

#### Q7: How does Stage 16 protect against Directory Traversal Attacks?
> **Answer:**
> If a malicious client sends `GET /../../../../etc/passwd HTTP/1.1`, naive string concatenation (`../public/../../../../etc/passwd`) would expose private system files.
> In `xps_file_create()`, we defend against this using `realpath()`:
> ```c
> char *resolved_path = realpath(file_path, NULL);
> char *resolved_public = realpath("../public", NULL);
> ```
> `realpath()` canonicalizes the path by resolving all symlinks, extra slashes, and `.`/`..` components. We then perform a string prefix comparison (`strncmp`). If `resolved_path` does not begin with `resolved_public`, the request is immediately aborted with `HTTP_FORBIDDEN` (`403`).

#### Q8: Explain the critical bug where `cat.jpg` (156 KB) truncated at 146 KB, and how you fixed it.
> **Answer:**
> **The Root Cause:**
> 1. In `xps_file.c`, when the file reached EOF on disk, `xps_file_destroy()` was called, detaching `file->source`.
> 2. `file_sink` drained the last bytes into `session->to_client_buff`. Because `file_sink` was no longer active, `session_check_destroy()` ran and determined that all session flows were complete.
> 3. The session was destroyed, detaching `session->client_sink` from `connection->source`.
> 4. In `xps_connection.c`, `connection_source_close_handler` checked:
>    ```c
>    if (!source->active && ...) connection_close(connection, false);
>    ```
>    Because `source->active` became false, it immediately closed the client TCP socket!
> 5. However, `connection->sink` was still writing the last $9.6\text{ KB}$ of `cat.jpg` to the network! Closing the socket caused an immediate TCP RST/FIN, dropping the remaining bytes and throwing `SocketError: other side closed` (`UND_ERR_SOCKET`).
>
> **The Fix:**
> We updated both `connection_source_close_handler` and `connection_sink_close_handler` to enforce **two-way flow termination**:
> ```c
> if (!source->active && !connection->sink->active) {
>     connection_close(connection, false);
> }
> ```
> The TCP connection is kept alive until `connection->sink` has completely flushed all buffered response bytes to the client socket.

#### Q9: Explain the event loop logic bug in `handle_pipes()` in `xps_loop.c`.
> **Answer:**
> In `xps_loop.c`, `handle_pipes()` had the following line:
> ```c
> if (pipe->sink && pipe->source == NULL && !xps_pipe_is_readable(pipe)) return true;
> ```
> Because of the mistaken `!` (NOT operator), when a source reached EOF and detached (`pipe->source == NULL`), but the pipe buffer still contained unread data waiting to be sent to the socket (`xps_pipe_is_readable(pipe) == true`), `handle_pipes()` returned `false`.
> This caused `epoll_wait` to enter a blocking sleep (`timeout = -1`) instead of executing an immediate non-blocking poll (`timeout = 0`) to drain the pipe, resulting in connection stalls and timeouts. Removing the `!` fixed the condition to `xps_pipe_is_readable(pipe)`.

#### Q10: How does the server handle GLIBC compatibility issues when deployed in Docker test environments?
> **Answer:**
> Compiling C programs on modern host distributions (like Fedora with GLIBC 2.38+) introduces symbol versions (`GLIBC_2.38`) that do not exist in standard Docker test containers (such as Debian Bookworm, which uses GLIBC 2.36). Running a host-compiled binary inside the container produces loader errors (`version GLIBC_2.38 not found`).
> To guarantee 100% ABI compatibility, we created `build_docker.sh` scripts that invoke `gcc` inside the target Docker container environment (`expserver-tester-image`), producing binaries targeting the container's exact GLIBC runtime.

---

## 5. Rapid Revision Summary Table

| Milestone / Concept | Stage | Primary Source Files | Key Mechanism |
| :--- | :---: | :--- | :--- |
| **HTTP Request Parsing** | **14** | `http/xps_http_req.c`<br>`http/xps_http.c` | State machine parsing; parses request-line & headers; enforces `Host:` header and returns `E_AGAIN` on partial reads. |
| **HTTP Response Formatting**| **15** | `http/xps_http_res.c` | Encapsulates status line + reason phrase; dynamic header list; strictly enforces CRLF (`\r\n`) wire format. |
| **JSON Configuration** | **16** | `config/xps_config.c`<br>`lib/parson/parson.c`| Parson parser deserializes `xps_config.json`; binds dynamic ports 8001–8004 to listeners and routes. |
| **Prefix Route Lookup** | **16** | `config/xps_config.c` | `xps_config_lookup()` matches port + hostname + longest URI path prefix to select route handler. |
| **File Server Security** | **16** | `disk/xps_file.c` | `realpath()` canonical path comparison prevents `../` path traversal; checks `S_IROTH` read permissions via `stat()`. |
| **Reverse Proxy Bridging** | **16** | `core/xps_session.c`<br>`network/xps_upstream.c` | Asynchronously connects to upstream (port 3000); cross-connects bidirectional client $\leftrightarrow$ upstream pipes. |
| **Safe Socket Teardown** | **16** | `network/xps_connection.c` | Dual-active check (`!source->active && !sink->active`) prevents socket closure while responses are still draining. |

---

## 6. How to Give a Confident Viva Presentation

When the examiner asks you to explain what you did from Stage 14 to Stage 16, follow this 4-step structure:

1. **Start with the Motivation:**
   > *"In Stage 13, our server was a raw event-driven TCP proxy. In Phase 2, we brought in the HTTP application layer. In Stage 14, we built a non-blocking HTTP request parser using a state machine. In Stage 15, we built a structured HTTP response serializer conforming strictly to RFC CRLF requirements. In Stage 16, we made the server fully configurable via JSON using Parson, adding dynamic file serving, HTTP 302 redirects, and reverse proxying."*
2. **Show the Code Structure:**
   - Point to [xps_http_req.h](file:///home/ldzbeta/Documents/eXpServer-Networks-Lab-NITC/current_full_version/src/http/xps_http_req.h) and [xps_http_res.h](file:///home/ldzbeta/Documents/eXpServer-Networks-Lab-NITC/current_full_version/src/http/xps_http_res.h).
   - Point to [xps_config.c](file:///home/ldzbeta/Documents/eXpServer-Networks-Lab-NITC/current_full_version/src/config/xps_config.c) to show how JSON routes are matched.
   - Point to [xps_session.c](file:///home/ldzbeta/Documents/eXpServer-Networks-Lab-NITC/current_full_version/src/core/xps_session.c#L145) (`session_process_request`) to show the clean `switch/if` handling `REQ_FILE_SERVE`, `REQ_REDIRECT`, and `REQ_REVERSE_PROXY`.
3. **Highlight Key Security & Systems Engineering Choices:**
   - Explain how you prevented directory traversal with `realpath()`.
   - Explain how non-blocking edge-triggered epoll avoids thread overhead.
4. **Demonstrate Deep Debugging Knowledge:**
   - Mention the CRLF requirement in HTTP/1.1 and how strict parsers like Node.js `undici` fail if `\n` is used instead of `\r\n`.
   - Mention how you solved the large file truncation issue ($146\text{ KB}$ vs $156\text{ KB}$) by synchronizing connection teardown until both source and sink pipes are fully flushed.
