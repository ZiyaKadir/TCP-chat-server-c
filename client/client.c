#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "../utils/utils.h"
#include "client_helper.h"


void *receive_thread(void *arg) {
    (void)arg; 
    char buffer[4096];
    int bytes_received;
    
    printf("Receive thread started\n");
    
    while (client_running) {
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(client_socket, &read_fds);
        
        timeout.tv_sec = 1;  
        timeout.tv_usec = 0;
        
        int select_result = select(client_socket + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select failed");
            client_running = 0;
            break;
        }
        else if (select_result == 0) {
            continue;  
        }
        else {
            if (FD_ISSET(client_socket, &read_fds)) {
                bytes_received = receive_message(buffer, sizeof(buffer));
                
                if (bytes_received <= 0) {
                    if (bytes_received == 0) {
                        printf("Server closed the connection\n");
                    } else {
                        perror("recv failed");
                    }
                    client_running = 0;
                    break;
                }
                
                buffer[bytes_received] = '\0';
                
                if (strncmp(buffer, "FILE_UPLOAD_REQUEST:", 20) == 0) {
                    char *colon1 = strchr(buffer + 20, ':');
                    if (colon1) {
                        *colon1 = '\0';
                        char *filename = buffer + 20;
                        char *target_username = colon1 + 1;
                        
                        printf("\n Server requesting upload of: %s to %s\n", filename, target_username);
                        printf("Starting file upload...\n");
                        
                        // Upload the file
                        if (upload_file_to_server(filename, target_username) == 0) {
                            printf(" File upload completed successfully\n");
                        } else {
                            printf(" Failed to upload file: %s\n", filename);
                        }
                        
                        printf("Enter a command: ");
                        fflush(stdout);
                    }
                } 
                else if (strncmp(buffer, "FILE_DOWNLOAD:", 14) == 0) {
                    // Handle incoming file download
                    printf("\n Receiving file from server...\n");
                    if (receive_file_from_server(buffer) == 0) {
                        // Success message already printed in receive_file_from_server
                    } else {
                        printf(" Failed to receive file\n");
                        printf("Enter a command: ");
                        fflush(stdout);
                    }
                } 
                else if (strncmp(buffer, "FILE_TRANSFER_ABORT", 19) == 0) {
                    printf("\n %s\n", buffer);
                    printf(" File transfer cancelled due to server shutdown\n");
                    printf("Enter a command: ");
                    fflush(stdout);
                } 
                else if (strncmp(buffer, "SERVER_SHUTDOWN", 15) == 0) {
                    printf("\n %s\n", buffer);
                    printf(" Disconnecting from server...\n");
                    
                    // Set flag to stop receive thread
                    client_running = 0;
                    
                    // Don't try to send /exit if server is shutting down - connection might be closed
                    // Just break out of receive loop
                    printf("Server initiated shutdown - disconnecting gracefully\n");
                    break;
                }
                else {
                    // Handle regular text messages
                    printf("\nReceived: %s\n", buffer);
                    printf("Enter a command: ");
                    fflush(stdout);
                }
            }
        }
    }
    
    printf("Receive thread ending gracefully\n");
    return NULL;
}

pthread_t thread_id;

int main(int argc, char **argv) {
    struct client_parameter params;
    
    // Parse command line arguments
    if (parse_client_args(argc, argv, &params) != 0) {
        return 1;
    }
    
    printf("Server IP: %s\n", params.server_ip);
    printf("Server Port: %d\n", params.port);
    
    setup_signal_handlers();
    
    if (initialize_client(params.server_ip, params.port) != 0) {
        fprintf(stderr, "Failed to connect to server\n");
        return 1;
    }
    

    
    // Login to server (handles all username input internally)
    if (login_to_server() != 0) {  // ← NO PARAMETER
        cleanup_client();
        return 1;
    }
    
    if (pthread_create(&thread_id, NULL, receive_thread, NULL) != 0) {
        perror("Thread creation failed");
        cleanup_client();
        return 1;
    }
    
    process_user_input();
    
    pthread_join(thread_id, NULL);
    cleanup_client();
    
    return 0;
}