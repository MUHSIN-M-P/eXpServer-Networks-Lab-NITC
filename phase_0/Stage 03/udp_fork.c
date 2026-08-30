#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

#define PORT 8080
#define BUFF_SIZE 10000

typedef struct
{
    char message[BUFF_SIZE];
    struct sockaddr_in client_addr;
    int sockfd;
    socklen_t addr_len;
} client_data_t;

void strrev(char *str)
{
    for (int start = 0, end = strlen(str) - 2; start < end; start++, end--)
    {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
    }
}

int main()
{
    int sockfd;
    char buffer[BUFF_SIZE];
    struct sockaddr_in server_addr, client_addr;
    pthread_t thread_id;

    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    // Set server address parameters
    server_addr.sin_family = AF_INET;                // IPv4
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Any incoming interface
    server_addr.sin_port = htons(PORT);              // Server port

    // Bind the socket to the server address
    bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    printf("[INFO] server listening on port %d\n", PORT);

    while (1)
    {
        socklen_t len = sizeof(client_addr);
        ssize_t n = recvfrom(sockfd, buffer, BUFF_SIZE, 0, (struct sockaddr *)&client_addr, &len);
        buffer[n] = '\0';

        pid_t pid = fork();

        if (pid == 0)
        {
            printf("[CLIENT MESSAGE] %s", buffer);

            // Reverse the string
            strrev(buffer);

            // Send back the reversed string
            sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&(client_addr),len);

            exit(0);//child exists
        }
        else if(pid<0)
        {
            perror("fork error\n");
        }
    }

    // Close the socket (unreachable in this infinite loop)
    close(sockfd);
    return 0;
}