/* ---------- src/client.c ------------------------------------ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "../include/common.h"
#include "../include/utils.h"
static int is_prompt_line(const char *s)
{
    size_t i = strlen(s);
    while (i && (s[i-1]==' ' || s[i-1]=='\t' || s[i-1]=='\r' || s[i-1]=='\n'))
        --i;                               /* skip trailing white-space      */
    return i && (s[i-1]==':' || s[i-1]=='>');
}

int main(void)
{
    int sockfd;
    struct sockaddr_in servaddr;
    char sendbuf[MAX_LINE], recvbuf[MAX_LINE];

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family      = AF_INET;
    servaddr.sin_port        = htons(PORT);
    servaddr.sin_addr.s_addr = htonl(0x7f000001);          /* 127.0.0.1 */

    if (connect(sockfd,(void*)&servaddr,sizeof servaddr) < 0){
        perror("connect");  exit(1);
    }

    /* ------------ simple request/response loop --------------- */
    while (1) {
        ssize_t n = recv_line(sockfd, recvbuf, sizeof recvbuf);
        if (n <= 0) break;
        fputs(recvbuf, stdout);

        if (is_prompt_line(recvbuf)) {      /* use new helper                */
            if (fgets(sendbuf, sizeof sendbuf, stdin) == NULL) break;
            send_line(sockfd, sendbuf);
        }
    }
    close(sockfd);
    return 0;
}
