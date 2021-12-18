/*
 *  This file is for use by students to define anything they wish.  It is used by both proxy server implementation
 */
#ifndef __SERVER_STUDENT_H__
#define __SERVER_STUDENT_H__

#include "steque.h"

// Define a struct for accepting LibCurl's output
typedef struct curlcontext_t{
    gfcontext_t * ctx;
    char * buffer;
    size_t bytes_received;
}curlcontext_t;

// This is the function we pass to LibCurl: writes the output to a FileStruct
static  size_t WriteMemoryCallback(void *, size_t, size_t, void *);

#endif // __SERVER_STUDENT_H__