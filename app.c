#include<stdio.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<unistd.h>
#include<signal.h>
#include <sys/ioctl.h>
extern int errno;
#define SIG_TEST 44
#define MAGIC_NO 100
#define SET_PID_CMD	_IOW(MAGIC_NO, 1, int) 
char read_buffer[20];
int fd;
int length;
void signalFunction(int n, siginfo_t *info, void *unused) {
}
void handleFunction(int sig){
    int i=0;
    if(read(fd,read_buffer,length) < 0){
            printf("Got error: %s\n",strerror(errno));
            return;
    }
    for(i=0;i<length;i++){
        printf("%c",read_buffer[i]);
    }
    printf("\n");
}
int main(void){
    fd = open("/dev/device_axidma",O_RDWR);
    if(fd < 0){
        printf("Got error: %s\n",strerror(errno));
        return -1;
    }
    int pid = getpid();
    int ret = 0;
    char buffer[20]; 
    if(ioctl(fd,SET_PID_CMD,&pid)<0){
        close(fd);        
        printf("Got error: %s\n",strerror(errno));
        return -1;
    } 
    
    /* struct sigaction sig;
    sig.sa_sigaction = signalFunction; // Callback functions
    sig.sa_flags = SA_SIGINFO;
    sigaction(SIG_TEST, &sig, NULL); */
    signal(SIG_TEST,handleFunction);
    memset(buffer,0,20);
    while(1){  
        memset(buffer,0,20);
        printf("Typing !\n");
        scanf("%s",buffer);
        length = strlen(buffer);
        ret = write(fd,buffer,length);
        if(ret <= 0 ){
            printf("Got errro with write ops\n");
            close(fd);
            return -1;
        } 
    }
    close(fd);
    return 1;
}