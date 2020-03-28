#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <pthread.h>
/* header for socket */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
/* header for video */
#include <linux/videodev2.h>
#include <v4l2-common.h>
#include <v4l2-controls.h>
/* data structure for thread */
typedef struct { key_t frameid, servoid; } msgid;
typedef struct { int servfd, clnfd; } sockid;
typedef struct { int v4l2_fd, fb_fd; } devid;
typedef struct { pthread_t *tid; sockid skid; msgid mid; devid did;} trxid;
typedef struct { void *start; int size;} framemsg;
typedef struct { char cmd; } servomsg;
pthread_mutex_t common_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t camera_cond = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t trx_cond = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t servo_cond = PTHREAD_MUTEX_INITIALIZER;
/* data structure for devices */
void *frame_start;
int camfd, servofd;
struct v4l2_capability cap;
struct v4l2_format format;
struct v4l2_requestbuffers bufrequest;
struct v4l2_buffer bufferinfo;
/* macro for miscellaneous */
#define LOCALPORT 5050
#define FRAMESIZE 153600
#define VIDEODEV "/dev/video0"
#define FBDEV	 "/dev/fb0"
#define WIDTH 320
#define HEIGHT 240

msgid messagecreation(void)
{
    key_t frameid, servoid;
    frameid = msgget((key_t)1234, IPC_CREAT | 0666);
    servoid = msgget((key_t)4567, IPC_CREAT | 0666);
    msgid ret = { frameid, servoid };
    return ret;
}

sockid socketcreation(void)
{
    sockaddr_in serv_addr, clnt_addr;
    int servid, clntid; socklen_t clnt_len = sizoef(clnt_addr);
    if((servid = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket error\n");
        exit(1);
    }
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(LOCALPORT);

    if(bind(servid, &serv_addr, sizeof(serv_addr)) == -1) {
        perror("2. bind error\n");
        exit(1);
    }

    if(listen(servid, 5) == -1) {
        perror("3. listen error\n");
        exit(1);
    }

    if((clntid = accept(servid, &clnt_addr, &clnt_len)) == -1){
        perror("4. accept error\n");
        exit(1);
    }
}

void *transmission_thread(trxid *tk)
{
    pthread_cond_wait(&trx_cond, &common_mutex);
    int clntid = tk->skid.clnfd;
    key_t frameid = tk->mid.frameid, servoid = tk->mid.servoid;
    framemsg recvmsg; servomsg sendmsg;
    while(1) {
        int recvcnt = 0;
        if(msgrcv(frameid, &recvmsg, sizeof(framemsg), 0, MSG_NOERROR) == -1)
            perror("message isn't enqueued in frame message queue\n");
        if(send(clntid, recvmsg.start, recvmsg.size, 0) == -1)
            perror("transmit dma buffer to control tower error\n");
        recvcnt = recv(clntid, &sendmsg.cmd, sizeof(char), MSG_DONTWAIT);
        if(!recvcnt) {
            pthread_cond_signal(&camera_cond);
            pthread_cond_wait(&trx_cond, &common_mutex);
        }
        else {
            if (sendmsg.cmd == 'q' || sendmsg.cmd == 'Q') termination(tk->tid, &(tk->skid), &tk->mid, &(tk->did));
            else {
                msgsnd(servoid, &sendmsg, sizeof(servomsg), IPC_NOWAIT);
                pthread_cond_signal(&servo_cond);
                pthread_cond_wait(&trx_cond, &common_mutex);
            }
        }
    }
    
}

void *camera_thread(msgid *mk)
{
    pthread_mutex_lock(&common_mutex);
    int frameid = mk->frameid;
    /* define frame format */
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    format.fmt.pix.width = WIDTH;
    format.fmt.pix.height = HEIGHT;
    /* inform dma buffer to device */
    bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufrequest.memory = V4L2_MEMORY_MMAP;
    bufrequest.count = FRAMESIZE;
    /* specify dma buffer to allocate in kernel memory */
    memset(&bufferinfo, 0, sizeof(bufferinfo));
    bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufferinfo.memory = V4L2_MEMORY_MMAP;
    bufferinfo.index = 0;

    /* 1. open v4l2 device driver */
    if((camfd = open(VIDEODEV, O_RDWR)) == - 1){
        perror("open camera error\n");
        exit(1);
    }
    /* 2. check whether it is available to work in this system or not -> ioctl query flag */
    if(ioctl(camfd, VIDIOC_QUERYCAP, &cap) == -1){
        perror("not applied in this system\n");
        exit(1);
    }
    
    if(!(cap.capabilities &  V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "doesn't handle single-planar video streaming\n");
        exit(1);
    }

    if(ioctl(camfd, VIDIOC_S_FMT, &format) == -1) {
        perror("format not applied\n");
        exit(1);
    }
    /* 3. allocate dma buffer to kernel memory region */
    if(ioctl(camfd, VIDIOC_REQBUFS, &bufrequest) == -1) {
        perror("failed to notify buffer information to device\n");
        exit(1);
    }

    if(ioctl(camfd, VIDIOC_QUERYBUF, &bufferinfo) == -1) {
        perror('failed to allocate buffer in kernel memory\n');
        exit(1);
    }
    /* 4. mmap dma buffer to this thread */
    frame_start = mmap(NULL, bufferinfo.length, PROT_READ | PROT_WRITE, MAP_SHARED, camfd, bufferinfo.m.offset);
    if (frame_start == MAP_FAILED) {
        perror("mmapping dma buffer to application memory region failed\n");
        exit(1);
    }
    memset(frame_start, 0, bufferinfo.length);
    /* 5. activating streaming */
    if (ioctl(camfd, VIDIOC_STREAMON, &bufferinfo.type) == -1) {
        perror("activation failed\n");
        exit(1);
    }
    while(1) {
        /* 1. read frame from device to dma buffer */
        int readcnt = 0;
        while(readcnt < FRAMESIZE){
            int getdata = 0;
            if (ioctl(camfd, VIDIOC_QBUF, &bufferinfo) == -1) 
                perror("failed to request new frame from device\n");
            bufferinfo.index = readcnt;
            getdata = ioctl(camfd, VIDIOC_DQBUF, &bufferinfo);
            if(getdata > 0) readcnt += getdata; 
            else if (getdata == -1) 
                perror("failed to read frame data from device to dma buffer\n");
            
        }
        /* 3. send pointer and frame size to msg queue */
        framemsg sendmsg = { frame_start, FRAMESIZE };
        if(msgsnd(frameid, &sendmsg, sizeof(sendmsg), 0) != sizeof(sendmsg))
            perror("not properly sended from camera to message queue\n");
        memset(frame_start, 0, FRAMESIZE);
        pthread_cond_signal(&trx_cond);
        pthread_cond_wait(&camera_cond, &common_mutex);
    }
}

void *servo_thread(msgid *mk)
{
    /* 0. mutex lock */
    /* 1. open motor device driver */
    /* 2. check whether it is connected to raspi */
    /* 3. mutex unlock */
    while(1) {
        /* 0. lock common mutex */
        /* 1. read msg queue and figure out whether recving msg or not */
        /* 1-1. wake up camera thread and will be in sleep after command to motor by ioctl() */
        /* 1-2. wake up camera camera thread and will be in sleep immediately */
    }
}

void pthreadcreation(pthread_t *tid, sockid *sk, msgid *mk)
{
    /* we need to open device about camera and motor */
    trxid tk = {tid, sk, mk};
    pthread_create(&tid[0], NULL, transmission_thread, &tk);
    pthread_create(&tid[1], NULL, camera_thread, mk);
    pthread_create(&tid[2], NULL, servo_thread, mk);
}

void termination(pthread_t *tid, sockid *sk, msgid *mk, devid *dk)
{
    int size = sizeof(key_t);
    /* deactivation device for image processing */
    ioctl(camfd, VIDIOC_STREAMOFF, &bufferinfo.type);
    mmunmap(frame_start, FRAMESIZE); close(camfd);
    /* deactivation device for servo motor */
    close(servofd);
    /* need to close all device */
    for(int i = 0, j = 0; i < 2; i++, j += size) msgctl(*(key_t *)(mk + j), IPC_RMID, 0);
    for(int i = 0; i < 3; i++) pthread_cancle(tid[i]);
    for(int i = 0, j = 0; i < 2; i++, j += 4) close(*(int *)(sk + j));
}

int main(int argc, char **argv)
{
    pthread_t tid[3];
    sockid sk = socketcreation();
    msgid mk = messagecreation();
    pthreadcreation(tid, &sk, &mk);
    return 0;

}