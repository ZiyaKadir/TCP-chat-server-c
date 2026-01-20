#include "server_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ctype.h>  


pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE *log_file = NULL;


extern client_thread_data_t *current_thread_data;
extern pthread_mutex_t thread_mutex;
volatile sig_atomic_t logging_shutdown = 0;

int server_socket = -1;
volatile sig_atomic_t server_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    red();
    printf("\nServer shutting down...\n");
    reset();
    
    log_message(LOG_SERVER, "SIGINT received - initiating graceful shutdown");
    
    server_running = 0;
    
    pthread_mutex_lock(&thread_mutex);
    if (current_thread_data != NULL) {
        log_message(LOG_CLIENT, "Cleaning up pending client connection during shutdown");
        
        if (current_thread_data->client_socket >= 0) {
            log_message(LOG_CLIENT, "Force closing socket %d during shutdown", current_thread_data->client_socket);
            close(current_thread_data->client_socket);
        }
        
        free(current_thread_data);
        current_thread_data = NULL;
    }
    pthread_mutex_unlock(&thread_mutex);
    
    int initial_client_count = count_active_threads();
    int file_queue_count = get_file_queue_count();
    
    printf("[SHUTDOWN] Found %d active clients and %d pending file transfers\n", 
           initial_client_count, file_queue_count);
    
    if (initial_client_count > 0 || file_queue_count > 0) {
        shutdown_all_clients();
        
        printf("[SHUTDOWN] Waiting for clients to disconnect gracefully...\n");
        log_message(LOG_SERVER, "Waiting for %d clients to disconnect gracefully", initial_client_count);
        
        int wait_seconds = 3;
        int current_client_count;
        
        for (int i = 0; i < wait_seconds; i++) {
            sleep(1);
            current_client_count = count_active_threads();
            
            printf("[SHUTDOWN] Waiting... %d clients remaining (%d/%d seconds)\n", 
                   current_client_count, i + 1, wait_seconds);
            
            if (current_client_count == 0) {
                printf("[SHUTDOWN] All clients disconnected gracefully\n");
                log_message(LOG_SERVER, "All clients disconnected gracefully");
                break;
            }
        }
        
        current_client_count = count_active_threads();
        if (current_client_count > 0) {
            printf("[SHUTDOWN] Force disconnecting %d remaining clients\n", current_client_count);
            log_message(LOG_SERVER, "Force disconnecting %d remaining clients", current_client_count);
        }
    } else {
        printf("[SHUTDOWN] No active clients or file transfers to handle\n");
    }
    
    log_message(LOG_SERVER, "Emergency cleanup: file transfer queue");
    cleanup_file_queue();
    
    log_message(LOG_SERVER, "Emergency cleanup: client connections");
    cleanup_clients();
    
    log_message(LOG_SERVER, "Emergency cleanup: room management");
    cleanup_rooms();
    
    if (server_socket != -1) {
        log_message(LOG_SERVER, "Closing server socket");
        close(server_socket);
        server_socket = -1;
    }
    
    log_message(LOG_SERVER, "Graceful shutdown complete");
    
    cleanup_logging();
    
    green();
    printf("Server shutdown complete.\n");
    reset();
    exit(0);
}

void setup_signal_handlers() {
    signal(SIGINT, handle_sigint);
    
}


int initialize_server(int port) {
    struct sockaddr_in server_addr;
    
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        log_message(LOG_ERROR, "Socket creation failed: %s", strerror(errno));
        red();
        perror("Socket creation failed");
        reset();
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_message(LOG_ERROR, "setsockopt failed: %s", strerror(errno));
        red();
        perror("setsockopt failed");
        reset();
        close(server_socket);
        return -1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_message(LOG_ERROR, "Bind failed on port %d: %s", port, strerror(errno));
        red();
        perror("Bind failed");
        reset();
        close(server_socket);
        return -1;
    }
    
    if (listen(server_socket, 30) < 0) {  // Allow queue of up to 30 clients
        log_message(LOG_ERROR, "Listen failed: %s", strerror(errno));
        red();
        perror("Listen failed");
        reset();
        close(server_socket);
        return -1;
    }
    
    log_message(LOG_SERVER, "Server socket initialized on port %d", port);
    return 0;
}

int send_message(int client_socket, const char* message) {
    if (client_socket == -1 || message == NULL) {
        return -1;
    }
    
    uint32_t message_len = strlen(message);
    uint32_t network_len = htonl(message_len);  // Convert to network byte order
    
    int total_sent = 0;
    int bytes_to_send = sizeof(network_len);
    char *len_ptr = (char *)&network_len;
    
    while (total_sent < bytes_to_send) {
        int sent = send(client_socket, len_ptr + total_sent, bytes_to_send - total_sent, 0);
        if (sent <= 0) {
            log_message(LOG_ERROR, "Failed to send message length to socket %d: %s", client_socket, strerror(errno));
            return -1;
        }
        total_sent += sent;
    }
    
    total_sent = 0;
    bytes_to_send = message_len;
    
    while (total_sent < bytes_to_send) {
        int sent = send(client_socket, message + total_sent, bytes_to_send - total_sent, 0);
        if (sent <= 0) {
            log_message(LOG_ERROR, "Failed to send message data to socket %d: %s", client_socket, strerror(errno));
            return -1;
        }
        total_sent += sent;
    }
    
    return 0;  
}

int receive_message(int client_socket, char* buffer, size_t buffer_size) {
    if (client_socket == -1 || buffer == NULL || buffer_size < 1) {
        return -1;
    }
    
    uint32_t network_len;
    int total_received = 0;
    int bytes_to_receive = sizeof(network_len);
    char *len_ptr = (char *)&network_len;
    
    while (total_received < bytes_to_receive) {
        int received = recv(client_socket, len_ptr + total_received, bytes_to_receive - total_received, 0);
        if (received <= 0) {
            if (received == 0) {
                return 0;  
            }
            log_message(LOG_ERROR, "Failed to receive message length from socket %d: %s", client_socket, strerror(errno));
            return -1;
        }
        total_received += received;
    }
    
    uint32_t message_len = ntohl(network_len);
    
    if (message_len == 0) {
        return 0;
    }
    if (message_len >= buffer_size) {
        log_message(LOG_ERROR, "Message too large from socket %d: %u bytes (buffer size: %zu)", client_socket, message_len, buffer_size);
        return -1;
    }
    
    total_received = 0;
    bytes_to_receive = message_len;
    
    while (total_received < bytes_to_receive) {
        int received = recv(client_socket, buffer + total_received, bytes_to_receive - total_received, 0);
        if (received <= 0) {
            if (received == 0) {
                log_message(LOG_ERROR, "Connection closed while receiving message data from socket %d", client_socket);
                return -1;
            }
            log_message(LOG_ERROR, "Failed to receive message data from socket %d: %s", client_socket, strerror(errno));
            return -1;
        }
        total_received += received;
    }
    
    buffer[message_len] = '\0';
    
    return message_len;  
}



void cleanup_server() {
    if (server_socket != -1) {
        close(server_socket);
        server_socket = -1;
    }
    
}


int setup_client_connection(void *arg, char *client_ip, int *client_port) {
    if (arg == NULL) {
        log_message(LOG_ERROR, "Invalid client argument in setup_client_connection");
        red();
        fprintf(stderr, "Invalid client argument\n");
        reset();
        return -1;
    }
    
    client_thread_data_t *thread_data = (client_thread_data_t *)arg;
    
    int client_socket = thread_data->client_socket;
    strcpy(client_ip, thread_data->client_ip);
    *client_port = thread_data->client_port;
    
    free(thread_data);
    
    log_message(LOG_CLIENT, "Client connection setup: socket %d from %s:%d", client_socket, client_ip, *client_port);
    
    return client_socket;
}

int handle_client_login(int client_socket, pthread_t thread_id, 
                       const char *client_ip, int client_port) {
    char username[1024];
    char file_path[1024];
    int bytes_received;
    
    log_message(LOG_CLIENT, "Starting login process for client %s:%d", client_ip, client_port);
    
    while (1) {
        bytes_received = receive_message(client_socket, username, sizeof(username));
        if (bytes_received <= 0) {
            log_message(LOG_ERROR, "Failed to receive username from %s:%d", client_ip, client_port);
            return -1;
        }
        username[bytes_received] = '\0';
        
        bytes_received = receive_message(client_socket, file_path, sizeof(file_path));
        if (bytes_received <= 0) {
            log_message(LOG_ERROR, "Failed to receive file path from %s:%d", client_ip, client_port);
            return -1;
        }
        file_path[bytes_received] = '\0';
        
        char *end = username + strlen(username) - 1;
        while (end > username && (*end == ' ' || *end == '\n' || *end == '\t')) {
            *end = '\0';
            end--;
        }
        
        end = file_path + strlen(file_path) - 1;
        while (end > file_path && (*end == ' ' || *end == '\n' || *end == '\t')) {
            *end = '\0';
            end--;
        }
        
        log_message(LOG_CLIENT, "Login attempt: user '%s' from %s:%d, path: %s", username, client_ip, client_port, file_path);
        
        if (validate_username(username) != 0) {
            log_message(LOG_WARNING, "Invalid username format: %s from %s:%d", username, client_ip, client_port);
            yellow();
            printf("Invalid username format: %s\n", username);
            reset();
            send_message(client_socket, "Invalid username format");
            continue;
        }
        
        if (find_client_by_username(username) != NULL) {
            log_message(LOG_WARNING, "Username already taken: %s from %s:%d", username, client_ip, client_port);
            yellow();
            printf("Username already taken: %s\n", username);
            reset();
            send_message(client_socket, "Username already taken");
            continue;
        }
        
        
        client_info_t *client = add_client(username, client_socket, thread_id, 
                                          client_ip, client_port, file_path);
        
        if (client == NULL) {
            log_message(LOG_ERROR, "Failed to add client '%s' to list", username);
            red();
            printf("Failed to add client to list\n");
            reset();
            send_message(client_socket, "Server error");
            continue;
        }
        
        send_message(client_socket, "LOGIN_SUCCESS");
        log_message(LOG_CLIENT, "User '%s' successfully logged in from %s:%d", username, client_ip, client_port);
        green();
        printf("User '%s' connected\n", username);
        reset();
        
        return 0; 
    }
}

int validate_username(const char *username) {
    if (username == NULL) {
        return -1;
    }
    
    size_t len = strlen(username);
    
    if (len == 0 || len > 16) {
        log_message(LOG_WARNING, "Username validation failed: invalid length (%zu)", len);
        return -1;
    }
    
    for (size_t i = 0; i < len; i++) {
        if (!isalnum(username[i])) {
            log_message(LOG_WARNING, "Username validation failed: non-alphanumeric character in '%s'", username);
            return -1;
        }
    }
    
    return 0;
}


void client_message_loop(int client_socket) {
    char buffer[4096];
    int bytes_received;
    
    log_message(LOG_CLIENT, "Starting message loop for socket %d", client_socket);
    
    while (server_running) {
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(client_socket, &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int select_result = select(client_socket + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) {
                // printf("select() interrupted by signal for client (socket %d)\n", client_socket);
                continue;
            }
            log_message(LOG_ERROR, "select failed in client message loop for socket %d: %s", client_socket, strerror(errno));
            // perror("select failed in client message loop");
            break;
        }
        else if (select_result == 0) {
            continue;
        }
        else {
            if (FD_ISSET(client_socket, &read_fds)) {
                // printf("Data available from client (socket %d), receiving...\n", client_socket);
                
                bytes_received = receive_message(client_socket, buffer, sizeof(buffer));
                
                if (bytes_received <= 0) {
                    if (bytes_received == 0) {
                        log_message(LOG_CLIENT, "Client (socket %d) disconnected", client_socket);
                        cyan();
                        printf("Client disconnected\n");
                        reset();
                    } else {
                        log_message(LOG_ERROR, "Failed to receive message from socket %d", client_socket);
                        // perror("Failed to receive message");
                    }
                    break;
                }
                
                buffer[bytes_received] = '\0';
                log_message(LOG_DEBUG, "Received command from socket %d: %s", client_socket, buffer);
                // printf("Received command from client (socket %d): %s\n", client_socket, buffer);
                
                process_client_command(client_socket, buffer);
                
                if (strncmp(buffer, "/exit", 5) == 0) {
                    log_message(LOG_CLIENT, "Client (socket %d) requested exit", client_socket);
                    // printf("Client (socket %d) requested exit\n", client_socket);
                    break;
                }
            }
        }
    }
    
    log_message(LOG_CLIENT, "Message loop ended for socket %d", client_socket);
    // printf("Message loop ended for client (socket %d)\n", client_socket);
}


void process_client_command(int client_socket, const char *command) {
    if (command == NULL || strlen(command) == 0) {
        log_message(LOG_WARNING, "Empty command received from socket %d", client_socket);
        send_message(client_socket, "ERROR Empty command");
        return;
    }
    
    log_message(LOG_DEBUG, "Processing command from socket %d: %s", client_socket, command);
    // printf("Processing command: %s\n", command);
    
    if (strncmp(command, "/join ", 6) == 0) {
        handle_join_command(client_socket, command + 6);
    }
    else if (strncmp(command, "/leave", 6) == 0) {
        handle_leave_command(client_socket);
    }
    else if (strncmp(command, "/broadcast ", 11) == 0) {
        handle_broadcast_command(client_socket, command + 11);
    }
    else if (strncmp(command, "/whisper ", 9) == 0) {
        handle_whisper_command(client_socket, command + 9);
    }
    else if (strncmp(command, "/sendfile ", 10) == 0) {
        handle_sendfile_command(client_socket, command + 10);
    }
    else if (strncmp(command, "/exit", 5) == 0) {
        handle_exit_command(client_socket);
    }
    else {
        log_message(LOG_WARNING, "Unknown command from socket %d: %s", client_socket, command);
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "ERROR Unknown command: %s", command);
        send_message(client_socket, error_msg);
    }
}


void cleanup_client_connection(int client_socket) {
    if (client_socket != -1) {
        log_message(LOG_CLIENT, "Cleaning up client connection (socket %d)", client_socket);
        // printf("Cleaning up client (socket %d)\n", client_socket);
        
        client_info_t *client = find_client_by_socket(client_socket);
        if (client) {
            if (strlen(client->current_room_name) > 0) {
                room_info_t *current_room = find_room(client->current_room_name);
                if (current_room) {
                    pthread_mutex_lock(&current_room->room_mutex);
                    
                    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
                        if (current_room->clients[i] == client) {
                            current_room->clients[i] = NULL;
                            current_room->client_count--;
                            current_room->last_activity = time(NULL);
                            log_message(LOG_ROOM, "Removed '%s' from room '%s' (%d clients remaining)", 
                                       client->username, current_room->room_name, current_room->client_count);
                            // printf("[DISCONNECT-CLEANUP] Removed '%s' from room '%s' (%d clients remaining)\n", 
                            //        client->username, current_room->room_name, current_room->client_count);
                            break;
                        }
                    }
                    
                    char notification[256];
                    snprintf(notification, sizeof(notification), "ROOM_NOTIFICATION %s disconnected", client->username);
                    
                    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
                        if (current_room->clients[i] && current_room->clients[i]->is_active) {
                            send_message(current_room->clients[i]->socket_fd, notification);
                        }
                    }
                    
                    char room_name_copy[MAX_ROOM_NAME_LENGTH + 1];
                    strncpy(room_name_copy, current_room->room_name, sizeof(room_name_copy));
                    room_name_copy[sizeof(room_name_copy) - 1] = '\0';
                    int room_client_count = current_room->client_count;
                    
                    pthread_mutex_unlock(&current_room->room_mutex);
                    
                    if (room_client_count == 0) {
                        log_message(LOG_ROOM, "Room '%s' is empty, removing", room_name_copy);
                        yellow();
                        printf("Room '%s' removed (empty)\n", room_name_copy);
                        reset();
                    }
                }
            }
            
            log_message(LOG_CLIENT, "User '%s' disconnected from %s:%d", client->username, client->client_ip, client->client_port);
            green();
            printf("User '%s' disconnected\n", client->username);
            reset();
        }
        
        close(client_socket);
        // printf("Client (socket %d) cleanup complete\n", client_socket);
    }
}

void handle_join_command(int client_socket, const char *room_name) {
    if (!room_name || strlen(room_name) == 0) {
        log_message(LOG_WARNING, "Empty room name in join command from socket %d", client_socket);
        send_message(client_socket, "ERROR Usage: /join <room_name>");
        return;
    }
    
    client_info_t *client = find_client_by_socket(client_socket);
    if (!client) {
        log_message(LOG_ERROR, "Unable to identify client for socket %d in join command", client_socket);
        send_message(client_socket, "ERROR Unable to identify client");
        return;
    }
    
    char clean_room_name[MAX_ROOM_NAME_LENGTH + 1];
    strncpy(clean_room_name, room_name, sizeof(clean_room_name) - 1);
    clean_room_name[sizeof(clean_room_name) - 1] = '\0';
    
    char *start = clean_room_name;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    
    if (strlen(start) == 0) {
        log_message(LOG_WARNING, "Empty room name after cleaning from user '%s'", client->username);
        send_message(client_socket, "ERROR Room name cannot be empty");
        return;
    }
    
    if (strlen(start) > MAX_ROOM_NAME_LENGTH) {
        log_message(LOG_WARNING, "Room name too long from user '%s': %s", client->username, start);
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "ERROR Room name too long (max %d characters)", MAX_ROOM_NAME_LENGTH);
        send_message(client_socket, error_msg);
        return;
    }
    
    for (char *ptr = start; *ptr != '\0'; ptr++) {
        if (!isalnum(*ptr)) {
            log_message(LOG_WARNING, "Invalid room name format from user '%s': %s", client->username, start);
            send_message(client_socket, "ERROR Room name must be alphanumeric only (no spaces or special characters)");
            return;
        }
    }
    
    if (strlen(client->current_room_name) > 0 && strcmp(client->current_room_name, start) == 0) {
        log_message(LOG_INFO, "User '%s' already in room '%s'", client->username, start);
        char msg[256];
        snprintf(msg, sizeof(msg), "INFO You are already in room '%s'", start);
        send_message(client_socket, msg);
        return;
    }
    
    if (strlen(client->current_room_name) > 0) {
        room_info_t *old_room = find_room(client->current_room_name);
        if (old_room) {
            pthread_mutex_lock(&old_room->room_mutex);
            
            for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
                if (old_room->clients[i] == client) {
                    old_room->clients[i] = NULL;
                    old_room->client_count--;
                    log_message(LOG_ROOM, "Client '%s' left room '%s' (%d clients remaining)", 
                               client->username, old_room->room_name, old_room->client_count);
                    // printf("[ROOM] Client '%s' left room '%s' (%d clients remaining)\n", 
                    //        client->username, old_room->room_name, old_room->client_count);
                    break;
                }
            }
            
            pthread_mutex_unlock(&old_room->room_mutex);
            
            if (old_room->client_count == 0) {
                log_message(LOG_ROOM, "Room '%s' is empty, removing", old_room->room_name);
                // printf("[ROOM] Room '%s' is empty, removing...\n", old_room->room_name);
                remove_room(old_room->room_name);
            }
        }
    }
    
    room_info_t *target_room = find_room(start);
    if (!target_room) {
        target_room = add_room(start);
        if (!target_room) {
            log_message(LOG_ERROR, "Failed to create room '%s' for user '%s'", start, client->username);
            send_message(client_socket, "ERROR Failed to create room");
            return;
        }
        log_message(LOG_ROOM, "Created new room '%s'", start);
        green();
        printf("Room '%s' created\n", start);
        reset();
    }
    
    pthread_mutex_lock(&target_room->room_mutex);
    
    if (target_room->client_count >= MAX_CLIENTS_PER_ROOM) {
        pthread_mutex_unlock(&target_room->room_mutex);
        log_message(LOG_WARNING, "Room '%s' is full, user '%s' cannot join", start, client->username);
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "ERROR Room '%s' is full (%d/%d clients)", 
                 start, MAX_CLIENTS_PER_ROOM, MAX_CLIENTS_PER_ROOM);
        send_message(client_socket, error_msg);
        return;
    }
    
    int slot_found = -1;
    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
        if (target_room->clients[i] == NULL) {
            slot_found = i;
            break;
        }
    }
    
    if (slot_found == -1) {
        pthread_mutex_unlock(&target_room->room_mutex);
        log_message(LOG_ERROR, "Room '%s' is full (no available slots) for user '%s'", start, client->username);
        send_message(client_socket, "ERROR Room is full (no available slots)");
        return;
    }
    
    target_room->clients[slot_found] = client;
    target_room->client_count++;
    target_room->last_activity = time(NULL);
    
    strncpy(client->current_room_name, start, sizeof(client->current_room_name) - 1);
    client->current_room_name[sizeof(client->current_room_name) - 1] = '\0';
    client->current_room_index = get_room_index(start);
    
    pthread_mutex_unlock(&target_room->room_mutex);
    
    char success_msg[256];
    snprintf(success_msg, sizeof(success_msg), "JOIN_SUCCESS Joined room '%s' (%d/%d clients)", 
             start, target_room->client_count, MAX_CLIENTS_PER_ROOM);
    send_message(client_socket, success_msg);
    
    pthread_mutex_lock(&target_room->room_mutex);
    
    char notification[256];
    snprintf(notification, sizeof(notification), "ROOM_NOTIFICATION %s joined the room", client->username);
    
    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
        if (target_room->clients[i] && 
            target_room->clients[i]->is_active && 
            target_room->clients[i] != client) {
            
            send_message(target_room->clients[i]->socket_fd, notification);
        }
    }
    
    pthread_mutex_unlock(&target_room->room_mutex);
    
    log_message(LOG_JOIN, "User '%s' joined room '%s' (%d/%d clients)", 
               client->username, start, target_room->client_count, MAX_CLIENTS_PER_ROOM);
    blue();
    printf("User '%s' joined room '%s'\n", client->username, start);
    reset();
}



void handle_leave_command(int client_socket) {
    client_info_t *client = find_client_by_socket(client_socket);
    if (!client) {
        log_message(LOG_ERROR, "Unable to identify client for socket %d in leave command", client_socket);
        send_message(client_socket, "ERROR Unable to identify client");
        return;
    }
    
    if (strlen(client->current_room_name) == 0) {
        log_message(LOG_WARNING, "User '%s' tried to leave but not in any room", client->username);
        send_message(client_socket, "ERROR You are not in any room");
        return;
    }
    
    room_info_t *current_room = find_room(client->current_room_name);
    if (!current_room) {
        log_message(LOG_WARNING, "Room '%s' no longer exists for user '%s'", client->current_room_name, client->username);
        client->current_room_name[0] = '\0';
        client->current_room_index = -1;
        send_message(client_socket, "ERROR Room no longer exists");
        return;
    }
    
    pthread_mutex_lock(&current_room->room_mutex);
    
    int client_found = 0;
    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
        if (current_room->clients[i] == client) {
            current_room->clients[i] = NULL;
            current_room->client_count--;
            current_room->last_activity = time(NULL);
            client_found = 1;
            log_message(LOG_ROOM, "Client '%s' left room '%s' (%d clients remaining)", 
                       client->username, current_room->room_name, current_room->client_count);
            // printf("[LEAVE] Client '%s' left room '%s' (%d clients remaining)\n", 
            //        client->username, current_room->room_name, current_room->client_count);
            break;
        }
    }
    
    if (!client_found) {
        pthread_mutex_unlock(&current_room->room_mutex);
        log_message(LOG_WARNING, "User '%s' was not properly registered in room '%s'", client->username, client->current_room_name);
        client->current_room_name[0] = '\0';
        client->current_room_index = -1;
        send_message(client_socket, "ERROR You were not properly registered in the room");
        return;
    }
    
    char notification[256];
    snprintf(notification, sizeof(notification), "ROOM_NOTIFICATION %s left the room", client->username);
    
    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
        if (current_room->clients[i] && current_room->clients[i]->is_active) {
            send_message(current_room->clients[i]->socket_fd, notification);
        }
    }
    
    char room_name_copy[MAX_ROOM_NAME_LENGTH + 1];
    strncpy(room_name_copy, current_room->room_name, sizeof(room_name_copy));
    room_name_copy[sizeof(room_name_copy) - 1] = '\0';
    
    int room_client_count = current_room->client_count;
    
    pthread_mutex_unlock(&current_room->room_mutex);
    
    client->current_room_name[0] = '\0';
    client->current_room_index = -1;
    
    char success_msg[256];
    snprintf(success_msg, sizeof(success_msg), "LEAVE_SUCCESS Left room '%s'", room_name_copy);
    send_message(client_socket, success_msg);
    
    if (room_client_count == 0) {
        log_message(LOG_ROOM, "Room '%s' is empty, removing", room_name_copy);
        yellow();
        printf("Room '%s' removed (empty)\n", room_name_copy);
        reset();
    }
    
    log_message(LOG_LEAVE, "User '%s' left room '%s'", client->username, room_name_copy);
    magenta();
    printf("User '%s' left room '%s'\n", client->username, room_name_copy);
    reset();
}



void handle_broadcast_command(int client_socket, const char *message) {
    if (!message || strlen(message) == 0) {
        log_message(LOG_WARNING, "Empty broadcast message from socket %d", client_socket);
        send_message(client_socket, "ERROR Usage: /broadcast <message>");
        return;
    }
    
    client_info_t *sender = find_client_by_socket(client_socket);
    if (!sender) {
        log_message(LOG_ERROR, "Unable to identify sender for socket %d in broadcast", client_socket);
        send_message(client_socket, "ERROR Unable to identify sender");
        return;
    }
    
    if (strlen(sender->current_room_name) == 0) {
        log_message(LOG_WARNING, "User '%s' tried to broadcast but not in any room", sender->username);
        send_message(client_socket, "ERROR You must join a room first to broadcast messages");
        red();
        printf("User '%s' tried to broadcast but not in any room\n", sender->username);
        reset();
        return;
    }
    
    room_info_t *current_room = find_room(sender->current_room_name);
    if (!current_room) {
        log_message(LOG_WARNING, "Room '%s' no longer exists for user '%s' broadcast", sender->current_room_name, sender->username);
        sender->current_room_name[0] = '\0';
        sender->current_room_index = -1;
        send_message(client_socket, "ERROR Room no longer exists. Please join a room first.");
        return;
    }
    
    char clean_message[1024];
    strncpy(clean_message, message, sizeof(clean_message) - 1);
    clean_message[sizeof(clean_message) - 1] = '\0';
    
    char *start = clean_message;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    
    if (strlen(start) == 0) {
        log_message(LOG_WARNING, "Empty broadcast message after cleaning from user '%s'", sender->username);
        send_message(client_socket, "ERROR Broadcast message cannot be empty");
        return;
    }
    
    pthread_mutex_lock(&current_room->room_mutex);
    
    char broadcast_msg[1200];
    snprintf(broadcast_msg, sizeof(broadcast_msg), "BROADCAST [%s@%s]: %s", 
             sender->username, current_room->room_name, start);
    
    int messages_sent = 0;
    int total_recipients = 0;
    
    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
        if (current_room->clients[i] && 
            current_room->clients[i]->is_active && 
            current_room->clients[i] != sender) {
            
            total_recipients++;
            
            if (send_message(current_room->clients[i]->socket_fd, broadcast_msg) == 0) {
                messages_sent++;
            } else {
                log_message(LOG_WARNING, "Failed to deliver broadcast to '%s'", current_room->clients[i]->username);
                // printf("[BROADCAST-WARNING] Failed to deliver message to '%s'\n", 
                //        current_room->clients[i]->username);
            }
        }
    }
    
    current_room->total_messages_sent++;
    current_room->last_activity = time(NULL);
    
    pthread_mutex_unlock(&current_room->room_mutex);
    
    char confirmation[256];
    if (messages_sent == total_recipients) {
        snprintf(confirmation, sizeof(confirmation), 
                 "BROADCAST_SUCCESS Message delivered to %d recipient(s) in room '%s'", 
                 total_recipients, current_room->room_name);
    } else {
        snprintf(confirmation, sizeof(confirmation), 
                 "BROADCAST_PARTIAL Message delivered to %d/%d recipient(s) in room '%s'", 
                 messages_sent, total_recipients, current_room->room_name);
    }
    
    send_message(client_socket, confirmation);
    
    log_message(LOG_BROADCAST, "User '%s' in room '%s': %s (sent to %d/%d clients)", 
               sender->username, current_room->room_name, start, messages_sent, total_recipients);
    cyan();
    printf("Broadcast from %s@%s: %s\n", sender->username, current_room->room_name, start);
    reset();
}


void handle_whisper_command(int client_socket, const char *whisper_args) {
    if (!whisper_args || strlen(whisper_args) == 0) {
        log_message(LOG_WARNING, "Empty whisper arguments from socket %d", client_socket);
        send_message(client_socket, "ERROR Usage: /whisper <username> <message>");
        return;
    }
    
    client_info_t *sender = find_client_by_socket(client_socket);
    if (!sender) {
        log_message(LOG_ERROR, "Unable to identify sender for socket %d in whisper", client_socket);
        send_message(client_socket, "ERROR Unable to identify sender");
        return;
    }
    
    char *args_copy = malloc(strlen(whisper_args) + 1);
    strcpy(args_copy, whisper_args);
    
    char *space = strchr(args_copy, ' ');
    if (!space) {
        log_message(LOG_WARNING, "Invalid whisper format from user '%s'", sender->username);
        send_message(client_socket, "ERROR Usage: /whisper <username> <message>");
        free(args_copy);
        return;
    }
    
    *space = '\0';
    char *target_username = args_copy;
    char *message = space + 1;
    
    while (*message == ' ' || *message == '\t') {
        message++;
    }
    
    if (strlen(message) == 0) {
        log_message(LOG_WARNING, "Empty whisper message from user '%s'", sender->username);
        send_message(client_socket, "ERROR Message cannot be empty");
        free(args_copy);
        return;
    }
    
    if (strcmp(sender->username, target_username) == 0) {
        log_message(LOG_WARNING, "User '%s' tried to whisper to self", sender->username);
        send_message(client_socket, "ERROR Cannot whisper to yourself");
        free(args_copy);
        return;
    }
    
    client_info_t *target = find_client_by_username(target_username);
    if (!target || !target->is_active) {
        log_message(LOG_WARNING, "Whisper target '%s' not found (from user '%s')", target_username, sender->username);
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "ERROR User '%s' not found or offline", target_username);
        send_message(client_socket, error_msg);
        free(args_copy);
        return;
    }
    
    char whisper_msg[1024];
    snprintf(whisper_msg, sizeof(whisper_msg), "WHISPER [%s → %s]: %s", 
             sender->username, target->username, message);
    
    if (send_message(target->socket_fd, whisper_msg) < 0) {
        log_message(LOG_ERROR, "Failed to deliver whisper from '%s' to '%s'", sender->username, target_username);
        send_message(client_socket, "ERROR Failed to deliver whisper");
        free(args_copy);
        return;
    }
    
    char confirm_msg[256];
    snprintf(confirm_msg, sizeof(confirm_msg), "WHISPER_SENT Whisper sent to %s", target_username);
    send_message(client_socket, confirm_msg);
    
    log_message(LOG_WHISPER, "%s → %s: %s", sender->username, target->username, message);
    yellow();
    printf("Whisper %s → %s: %s\n", sender->username, target->username, message);
    reset();
    
    free(args_copy);
}


void handle_sendfile_command(int client_socket, const char *file_args) {
    if (!file_args || strlen(file_args) == 0) {
        log_message(LOG_WARNING, "Empty sendfile arguments from socket %d", client_socket);
        send_message(client_socket, "ERROR Usage: /sendfile <filename> <username>");
        return;
    }
    
    client_info_t *sender = find_client_by_socket(client_socket);
    if (!sender) {
        log_message(LOG_ERROR, "Unable to identify sender for socket %d in sendfile", client_socket);
        send_message(client_socket, "ERROR Unable to identify sender");
        return;
    }
    
    char *args_copy = malloc(strlen(file_args) + 1);
    if (!args_copy) {
        log_message(LOG_ERROR, "Memory allocation failed for sendfile args from user '%s'", sender->username);
        send_message(client_socket, "ERROR Server memory error");
        return;
    }
    strcpy(args_copy, file_args);
    
    char *space = strchr(args_copy, ' ');
    if (!space) {
        log_message(LOG_WARNING, "Invalid sendfile format from user '%s'", sender->username);
        send_message(client_socket, "ERROR Usage: /sendfile <filename> <username>");
        free(args_copy);
        return;
    }
    
    *space = '\0';
    char *filename = args_copy;
    char *target_username = space + 1;
    
    while (*target_username == ' ') target_username++;
    char *end = filename + strlen(filename) - 1;
    while (end > filename && *end == ' ') *end-- = '\0';
    end = target_username + strlen(target_username) - 1;
    while (end > target_username && *end == ' ') *end-- = '\0';
    
    if (strlen(filename) == 0 || strlen(target_username) == 0) {
        log_message(LOG_WARNING, "Empty filename or username in sendfile from user '%s'", sender->username);
        send_message(client_socket, "ERROR Filename and username cannot be empty");
        free(args_copy);
        return;
    }
    
    if (!validate_file_extension(filename)) {
        log_message(LOG_WARNING, "Invalid file extension '%s' from user '%s'", filename, sender->username);
        send_message(client_socket, "ERROR Invalid file type. Allowed: .txt, .pdf, .jpg, .png");
        free(args_copy);
        return;
    }
    
    if (strcmp(sender->username, target_username) == 0) {
        log_message(LOG_WARNING, "User '%s' tried to send file to self", sender->username);
        send_message(client_socket, "ERROR Cannot send file to yourself");
        free(args_copy);
        return;
    }
    
    client_info_t *receiver = find_client_by_username(target_username);
    if (!receiver || !receiver->is_active) {
        log_message(LOG_WARNING, "Sendfile target '%s' not found (from user '%s')", target_username, sender->username);
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "ERROR User '%s' not found or offline", target_username);
        send_message(client_socket, error_msg);
        free(args_copy);
        return;
    }
    
    if (is_file_queue_full()) {
        log_message(LOG_WARNING, "File queue full, rejecting sendfile from user '%s'", sender->username);
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), 
                 "ERROR Upload queue is full (%d/%d). Please try again later.", 
                 MAX_UPLOAD_QUEUE, MAX_UPLOAD_QUEUE);
        send_message(client_socket, error_msg);
        free(args_copy);
        return;
    }
    
    char upload_request[512];
    snprintf(upload_request, sizeof(upload_request), "FILE_UPLOAD_REQUEST:%s:%s", filename, target_username);
    if (send_message(client_socket, upload_request) != 0) {
        log_message(LOG_ERROR, "Failed to send upload request to user '%s'", sender->username);
        send_message(client_socket, "ERROR Failed to initiate file transfer");
        free(args_copy);
        return;
    }
    
    char *file_data = NULL;
    size_t file_size = 0;
    
    if (receive_file_from_client(client_socket, filename, &file_data, &file_size) != 0) {
        log_message(LOG_ERROR, "Failed to receive file data '%s' from user '%s'", filename, sender->username);
        send_message(client_socket, "ERROR Failed to receive file data");
        free(args_copy);
        return;
    }
    
    int queue_index = add_to_file_queue(filename, sender->username, receiver->username,
                                       file_data, file_size, sender->socket_fd, receiver->socket_fd);
    
    if (queue_index < 0) {
        log_message(LOG_ERROR, "Failed to add file transfer to queue: %s from '%s' to '%s'", filename, sender->username, receiver->username);
        send_message(client_socket, "ERROR Failed to add to transfer queue");
        free(file_data);
        free(args_copy);
        return;
    }
    
    log_message(LOG_SENDFILE, "Processing transfer: %s -> %s (%s, %zu bytes)", sender->username, receiver->username, filename, file_size);
    // printf("[SENDFILE] Processing transfer immediately: %s -> %s\n", sender->username, receiver->username);
    
    if (send_file_to_client(receiver->socket_fd, filename, sender->username, file_data, file_size) == 0) {
        char success_msg[512];
        snprintf(success_msg, sizeof(success_msg), 
                 "FILE_TRANSFER_SUCCESS File '%s' sent successfully to %s (%zu bytes)",
                 filename, target_username, file_size);
        send_message(client_socket, success_msg);
        
        log_message(LOG_SENDFILE, "Transfer completed: %s -> %s (%s, %zu bytes)", 
                   sender->username, receiver->username, filename, file_size);
        green();
        printf("File transfer completed: %s -> %s (%s)\n",
               sender->username, receiver->username, filename);
        reset();
    } else {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), 
                 "FILE_TRANSFER_FAILED Failed to send '%s' to %s",
                 filename, target_username);
        send_message(client_socket, error_msg);
        
        log_message(LOG_ERROR, "Transfer failed: %s -> %s (%s)", sender->username, receiver->username, filename);
        red();
        printf("File transfer failed: %s -> %s (%s)\n",
               sender->username, receiver->username, filename);
        reset();
    }
    
    remove_from_file_queue(queue_index);
    free(args_copy);
}



void handle_exit_command(int client_socket) {
    client_info_t *client = find_client_by_socket(client_socket);
    if (client) {
        log_message(LOG_CLIENT, "User '%s' requested exit", client->username);
        green();
        printf("User '%s' disconnecting...\n", client->username);
        reset();
    } else {
        log_message(LOG_WARNING, "Exit command from unknown client (socket %d)", client_socket);
    }
    
    // send_message(client_socket, "GOODBYE Disconnecting...");
}



void *handle_client(void *arg) {
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    
    int client_socket = setup_client_connection(arg, client_ip, &client_port);
    if (client_socket == -1) {
        log_message(LOG_ERROR, "Failed to setup client connection");
        return NULL;
    }
    
    if (handle_client_login(client_socket, pthread_self(), client_ip, client_port) != 0) {
        log_message(LOG_ERROR, "Login failed for client %s:%d (socket %d)", client_ip, client_port, client_socket);
        // printf("Login failed for client %s:%d (socket %d)\n", 
        //        client_ip, client_port, client_socket);
        cleanup_client_connection(client_socket);
        return NULL;
    }
    
    client_message_loop(client_socket);
    
    cleanup_client_connection(client_socket);
    remove_client(client_socket);
    
    return NULL;
}

const char* log_level_to_string(log_level_t level) {
    switch (level) {
        case LOG_INFO:      return "INFO";
        case LOG_ERROR:     return "ERROR";
        case LOG_WARNING:   return "WARNING";
        case LOG_DEBUG:     return "DEBUG";
        case LOG_CLIENT:    return "CLIENT";
        case LOG_ROOM:      return "ROOM";
        case LOG_FILE:      return "FILE";
        case LOG_SERVER:    return "SERVER";
        case LOG_JOIN:      return "JOIN";
        case LOG_BROADCAST: return "BROADCAST";
        case LOG_WHISPER:   return "WHISPER";
        case LOG_LEAVE:     return "LEAVE";
        case LOG_SENDFILE:  return "SENDFILE";
        default:            return "UNKNOWN";
    }
}

void init_logging(void) {
    logging_shutdown = 0;  // Initialize shutdown flag
    
    pthread_mutex_lock(&log_mutex);
    
    // Open log file in write mode (overwrites existing file)
    log_file = fopen("server.log", "w");
    
    if (!log_file) {
        perror("[LOGGING] Failed to open server.log");
        pthread_mutex_unlock(&log_mutex);
        return;
    }
    
    pthread_mutex_unlock(&log_mutex);
    
    log_message(LOG_SERVER, "=== Server logging system initialized ===");
    printf("[LOGGING] Logging system initialized - writing to server.log\n");
}

void cleanup_logging(void) {
    static volatile sig_atomic_t cleanup_done = 0;
    
    // Prevent multiple cleanup calls
    if (cleanup_done) {
        return;
    }
    cleanup_done = 1;
    
    logging_shutdown = 1;
    
    int lock_result = pthread_mutex_trylock(&log_mutex);
    
    if (lock_result == 0) {
        if (log_file) {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char timestamp[64];
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
            
            fprintf(log_file, "[%s] [%s] %s\n", timestamp, "SERVER", 
                    "=== Server shutting down - logging system cleanup ===");
            fflush(log_file);
            
            fclose(log_file);
            log_file = NULL;
        }
        
        pthread_mutex_unlock(&log_mutex);
        pthread_mutex_destroy(&log_mutex);
    } else {
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }
    }
    
    printf("[LOGGING] Logging system cleaned up\n");
}

void log_message(log_level_t level, const char *format, ...) {
    if (!format || logging_shutdown) return;
    
    pthread_mutex_lock(&log_mutex);
    
    if (logging_shutdown) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }
    

    
    if (!log_file) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    fprintf(log_file, "[%s] [%s] ", timestamp, log_level_to_string(level));
    
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    fflush(log_file);
    
    pthread_mutex_unlock(&log_mutex);
}


void shutdown_all_clients(void) {
    printf("[SHUTDOWN] Notifying all connected clients...\n");
    log_message(LOG_SERVER, "Sending shutdown notification to all connected clients");
    
    printf("[SHUTDOWN] Checking for active file transfers...\n");
    log_message(LOG_SERVER, "Checking file transfer queue before shutdown");
    
    int file_queue_count = get_file_queue_count();
    if (file_queue_count > 0) {
        printf("[SHUTDOWN] Found %d pending file transfers, notifying clients...\n", file_queue_count);
        log_message(LOG_SERVER, "Found %d pending file transfers", file_queue_count);
        
        notify_file_transfer_shutdown();
        
        usleep(500000); // 500ms
        
        abort_all_file_transfers();
    } else {
        printf("[SHUTDOWN] No active file transfers found\n");
    }
    
    pthread_mutex_lock(&client_list_mutex);
    
    client_info_t *current = client_list_head;
    int notification_count = 0;
    
    while (current) {
        if (current->is_active && current->socket_fd >= 0) {
            if (send_message(current->socket_fd, "SERVER_SHUTDOWN Server is shutting down. Please disconnect.") == 0) {
                printf("[SHUTDOWN] Notified client '%s'\n", current->username);
                notification_count++;
            } else {
                printf("[SHUTDOWN] Failed to notify client '%s'\n", current->username);
            }
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&client_list_mutex);
    
    printf("[SHUTDOWN] Sent shutdown notification to %d clients\n", notification_count);
    log_message(LOG_SERVER, "Shutdown notification sent to %d clients", notification_count);
}

int count_active_threads(void) {
    pthread_mutex_lock(&client_list_mutex);
    int count = active_client_count;
    pthread_mutex_unlock(&client_list_mutex);
    return count;
}


void notify_file_transfer_shutdown(void) {
    pthread_mutex_lock(&global_file_queue.mutex);
    
    printf("[FILE-SHUTDOWN] Checking file transfer queue (%d items)\n", global_file_queue.count);
    
    if (global_file_queue.count == 0) {
        pthread_mutex_unlock(&global_file_queue.mutex);
        printf("[FILE-SHUTDOWN] No active file transfers\n");
        return;
    }
    
    printf("[FILE-SHUTDOWN] Notifying clients about %d pending file transfers\n", global_file_queue.count);
    
    // Notify all clients involved in file transfers
    for (int i = 0; i < global_file_queue.count; i++) {
        file_queue_item_t *item = &global_file_queue.items[i];
        
        // Notify sender
        char sender_msg[512];
        snprintf(sender_msg, sizeof(sender_msg), 
                "FILE_TRANSFER_ABORT Server shutting down - file transfer of '%s' to '%s' cancelled", 
                item->filename, item->receiver_username);
        
        if (send_message(item->sender_socket, sender_msg) == 0) {
            printf("[FILE-SHUTDOWN] Notified sender '%s' about cancelled transfer\n", item->sender_username);
        }
        
        char receiver_msg[512];
        snprintf(receiver_msg, sizeof(receiver_msg), 
                "FILE_TRANSFER_ABORT Server shutting down - incoming file '%s' from '%s' cancelled", 
                item->filename, item->sender_username);
        
        if (send_message(item->receiver_socket, receiver_msg) == 0) {
            printf("[FILE-SHUTDOWN] Notified receiver '%s' about cancelled transfer\n", item->receiver_username);
        }
        
        printf("[FILE-SHUTDOWN] Cancelled transfer: %s -> %s (%s)\n", 
               item->sender_username, item->receiver_username, item->filename);
    }
    
    pthread_mutex_unlock(&global_file_queue.mutex);
}


void abort_all_file_transfers(void) {
    pthread_mutex_lock(&global_file_queue.mutex);
    
    printf("[FILE-SHUTDOWN] Aborting %d pending file transfers\n", global_file_queue.count);
    
    for (int i = 0; i < global_file_queue.count; i++) {
        if (global_file_queue.items[i].file_data) {
            printf("[FILE-SHUTDOWN] Freeing file data for: %s (%zu bytes)\n", 
                   global_file_queue.items[i].filename, 
                   global_file_queue.items[i].file_size);
            
            free(global_file_queue.items[i].file_data);
            global_file_queue.items[i].file_data = NULL;
        }
    }
    
    global_file_queue.count = 0;
    
    pthread_mutex_unlock(&global_file_queue.mutex);
    
    printf("[FILE-SHUTDOWN] All file transfers aborted and memory freed\n");
}
