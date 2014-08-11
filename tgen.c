#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

int main(int argc,char ** argv){
    struct stat fileInfo;
    unsigned st_size;
    int filename_s ;
    int filename_l ;
    char * filePath;
    char * fileName;
    struct in_addr addr;
    unsigned short port;
    int fd ;
    pid_t pid;
    int fds[2];
    int fileID = 0;
    

    if(argc != 5){
        printf("Usage: ./tgen ip port filePath filename\n");
        exit(0);
    }
    filePath = argv[3];
    if(stat(filePath,&fileInfo) != 0){
        perror(filePath);
        exit(0);
    }
    
    st_size = (unsigned)fileInfo.st_size;

    filename_s = strlen(filePath) - 1;
    while(--filename_s && filePath[filename_s - 1] != '/');
    fileName = filePath + filename_s;
    filename_l = strlen(fileName);

    printf("filename is %s\n",fileName);
    printf("file size is %u\n",st_size);
    if(inet_aton(argv[1],&addr) == 0){
        perror("Wrong address");
        exit(0);
    }
    port = atoi(argv[2]);
    printf("ip addr = %s:%u\n",inet_ntoa(addr),port);

    
    fd = open(argv[4],O_RDWR | O_CREAT | O_TRUNC,0777);
    if(fd < 0){
        puts(argv[4]);
        perror("error");
        exit(0);
    }

    if(pipe(fds)){
        perror("a");
        exit(0);
    }

    pid = fork();
    
    if(pid){
        char tmp[9] = "";
        int count = 0;
        unsigned taddr;

        close(fds[1]);
        count = read(fds[0],tmp,8);
        if(count == 8){
            tmp[9] = 0;
            sscanf(tmp,"%p",(void**)&fileID);
            printf("fileID = %X\n",fileID);
            kill(pid,SIGINT);
        }
        else{
            exit(0);
        }

        write(fd,&fileID,4);
        memcpy(&taddr,&addr.s_addr,4);
        taddr = ntohl(taddr);
        write(fd,&taddr,4);
        write(fd,&port,2);
        write(fd,&filename_l,4);
        write(fd,fileName,filename_l);
        write(fd,&st_size,4);
        
    }
    else{
        close(fds[0]);
        close(1);
        dup(fds[1]);
        execlp("md5sum","md5sum",argv[3],NULL);
        execlp("md5","md5sum",argv[3],NULL);
        printf("Your system done have md5sum or md5???\n");
    }


    return 0;
}
