#include "client_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <ctype.h>  // Add this for isalnum()
#include <sys/stat.h>
#include <fcntl.h>

#define CHUNK_SIZE 4096
#define MAX_FILE_SIZE (3 * 1024 * 1024 )

int client_socket = -1;
int client_running = 1;
extern pthread_t thread_id;


void handle_sigint(int sig) {
    (void)sig;  
    printf("\nDisconnecting from server...\n");
    client_running = 0;
    
    if (client_socket != -1) {
        printf("Waiting for receive thread to finish...\n");
        
        if (pthread_join(thread_id, NULL) == 0) {
            printf("Receive thread joined successfully\n");
        } else {
            printf("Failed to join receive thread, proceeding with cleanup\n");
        }
        
        cleanup_client();
    }
    
    printf("Client shutdown complete.\n");
    exit(0);
}

void setup_signal_handlers() {
    signal(SIGINT, handle_sigint);
}


int initialize_client(const char *server_ip, int port) {
    if (connect_to_server(server_ip, port) != 0) {
        return -1;
    }
    
    return 0;
}


int connect_to_server(const char *server_ip, int port) {
    struct sockaddr_in server_addr;
    
    // Create socket
    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    // Initialize server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // Convert IP address from text to binary
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        close(client_socket);
        client_socket = -1;
        return -1;
    }
    
    // Connect to the server
    if (connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(client_socket);
        client_socket = -1;
        return -1;
    }
    
    printf("Connected to server at %s:%d\n", server_ip, port);
    
    
    return 0;
}


void cleanup_client() {
    if (client_socket != -1) {
        close(client_socket);
        client_socket = -1;
    }
}


int send_message(const char *message) {
    if (client_socket == -1 || message == NULL) {
        return -1;
    }
    
    // Calculate message length
    uint32_t message_len = strlen(message);
    uint32_t network_len = htonl(message_len);  // Convert to network byte order
    
    // Send length first (4 bytes)
    int total_sent = 0;
    int bytes_to_send = sizeof(network_len);
    char *len_ptr = (char *)&network_len;
    
    while (total_sent < bytes_to_send) {
        int sent = send(client_socket, len_ptr + total_sent, bytes_to_send - total_sent, 0);
        if (sent <= 0) {
            perror("Failed to send message length");
            return -1;
        }
        total_sent += sent;
    }
    
    // Send actual message
    total_sent = 0;
    bytes_to_send = message_len;
    
    while (total_sent < bytes_to_send) {
        int sent = send(client_socket, message + total_sent, bytes_to_send - total_sent, 0);
        if (sent <= 0) {
            perror("Failed to send message data");
            return -1;
        }
        total_sent += sent;
    }
    
    return 0;  // Success
}


int receive_message(char *buffer, size_t buffer_size) {
    if (client_socket == -1 || buffer == NULL || buffer_size < 1) {
        return -1;
    }
    
    // First, receive the message length (4 bytes)
    uint32_t network_len;
    int total_received = 0;
    int bytes_to_receive = sizeof(network_len);
    char *len_ptr = (char *)&network_len;
    
    while (total_received < bytes_to_receive) {
        int received = recv(client_socket, len_ptr + total_received, bytes_to_receive - total_received, 0);
        if (received <= 0) {
            if (received == 0) {
                return 0;  // Connection closed
            }
            perror("Failed to receive message length");
            return -1;
        }
        total_received += received;
    }
    
    // Convert from network byte order
    uint32_t message_len = ntohl(network_len);
    
    // Validate message length
    if (message_len == 0) {
        return 0;  // Empty message
    }
    if (message_len >= buffer_size) {
        fprintf(stderr, "Message too large: %u bytes (buffer size: %zu)\n", message_len, buffer_size);
        return -1;
    }
    
    // Receive the actual message
    total_received = 0;
    bytes_to_receive = message_len;
    
    while (total_received < bytes_to_receive) {
        int received = recv(client_socket, buffer + total_received, bytes_to_receive - total_received, 0);
        if (received <= 0) {
            if (received == 0) {
                fprintf(stderr, "Connection closed while receiving message data\n");
                return -1;
            }
            perror("Failed to receive message data");
            return -1;
        }
        total_received += received;
    }
    
    // Null-terminate the message
    buffer[message_len] = '\0';
    
    return message_len;  // Return actual message length
}


int login_to_server(void) {
    char username[17];
    char response[128];
    int bytes_received;
    
    while (1) {
        printf("Enter username: ");
        if (fgets(username, sizeof(username), stdin) == NULL) {
            fprintf(stderr, "Failed to read username\n");
            return -1;
        }
        
        if (strlen(username) == 1 && username[0] == '\n') {
            printf("Username cannot be empty. Please try again.\n");
            continue;  // Ask again without sending anything to server
        }

        size_t len = strlen(username);
        if (len > 0 && username[len - 1] == '\n') {
            username[len - 1] = '\0';
        }
        
        if (send_message(username) < 0) {
            perror("Failed to send username");
            return -1;
        }
        
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            strcpy(cwd, ".");  // Default fallback
        }
        
        if (send_message(cwd) < 0) {
            perror("Failed to send file path");
            return -1;
        }
        
        bytes_received = receive_message(response, sizeof(response));
        if (bytes_received <= 0) {
            perror("Failed to receive login response");
            return -1;
        }
        
        response[bytes_received] = '\0';
        
        if (strncmp(response, "LOGIN_SUCCESS", 13) == 0) {
            printf("Logged in as %s\n", username);
            return 0;  // Success
        } 
        else {
            printf("Login failed: %s\n", response);
            // Continue loop - try again with new username
        }
    }
}



void process_user_input(void) {
    char input[1024];
    
    printf("Welcome to the chat! Type /help for available commands.\n");
    printf("Enter a command: ");
    fflush(stdout);
    
    while (client_running) {
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);  
        
        timeout.tv_sec = 1;   
        timeout.tv_usec = 0;
        
        int select_result = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) {
                if (!client_running) {
                    printf("\nExiting due to server shutdown...\n");
                    break;
                }
                continue;
            }
            perror("select failed");
            break;
        }
        else if (select_result == 0) {
            if (!client_running) {
                printf("\nExiting due to server shutdown...\n");
                break;
            }
            continue;
        }
        else {
            if (FD_ISSET(STDIN_FILENO, &read_fds)) {
                if (fgets(input, sizeof(input), stdin) == NULL) {
                    break;  
                }
                
                size_t len = strlen(input);
                if (len > 0 && input[len - 1] == '\n') {
                    input[len - 1] = '\0';
                }
                
                if (strlen(input) == 0) {
                    printf("Enter a command: ");
                    continue;
                }
                
                if (input[0] == '/') {
                    command_result_t validation_result = validate_command(input);
                    
                    if (validation_result != CMD_VALID) {
                        printf("Enter a command: ");
                        continue;
                    }
                    
                    if (strncmp(input, "/exit", 5) == 0) {
                        if (send_message(input) < 0) {
                            perror("Failed to send exit command");
                        }
                        printf("Disconnecting from server...\n");
                        client_running = 0;
                        break;
                    }
                    else if (strncmp(input, "/help", 5) == 0) {
                        display_help_menu();
                    }
                    else {
                        if (send_message(input) < 0) {
                            perror("Failed to send command");
                        } else {
                            printf("Command sent: %s\n", input);
                        }
                    }
                }
                else {
                    yellow();
                    printf("Commands must start with '/'. Type /help to see available commands.\n");
                    printf("To send a message: /broadcast %s\n", input);
                    reset();
                }
                
                if (client_running) {
                    printf("Enter a command: ");
                    fflush(stdout);
                }
            }
        }
    }
    
    printf("Input processing ended.\n");
}


command_result_t validate_command(const char *command) {
    if (command == NULL || strlen(command) == 0) {
        return CMD_INVALID_COMMAND;
    }
    
    if (command[0] != '/') {
        return CMD_INVALID_FORMAT;
    }
    
    const char *cmd_str = command + 1;
    char args[10][256];
    int arg_count = extract_command_args(cmd_str, args, 10);
    
    if (arg_count == 0) {
        return CMD_INVALID_COMMAND;
    }
    
    if (strcmp(args[0], "exit") == 0) {
        if (arg_count != 1) {
            printf("Error: /exit command takes no arguments\n");
            printf("Usage: /exit\n");
            return CMD_TOO_MANY_ARGS;
        }
        return CMD_VALID;
    }
    else if (strcmp(args[0], "help") == 0) {
        if (arg_count != 1) {
            printf("Error: /help command takes no arguments\n");
            printf("Usage: /help\n");
            return CMD_TOO_MANY_ARGS;
        }
        return CMD_VALID;
    }
    else if (strcmp(args[0], "leave") == 0) {
        if (arg_count != 1) {
            printf("Error: /leave command takes no arguments\n");
            printf("Usage: /leave\n");
            return CMD_TOO_MANY_ARGS;
        }
        return CMD_VALID;
    }
    else if (strcmp(args[0], "join") == 0) {
        if (arg_count < 2) {
            printf("Error: /join command requires a room name\n");
            printf("Usage: /join <room_name>\n");
            return CMD_MISSING_ARGS;
        }
        if (arg_count > 2) {
            printf("Error: /join command takes only one argument (room name)\n");
            printf("Usage: /join <room_name>\n");
            return CMD_TOO_MANY_ARGS;
        }
        if (strlen(args[1]) == 0) {
            printf("Error: Room name cannot be empty\n");
            return CMD_EMPTY_MESSAGE;
        }
        return CMD_VALID;
    }
    else if (strcmp(args[0], "broadcast") == 0) {
        if (arg_count < 2) {
            printf("Error: /broadcast command requires a message\n");
            printf("Usage: /broadcast <message>\n");
            return CMD_MISSING_ARGS;
        }
        const char *message_start = strstr(command, "broadcast ");
        if (message_start) {
            message_start += 10; // Skip "broadcast "
            if (strlen(message_start) == 0) {
                printf("Error: Broadcast message cannot be empty\n");
                return CMD_EMPTY_MESSAGE;
            }
        }
        return CMD_VALID;
    }
    else if (strcmp(args[0], "whisper") == 0) {
        if (arg_count < 3) {
            printf("Error: /whisper command requires username and message\n");
            printf("Usage: /whisper <username> <message>\n");
            return CMD_MISSING_ARGS;
        }
        if (strlen(args[1]) == 0) {
            printf("Error: Username cannot be empty\n");
            return CMD_EMPTY_MESSAGE;
        }
        const char *username_end = strstr(command, args[1]);
        if (username_end) {
            const char *message_start = username_end + strlen(args[1]);
            while (*message_start == ' ' || *message_start == '\t') {
                message_start++;
            }
            if (strlen(message_start) == 0) {
                printf("Error: Whisper message cannot be empty\n");
                return CMD_EMPTY_MESSAGE;
            }
        }
        return CMD_VALID;
    }
    else if (strcmp(args[0], "sendfile") == 0) {
        if (arg_count < 3) {
            printf("Error: /sendfile command requires filename and username\n");
            printf("Usage: /sendfile <filename> <username>\n");
            return CMD_MISSING_ARGS;
        }
        if (arg_count > 3) {
            printf("Error: /sendfile command takes exactly two arguments\n");
            printf("Usage: /sendfile <filename> <username>\n");
            return CMD_TOO_MANY_ARGS;
        }
        if (strlen(args[1]) == 0) {
            printf("Error: Filename cannot be empty\n");
            return CMD_EMPTY_MESSAGE;
        }
        if (strlen(args[2]) == 0) {
            printf("Error: Username cannot be empty\n");
            return CMD_EMPTY_MESSAGE;
        }
        
        if (!validate_local_file(args[1])) {
            return CMD_INVALID_COMMAND;  
        }
        
        size_t file_size;
        if (get_file_size(args[1], &file_size) == 0) {
            if (file_size > MAX_FILE_SIZE) {
                red();
                printf("Error: File too large (%zu bytes, max %d bytes)\n", file_size, MAX_FILE_SIZE);
                reset();
                return CMD_INVALID_COMMAND;
            }
            
            if (file_size == 0) {
                yellow();
                printf("Warning: File '%s' is empty\n", args[1]);
                reset();
            } else {
                green();
                printf("File ready for upload: %s (%zu bytes)\n", args[1], file_size);
                reset();
            }
        } else {
            return CMD_INVALID_COMMAND;
        }
        
        return CMD_VALID;
    }
    else {
        printf("Error: Unknown command '%s'\n", args[0]);
        printf("Type /help to see available commands\n");
        return CMD_INVALID_COMMAND;
    }
}


int extract_command_args(const char *command, char args[][256], int max_args) {
    if (command == NULL || args == NULL || max_args <= 0) {
        return 0;
    }
    
    char temp_command[1024];
    strncpy(temp_command, command, sizeof(temp_command) - 1);
    temp_command[sizeof(temp_command) - 1] = '\0';
    
    int arg_count = 0;
    char *token = strtok(temp_command, " \t");
    
    while (token != NULL && arg_count < max_args) {
        strncpy(args[arg_count], token, 255);
        args[arg_count][255] = '\0';
        arg_count++;
        token = strtok(NULL, " \t");
    }
    
    return arg_count;
}


int count_command_args(const char *command) {
    if (command == NULL || strlen(command) == 0) {
        return 0;
    }
    
    int count = 0;
    int in_word = 0;
    
    for (const char *ptr = command; *ptr != '\0'; ptr++) {
        if (*ptr != ' ' && *ptr != '\t') {
            if (!in_word) {
                count++;
                in_word = 1;
            }
        } else {
            in_word = 0;
        }
    }
    
    return count;
}

void display_help_menu(void) {
    printf("\n==================== CHAT COMMANDS ====================\n");
    printf("  /join <room_name>              - Join or create a room\n");
    printf("  /leave                         - Leave the current room\n");
    printf("  /broadcast <message>           - Send message to everyone in room\n");
    printf("  /whisper <username> <message>  - Send private message to user\n");
    printf("  /sendfile <filename> <username> - Send file to specific user\n");
    printf("  /exit                          - Disconnect from server\n");
    printf("  /help                          - Display this help message\n");
    printf("======================================================\n");
    printf("Note: Messages without '/' are automatically broadcast\n\n");
}





int handle_command(const char *command) {
	(void)command; 
    return 0;
}



int upload_file_to_server(const char *filename, const char *target_username) {
    printf("[FILE-UPLOAD] Starting upload of: %s to %s\n", filename, target_username);
    
    if (!validate_local_file(filename)) {
        return -1;  
    }
    
    size_t file_size;
    if (get_file_size(filename, &file_size) != 0) {
        return -1;  
    }
    
    if (file_size > MAX_FILE_SIZE) {
        red();
        printf("Error: File too large (%zu bytes, max %d bytes)\n", file_size, MAX_FILE_SIZE);
        reset();
        return -1;
    }
    
    if (file_size == 0) {
        yellow();
        printf("Warning: File '%s' is empty\n", filename);
        reset();
    }
    
    printf("[FILE-UPLOAD] File validated: %s (%zu bytes)\n", filename, file_size);
    
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        red();
        printf("[FILE-UPLOAD] Error: Cannot open file '%s'\n", filename);
        perror("open");
        reset();
        return -1;
    }
    
    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0) {
        red();
        printf("[FILE-UPLOAD] Error: Cannot get file stats for '%s'\n", filename);
        perror("fstat");
        reset();
        close(fd);
        return -1;
    }
    
    printf("[FILE-UPLOAD] File size: %zu bytes\n", file_size);
    
    uint32_t network_size = htonl((uint32_t)file_size);
    if (send(client_socket, &network_size, sizeof(network_size), 0) != sizeof(network_size)) {
        red();
        printf("[FILE-UPLOAD] Error: Failed to send file size\n");
        reset();
        close(fd);
        return -1;
    }
    
    char buffer[CHUNK_SIZE];
    size_t total_sent = 0;
    
    while (total_sent < file_size) {
        ssize_t bytes_read = read(fd, buffer, CHUNK_SIZE);
        if (bytes_read < 0) {
            red();
            printf("[FILE-UPLOAD] Error: Failed to read from file\n");
            perror("read");
            reset();
            close(fd);
            return -1;
        }
        
        if (bytes_read == 0) {
            break;
        }
        
        size_t total_chunk_sent = 0;
        while (total_chunk_sent < (size_t)bytes_read) {
            ssize_t sent = send(client_socket, buffer + total_chunk_sent, 
                              bytes_read - total_chunk_sent, 0);
            if (sent <= 0) {
                red();
                printf("[FILE-UPLOAD] Error: Connection lost during upload\n");
                reset();
                close(fd);
                return -1;
            }
            total_chunk_sent += sent;
        }
        
        total_sent += bytes_read;
        
        int progress = (int)((total_sent * 100) / file_size);
        if (progress % 10 == 0 || total_sent == file_size) {
            green();
            printf("[FILE-UPLOAD] Progress: %zu/%zu bytes (%d%%)\n", 
                   total_sent, file_size, progress);
            reset();
        }
    }
    
    close(fd);
    green();
    printf("[FILE-UPLOAD] Upload completed: %s (%zu bytes)\n", filename, total_sent);
    reset();
    return 0;
}



int receive_file_from_server(const char *message) {
    if (strncmp(message, "FILE_DOWNLOAD:", 14) != 0) {
        return -1;  
    }
    
    char *msg_copy = malloc(strlen(message) + 1);
    strcpy(msg_copy, message);
    
    char *token = strtok(msg_copy + 14, ":");  
    if (!token) {
        free(msg_copy);
        return -1;
    }
    char *filename = malloc(strlen(token) + 1);
    strcpy(filename, token);
    
    token = strtok(NULL, ":");
    if (!token) {
        free(filename);
        free(msg_copy);
        return -1;
    }
    size_t file_size = atol(token);
    
    token = strtok(NULL, ":");
    if (!token) {
        free(filename);
        free(msg_copy);
        return -1;
    }
    char *sender = malloc(strlen(token) + 1);
    strcpy(sender, token);
    
    free(msg_copy);
    
    printf("[FILE-DOWNLOAD] Receiving file: %s (%zu bytes) from %s\n", 
           filename, file_size, sender);
    
    uint32_t network_size;
    if (recv(client_socket, &network_size, sizeof(network_size), MSG_WAITALL) != sizeof(network_size)) {
        printf("[FILE-DOWNLOAD] Error: Failed to receive file size confirmation\n");
        free(filename);
        free(sender);
        return -1;
    }
    
    size_t confirmed_size = ntohl(network_size);
    if (confirmed_size != file_size) {
        printf("[FILE-DOWNLOAD] Error: File size mismatch\n");
        free(filename);
        free(sender);
        return -1;
    }
    
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[FILE-DOWNLOAD] Error: Cannot create file '%s'\n", filename);
        perror("open");
        free(filename);
        free(sender);
        return -1;
    }
    
    char buffer[CHUNK_SIZE];
    size_t total_received = 0;
    
    while (total_received < file_size) {
        size_t remaining = file_size - total_received;
        size_t chunk_size = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        
        ssize_t received = recv(client_socket, buffer, chunk_size, 0);
        if (received <= 0) {
            printf("[FILE-DOWNLOAD] Error: Connection lost during download\n");
            close(fd);
            unlink(filename);  
            free(filename);
            free(sender);
            return -1;
        }
        
        size_t total_written = 0;
        while (total_written < (size_t)received) {
            ssize_t written = write(fd, buffer + total_written, received - total_written);
            if (written < 0) {
                printf("[FILE-DOWNLOAD] Error: Failed to write to file\n");
                perror("write");
                close(fd);
                unlink(filename);
                free(filename);
                free(sender);
                return -1;
            }
            total_written += written;
        }
        
        total_received += received;
        
        int progress = (int)((total_received * 100) / file_size);
        if (progress % 10 == 0 || total_received == file_size) {
            printf("[FILE-DOWNLOAD] Progress: %zu/%zu bytes (%d%%)\n", 
                   total_received, file_size, progress);
        }
    }
    
    close(fd);
    
    printf("[FILE-DOWNLOAD] Download completed: %s (%zu bytes) from %s\n", 
           filename, total_received, sender);
    
    printf("\n File received: '%s' from %s (%zu bytes)\n", filename, sender, file_size);
    printf("Enter a command: ");
    fflush(stdout);
    
    free(filename);
    free(sender);
    return 0;
}


int validate_local_file(const char *filename) {
    if (!filename || strlen(filename) == 0) {
        red();
        printf("Error: Filename cannot be empty\n");
        reset();
        return 0;
    }
    
    if (access(filename, F_OK) != 0) {
        red();
        printf("Error: File '%s' does not exist\n", filename);
        reset();
        return 0;
    }
    
    if (access(filename, R_OK) != 0) {
        red();
        printf("Error: File '%s' is not readable (permission denied)\n", filename);
        reset();
        return 0;
    }
    
    struct stat file_stat;
    if (stat(filename, &file_stat) != 0) {
        red();
        printf("Error: Cannot get file information for '%s'\n", filename);
        reset();
        return 0;
    }
    
    if (!S_ISREG(file_stat.st_mode)) {
        red();
        printf("Error: '%s' is not a regular file\n", filename);
        reset();
        return 0;
    }
    
    return 1; 
}


int get_file_size(const char *filename, size_t *file_size) {
    if (!filename || !file_size) {
        return -1;
    }
    
    struct stat file_stat;
    if (stat(filename, &file_stat) != 0) {
        red();
        printf("Error: Cannot get file size for '%s'\n", filename);
        reset();
        return -1;
    }
    
    *file_size = file_stat.st_size;
    return 0;
}
