/*
 *  This file is for use by students to define anything they wish.  It is used by the proxy cache implementation
 */
#ifndef __CACHE_STUDENT_H__
#define __CACHE_STUDENT_H__

#include <semaphore.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h> // For O_ constants
#include <mqueue.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include "gfserver.h"
#include "steque.h"

#define SHM_NAME "SHM_"
#define MQ_REQUEST_NAME "/RequestMQ"
#define MAX_SHMNAME_LEN 32
#define MAX_PATH_LEN 256
#define MAX_MSG_NUM 10
#define MAX_MSG_SIZE 1024



steque_t *proxy_queue;
steque_t *cache_queue;


typedef struct {
    char filePath[MAX_PATH_LEN];
    char shmName[MAX_SHMNAME_LEN];
    size_t nSegments;
    size_t segmentSize;
}MSQRequest_t;

typedef struct {
    pthread_cond_t cond;
    pthread_mutex_t mutex;
} lock_t;
lock_t *proxy_lock, *cache_lock;

typedef struct {
    size_t nSegments;
    size_t segmentSize;
    mqd_t mqRequest;
}ContextWebProxy_t;
ContextWebProxy_t g_webProxy;

typedef struct {
    char filePath[MAX_PATH_LEN];
    size_t fileLen;
    size_t dataLen;
    gfstatus_t status;
    sem_t semREAD;
    sem_t semWRITE;
}ContextShm_t;

typedef struct {
    char shm_name[MAX_SHMNAME_LEN];
    ContextShm_t * shm_context;
}ContextProxy_t;


typedef struct  {
  pthread_t pthread;
  bool isEnabled; // Used to kill threads and exit safe.
}threadInfo_t;


void *cache_worker(void *arg);



#endif // __CACHE_STUDENT_H__
