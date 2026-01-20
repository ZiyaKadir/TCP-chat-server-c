#ifndef CLIENT_HELPER_H
#define CLIENT_HELPER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../utils/utils.h" 




extern int client_socket;
extern int client_running;

typedef enum {
    CMD_VALID,
    CMD_INVALID_FORMAT,
    CMD_MISSING_ARGS,
    CMD_TOO_MANY_ARGS,
    CMD_INVALID_COMMAND,
    CMD_EMPTY_MESSAGE
} command_result_t;


int initialize_client(const char *server_ip, int port);
void cleanup_client();
void setup_signal_handlers();
void handle_sigint(int sig);
int connect_to_server(const char *server_ip, int port);
int send_message(const char *message);
int receive_message(char *buffer, size_t buffer_size);
int login_to_server(void);



void display_help_menu(void);
int count_command_args(const char *command);
int extract_command_args(const char *command, char args[][256], int max_args);
command_result_t validate_command(const char *command);
void process_user_input(void);


int handle_command(const char *command);

int upload_file_to_server(const char *filename, const char *target_username);
int receive_file_from_server(const char *message);

int validate_local_file(const char *filename);
int get_file_size(const char *filename, size_t *file_size);

#endif // CLIENT_HELPER_H