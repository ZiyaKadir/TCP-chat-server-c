#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../utils/utils.h"
#include "server_helper.h"

client_thread_data_t *current_thread_data = NULL;
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char **argv) {
    struct server_parameter params;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    if (parse_server_args(argc, argv, &params) != 0) {
        return 1;
    }
    
    blue();
    printf("Server Port: %d\n", params.port);
    reset();
    
    setup_signal_handlers();
    
    if (initialize_server(params.port) != 0) {
        red();
        fprintf(stderr, "Failed to initialize server\n");
        reset();
        return 1;
    }
    
    init_clients();
    init_rooms();

    if (init_file_queue() != 0) {
        red();
        fprintf(stderr, "Failed to initialize file transfer queue\n");
        reset();
        cleanup_rooms();
        cleanup_clients();
        cleanup_server();
        return 1;
    }

    init_logging();
    log_message(LOG_SERVER, "Server starting on port %d", params.port);
    log_message(LOG_SERVER, "Client management system initialized");
    log_message(LOG_SERVER, "Room management system initialized");
    log_message(LOG_SERVER, "File transfer queue initialized");
    
    green();
    printf("Server listening on port %d...\n", params.port);
    reset();
    log_message(LOG_SERVER, "Server ready - listening for client connections");
    
    while (server_running) {
        client_thread_data_t *thread_data = malloc(sizeof(client_thread_data_t));
        if (thread_data == NULL) {
            log_message(LOG_ERROR, "Memory allocation failed for client thread data");
            red();
            perror("Memory allocation failed");
            reset();
            continue;
        }

        memset(thread_data, 0, sizeof(client_thread_data_t));
        thread_data->client_socket = -1;
        thread_data->client_port = 0;
        thread_data->client_ip[0] = '\0';

        pthread_mutex_lock(&thread_mutex);
        current_thread_data = thread_data;
        pthread_mutex_unlock(&thread_mutex);
        
        thread_data->client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (thread_data->client_socket < 0) {
            log_message(LOG_ERROR, "Failed to accept client connection: %s", strerror(errno));
            red();
            perror("Accept failed");
            reset();
            
            pthread_mutex_lock(&thread_mutex);
            current_thread_data = NULL;
            pthread_mutex_unlock(&thread_mutex);
            free(thread_data);
            continue;
        }
        
        inet_ntop(AF_INET, &client_addr.sin_addr, thread_data->client_ip, INET_ADDRSTRLEN);
        thread_data->client_port = ntohs(client_addr.sin_port);
        
        cyan();
        printf("New client connected from %s:%d\n", 
               thread_data->client_ip, thread_data->client_port);
        reset();
        log_message(LOG_CLIENT, "New connection from %s:%d (socket %d)", 
                   thread_data->client_ip, thread_data->client_port, thread_data->client_socket);
        
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)thread_data) != 0) {
            log_message(LOG_ERROR, "Failed to create thread for client %s:%d", 
                       thread_data->client_ip, thread_data->client_port);
            red();
            perror("Thread creation failed");
            reset();
            close(thread_data->client_socket);

            pthread_mutex_lock(&thread_mutex);
            current_thread_data = NULL;
            pthread_mutex_unlock(&thread_mutex);
            free(thread_data);
            continue;
        }
        
        log_message(LOG_CLIENT, "Created handler thread for client %s:%d", 
                   thread_data->client_ip, thread_data->client_port);
        
        pthread_mutex_lock(&thread_mutex);
        current_thread_data = NULL;
        pthread_mutex_unlock(&thread_mutex);
        
        pthread_detach(thread_id);
    }

    printf("\nServer shutting down normally...\n");
    
    int client_count = count_active_threads();
    if (client_count > 0) {
        printf("Notifying %d connected clients about shutdown...\n", client_count);
        shutdown_all_clients();
        
        sleep(2);
        
        client_count = count_active_threads();
        if (client_count > 0) {
            printf("Force disconnecting %d remaining clients\n", client_count);
        }
    }



    
    log_message(LOG_SERVER, "Server shutdown initiated");
    cleanup_file_queue();
    log_message(LOG_SERVER, "File transfer queue cleaned up");
    cleanup_clients();
    log_message(LOG_SERVER, "Client management cleaned up");
    cleanup_rooms();
    log_message(LOG_SERVER, "Room management cleaned up");
    cleanup_server();
    log_message(LOG_SERVER, "Server shutdown complete");
    cleanup_logging();
    
    return 0;
}