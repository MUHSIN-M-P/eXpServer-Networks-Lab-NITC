#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

#define PORT 8080
#define BUFF_SIZE 10000
#define MAX_ACCEPT_BACKLOG 5

// struct sockaddr_in
// {
//   sa_family_t sin_family;  /* AF_INET */
//   in_port_t sin_port;      /* Port number */
//   struct in_addr sin_addr; /* IPv4 address */
// };

// Function to reverse a string in-place
void strrev(char *str)
{
    for (int start = 0, end = strlen(str) - 2; start < end; start++, end--)
    {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
    }
}

typedef struct {
    int conn_sock_fd;
} client_data_t;


void* handle_client(void* arg) {
    client_data_t* data = (client_data_t*)arg;

    while (1)
    {
        // Create buffer to store client message
        char buff[BUFF_SIZE];
        memset(buff, 0, BUFF_SIZE);
        // Read message from client to buffer
        ssize_t read_n = recv(data->conn_sock_fd, buff, sizeof(buff), 0);
        // Client closed connection or error occurred
        if (read_n < 0)
        {
            printf("[INFO] Error occured. Closing server\n");
            close(data->conn_sock_fd);
            break;
        }
        else if (read_n == 0)
        {
            printf("[INFO] Client Disconnected. Closing server\n");
            close(data->conn_sock_fd);
            break;
        }

        // Print message from client
        printf("[CLIENT MESSAGE] %s", buff);
        // Sting reverse
        strrev(buff);

        // Sending reversed string to client
        send(data->conn_sock_fd, buff, read_n, 0);
    }

    free(data);
    return NULL;
}

int main()
{
    // Creating listening sock
    int listen_sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Setting sock opt reuse addr
    int enable = 1;
    setsockopt(listen_sock_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

    // Creating an object of struct socketaddr_in
    struct sockaddr_in server_addr;

    // Setting up server addr
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // bind to all interfaces // 4 bytes
    server_addr.sin_port = htons(PORT);

    // Binding listening sock to port
    bind(listen_sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    // Starting to listen
    listen(listen_sock_fd, MAX_ACCEPT_BACKLOG);
    printf("[INFO] Server listening on port %d\n", PORT);

    pthread_t thread_id;

    while (1)
    {

        // Creating an object of struct socketaddr_in
        struct sockaddr_in client_addr;
        socklen_t client_addr_len=sizeof(client_addr);

        // Accept client connection
        int conn_sock_fd = accept(listen_sock_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (conn_sock_fd < 0)
        {
            printf("[ERROR] Error occured.\n");
            continue;
        }
        printf("[INFO] Client connected to server\n");

        client_data_t* data = (client_data_t*)malloc(sizeof(client_data_t));
        data->conn_sock_fd=conn_sock_fd;

         // Create a new thread to handle the client
        if (pthread_create(&thread_id, NULL, handle_client, (void*)data) != 0) {
            perror("Failed to create thread");
            free(data);
            close(conn_sock_fd);
            continue;
        }

        // Detach the thread to allow independent execution
        pthread_detach(thread_id);
       
    }
    close(listen_sock_fd);
    return 0;
}