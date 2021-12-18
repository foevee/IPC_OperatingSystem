

#include "gfserver.h"
#include "proxy-student.h"

#define BUFSIZE (128)


/*
 * Replace with an implementation of handle_with_curl and any other
 * functions you may need.
 */
ssize_t handle_with_curl(gfcontext_t *ctx, const char *path, void* arg){


    char url[BUFSIZE];
    double file_length;
    long response_code;
    ssize_t bytes_sent = 0, nsend = 0;
    CURL * curl_client;
    CURLcode get_result;
    curlcontext_t curl_ctx;

    //init
    curl_ctx.buffer = NULL;
    curl_ctx.bytes_received = 0;
    curl_ctx.ctx = ctx;

    //notice arg is the server path as defined in webproxy.c
    strcpy(url, arg);
    strcat(url, path); //here file path already starts with "/"
    printf("The requested url is %s\n", url);


    curl_client = curl_easy_init();
    if(curl_client) {
        curl_easy_setopt(curl_client, CURLOPT_WRITEFUNCTION, WriteMemoryCallback); // Passing the function pointer to LC
        curl_easy_setopt(curl_client, CURLOPT_WRITEDATA, (void *)&curl_ctx); // Passing our BufferStruct to LC
        curl_easy_setopt(curl_client, CURLOPT_URL, url);
        get_result = curl_easy_perform(curl_client);

    }

    if (get_result != CURLE_OK) {
        printf("Curl Error Code %d from %s\n", (int)get_result, curl_easy_strerror(get_result));
        curl_easy_cleanup(curl_client);
        free(curl_ctx.buffer);
        return SERVER_FAILURE;
    }

    curl_easy_getinfo(curl_client, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &file_length);
    curl_easy_getinfo(curl_client, CURLINFO_RESPONSE_CODE, &response_code);

    printf("Processed curl result: file content len %f, response code %ld\n", file_length, response_code);

    /* Error Handling */
    if(response_code == 404) return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
    else if(response_code >= 500) return gfs_sendheader(ctx, GF_ERROR, 0);

    /* Normal Send */
    gfs_sendheader(ctx, GF_OK, file_length);
    //send body
    while(bytes_sent < curl_ctx.bytes_received){
        nsend = gfs_send(ctx, curl_ctx.buffer, curl_ctx.bytes_received);
        if(nsend != curl_ctx.bytes_received){
            printf("error sending insufficient content\n");
            curl_easy_cleanup(curl_client);
            free(curl_ctx.buffer);
            return SERVER_FAILURE;
        }
        bytes_sent += nsend;
    }

    printf("Finished sending %s, size %d bytes\n", path, (int)bytes_sent);

	//final clean up
    curl_easy_cleanup(curl_client);
    free(curl_ctx.buffer);

    return bytes_sent;
}


/*
 * We provide a dummy version of handle_with_file that invokes handle_with_curl
 * as a convenience for linking.  We recommend you simply modify the proxy to
 * call handle_with_curl directly.
 */
ssize_t handle_with_file(gfcontext_t *ctx, const char *path, void* arg){
	return handle_with_curl(ctx, path, arg);
}


static  size_t WriteMemoryCallback(void *buffer, size_t size, size_t nmemb, void * ctx){
    size_t nbytes = size * nmemb;
    struct curlcontext_t * curl_ctx = (struct curlcontext_t*) ctx;
    curl_ctx->buffer = realloc(curl_ctx->buffer, curl_ctx->bytes_received + nbytes + 1);
    if(curl_ctx->buffer){
        memcpy(&(curl_ctx->buffer[curl_ctx->bytes_received]), buffer, nbytes);
        curl_ctx->bytes_received += nbytes;
        curl_ctx->buffer[curl_ctx->bytes_received] = 0;
    }
    return nbytes;
}