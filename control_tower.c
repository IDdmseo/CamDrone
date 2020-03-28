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
#include <linux/fb.h>