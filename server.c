#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <direct.h>  // for _mkdir


// Create directories if they don't exist
void ensure_directories() {
    // static
    if (_mkdir("static") == -1) {
        // -1 means already exists or error
        // You can ignore if exists
    }

    // static/upload
    if (_mkdir("static/upload") == -1) {
        // ignore if exists
    }

    printf("Ensured directories: static/, static/upload/\n");
}

// #pragma comment(lib, "ws2_32.lib")

#define PORT 8086
#define BUFFER_SIZE 65535

void send404(SOCKET client)
{
    const char *response =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 57\r\n"
        "\r\n"
        "<html><body><h1>404 Not Found</h1></body></html>";
    send(client, response, (int)strlen(response), 0);
}

void *memmem(const void *haystack, size_t haystack_len,
             const void *needle, size_t needle_len)
{
    if (needle_len == 0)
        return (void *)haystack;

    const char *h = haystack;
    const char *n = needle;

    for (size_t i = 0; i + needle_len <= haystack_len; i++)
    {
        if (memcmp(h + i, n, needle_len) == 0)
            return (void *)(h + i);
    }

    return NULL;
}

// ------------------ MIME TYPES ------------------
const char *get_mime(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";

    if (!_stricmp(ext, ".html"))
        return "text/html";
    if (!_stricmp(ext, ".css"))
        return "text/css";
    if (!_stricmp(ext, ".js"))
        return "application/javascript";
    if (!_stricmp(ext, ".png"))
        return "image/png";
    if (!_stricmp(ext, ".jpg"))
        return "image/jpeg";
    if (!_stricmp(ext, ".jpeg"))
        return "image/jpeg";
    if (!_stricmp(ext, ".ico"))
        return "image/x-icon";
    if (!_stricmp(ext, ".svg"))
        return "image/svg+xml";
    return "application/octet-stream";
}

// ------------------ SEND STATIC FILE ------------------
void send_file(SOCKET client, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        send404(client);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    const char *mime = get_mime(path);

    char header[256];
    sprintf(header,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n\r\n",
            mime, size);

    send(client, header, strlen(header), 0);

    char buf[1024];
    long n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        send(client, buf, n, 0);
    }

    fclose(fp);
}

// Structure to store info about saved files
typedef struct {
    char field[128];
    char originalName[256];
    char savedName[256];
} SavedFile;

void handle_post_upload(SOCKET client, char *buffer, int received) {
    int contentLength = 0;
    char boundary[256] = {0};
    char full_boundary[300] = {0};
    printf("\n--- START POST /upload ---\n");

    // 1. Extract Content-Length
    char *cl = strstr(buffer, "Content-Length:");
    if (!cl) { 
        printf("[DEBUG] Error: Content-Length header not found.\n");
        send404(client); 
        closesocket(client); 
        return; 
    }
    sscanf(cl, "Content-Length: %d", &contentLength);
    printf("[DEBUG] Content-Length = %d\n", contentLength);

    // 2. Extract Boundary
    char *bpos = strstr(buffer, "boundary=");
    if (!bpos) { 
        printf("[DEBUG] Error: Boundary not found.\n");
        send404(client); 
        closesocket(client); 
        return; 
    }
    bpos += 9;
    char *b_end = strpbrk(bpos, "\r\n");
    int blen = (int)(b_end - bpos);
    strncpy(boundary, bpos, blen);
    boundary[blen] = 0;

    sprintf(full_boundary, "--%s", boundary);
    int full_boundary_len = (int)strlen(full_boundary);
    printf("[DEBUG] Boundary = %s\n", full_boundary);

    // Prepare JSON output
    char json[8192];
    strcpy(json, "{\"success\":true,\"files\":[");
    int jsonCount = 0;

    // 3. Find start of body in initial buffer
    char *bodyStart = strstr(buffer, "\r\n\r\n");
    if (!bodyStart) { 
        printf("[DEBUG] Error: Cannot find end of headers.\n");
        send404(client); 
        closesocket(client); 
        return; 
    }
    bodyStart += 4;

    int initialBodyBytes = received - (bodyStart - buffer);
    int totalRead = initialBodyBytes;
    printf("[DEBUG] Initial body bytes in first recv: %d\n", initialBodyBytes);

    // 4. Streaming upload setup
    #define UPLOAD_CHUNK 65536
    char streamBuf[UPLOAD_CHUNK + 1024];
    int leftover = 0;
    FILE *current_file = NULL;
    char filename[256] = {0};
    char savedName[256];

    if (initialBodyBytes > 0) {
        memcpy(streamBuf, bodyStart, initialBodyBytes);
        leftover = initialBodyBytes;
    }

    int remaining = contentLength - initialBodyBytes;

    while (remaining > 0 || leftover > 0) {
        if (remaining > 0 && leftover < UPLOAD_CHUNK) {
            int toRead = (remaining < (UPLOAD_CHUNK - leftover)) ? remaining : (UPLOAD_CHUNK - leftover);
            int r = recv(client, streamBuf + leftover, toRead, 0);
            if (r <= 0) {
                printf("[DEBUG] recv error or connection closed.\n");
                break;
            }
            leftover += r;
            remaining -= r;
            totalRead += r;
            // printf("[DEBUG] Total bytes read so far: %d\n", totalRead);
        }

        char *b = memmem(streamBuf, leftover, full_boundary, full_boundary_len);

        if (b) {
            int preBoundaryLen = (int)(b - streamBuf);

            if (current_file) {
                fwrite(streamBuf, 1, preBoundaryLen - 2, current_file);
                fclose(current_file);
                printf("[DEBUG] Saved file: %s\n", filename);
                current_file = NULL;
            }

            char *hstart = b + full_boundary_len + 2; 
            char *hend = memmem(hstart, leftover - (hstart - streamBuf), "\r\n\r\n", 4);
            if (!hend) break;

            memset(filename, 0, sizeof(filename));
            char *fnStart = memmem(hstart, hend - hstart, "filename=\"", 10);
            if (fnStart) {
                fnStart += 10;
                char *fnEnd = memchr(fnStart, '"', hend - fnStart);
                if (fnEnd) {
                    int fnLen = (int)(fnEnd - fnStart);
                    memcpy(filename, fnStart, fnLen);
                    filename[fnLen] = 0;

                    // Build savedName using timestamp
                    time_t t = time(NULL);
                    char *ext = strrchr(filename, '.');
                    char base[256];
                    if (ext) {
                        strncpy(base, filename, ext - filename);
                        base[ext - filename] = 0;
                        sprintf(savedName, "%s_%ld%s", base, t, ext);
                    } else {
                        sprintf(savedName, "%s_%ld", filename, t);
                    }
                    printf("[DEBUG] Next file to save: %s\n", savedName);

                    char path[512];
                    sprintf(path, "static/upload/%s", savedName);
                    current_file = fopen(path, "wb");
                    if (!current_file) {
                        fprintf(stderr, "[DEBUG] Failed to open file %s\n", path);
                    }
                    
                    // Append to JSON
                    if (jsonCount++ > 0) strcat(json, ",");
                    char fileEntry[512];
                    sprintf(fileEntry, "{\"originalName\":\"%s\",\"savedName\":\"%s\",\"url\":\"/static/upload/%s\"}", filename, savedName, savedName);
                    strcat(json, fileEntry);


                }
            }

            int newLeftover = leftover - (hend + 4 - streamBuf);
            memmove(streamBuf, hend + 4, newLeftover);
            leftover = newLeftover;
        } else {
            int writeLen = leftover - full_boundary_len - 4;
            if (writeLen > 0 && current_file) {
                fwrite(streamBuf, 1, writeLen, current_file);
            }
            memmove(streamBuf, streamBuf + writeLen, leftover - writeLen);
            leftover -= writeLen;
        }
    }

    if (current_file) fclose(current_file);

    printf("[DEBUG] POST /upload finished\n");

    strcat(json, "]}");

    // Send JSON response
    char header[256];
    sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n", (int)strlen(json));
    send(client, header, strlen(header), 0);
    send(client, json, strlen(json), 0);

}


int main()
{
    WSADATA wsa;
    SOCKET server, client;
    struct sockaddr_in addr;
    char buffer[BUFFER_SIZE];

    WSAStartup(MAKEWORD(2, 2), &wsa);

    server = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    // addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0"); // your PC IP

    bind(server, (struct sockaddr *)&addr, sizeof(addr));
    listen(server, 5);

    ensure_directories();

    printf("Server running on http://localhost:%d\n", PORT);

    while (1)
    {
        client = accept(server, NULL, NULL);
        int received = recv(client, buffer, BUFFER_SIZE - 1, 0);
        buffer[received] = 0;

        if (received == -1)
        {
            closesocket(client);
            continue;
        }

        // --- Handle GET requests (existing logic) ---
        if (strncmp(buffer, "GET /favicon.ico", 16) == 0)
        {
            send_file(client, "static/favicon.ico");
        }
        else if (strncmp(buffer, "GET / ", 6) == 0 || strncmp(buffer, "GET /index", 10) == 0)
        {
            send_file(client, "index.html");
        }
        else if (strncmp(buffer, "GET /static/", 12) == 0)
        {
            char path[300];
            sscanf(buffer, "%*s /%s", path);
            send_file(client, path);
        }

        // --- Handle POST requests with large files ---
        else if (strncmp(buffer, "POST /upload", 12) == 0){
            handle_post_upload(client, buffer, received);
        }
        else
        {
            send404(client);
        }
        closesocket(client);
    }

    WSACleanup();
    return 0;
}
