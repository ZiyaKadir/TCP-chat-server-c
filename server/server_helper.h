#ifndef SERVER_HELPER_H
#define SERVER_HELPER_H

#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ctype.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include "../utils/utils.h"  



extern pthread_mutex_t log_mutex;
extern FILE *log_file;


extern int server_socket;
extern volatile sig_atomic_t server_running;
extern volatile sig_atomic_t logging_shutdown;


#define MAX_CLIENTS_PER_ROOM 15       
#define MAX_ROOM_NAME_LENGTH 32  
#define MAX_PATH_LENGTH 1024



#define MAX_UPLOAD_QUEUE 5
#define MAX_FILE_SIZE (3 * 1024 * 1024 )  // 3MB
#define MAX_FILENAME_LENGTH 256
#define CHUNK_SIZE 4096


typedef enum {
    LOG_INFO,       
    LOG_ERROR,        
    LOG_WARNING,    
    LOG_DEBUG,      
    LOG_CLIENT,      
    LOG_ROOM,        
    LOG_FILE,        
    LOG_SERVER,
    LOG_JOIN,
    LOG_BROADCAST,
    LOG_WHISPER,
    LOG_LEAVE,
    LOG_SENDFILE 
} log_level_t;



typedef struct file_queue_item {
    char filename[MAX_FILENAME_LENGTH];
    char sender_username[17];
    char receiver_username[17];
    char *file_data;           
    size_t file_size;
    time_t created_time;
    int sender_socket;          
    int receiver_socket;        
} file_queue_item_t;

typedef struct {
    file_queue_item_t items[MAX_UPLOAD_QUEUE];
    int count;                   
    pthread_mutex_t mutex;        
} file_queue_t;

extern file_queue_t global_file_queue;


typedef struct {
    int client_socket;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
} client_thread_data_t;



typedef struct client_info {
    char username[17];                   
    int socket_fd;                        
    pthread_t thread_id;                  
    
    char current_room_name[33];           
    int current_room_index;               
    
    char client_ip[INET_ADDRSTRLEN];      
    int client_port;                      
    time_t login_time;                    
    
    char current_file_path[MAX_PATH_LENGTH];         

    int is_active;                        
    int is_uploading;                     
    int is_downloading;                   
    
    struct client_info *next; 
} client_info_t;





typedef struct room_info {
    char room_name[MAX_ROOM_NAME_LENGTH + 1];  
    time_t created_time;                       
    
    client_info_t *clients[MAX_CLIENTS_PER_ROOM];  
    int client_count;                              
    
    int total_messages_sent;                  
    time_t last_activity;                      
    
    pthread_mutex_t room_mutex;    

    struct room_info *next;                   
} room_info_t;



extern room_info_t *room_list_head;
extern int total_room_count;
extern pthread_mutex_t room_list_mutex;



extern client_info_t *client_list_head;
extern int active_client_count;
extern pthread_mutex_t client_list_mutex;



void init_clients(void);
void cleanup_clients(void);

client_info_t* add_client(const char *username, int socket_fd, pthread_t thread_id, 
                         const char *client_ip, int client_port, const char *file_path);
int remove_client(int socket_fd);
int remove_client_by_username(const char *username);

client_info_t* find_client_by_username(const char *username);
client_info_t* find_client_by_socket(int socket_fd);
client_info_t* find_client_by_thread(pthread_t thread_id);

void list_clients(void);
int count_clients(void);




void init_rooms(void);
void cleanup_rooms(void);

room_info_t* add_room(const char *room_name);
int remove_room(const char *room_name);

room_info_t* find_room(const char *room_name);
room_info_t* get_room_by_index(int index);
int get_room_index(const char *room_name);

void list_rooms(void);
int count_rooms(void);




int init_file_queue(void);
void cleanup_file_queue(void);
int add_to_file_queue(const char *filename, const char *sender, const char *receiver, 
                     char *file_data, size_t file_size, int sender_socket, int receiver_socket);
int remove_from_file_queue(int index);
int is_file_queue_full(void);
int get_file_queue_count(void);

int validate_file_extension(const char *filename);
int validate_file_size_limit(size_t file_size);

int receive_file_from_client(int client_socket, const char *filename, char **file_data, size_t *file_size);
int send_file_to_client(int client_socket, const char *filename, const char *sender, 
                       const char *file_data, size_t file_size);

int upload_file_to_server(const char *filename, const char *target_username);
int receive_file_from_server(const char *message);

void handle_sendfile_command(int client_socket, const char *file_args);


int initialize_server(int port);
void cleanup_server();

void setup_signal_handlers();
void handle_sigint(int sig);

int send_message(int client_socket, const char* message);
int receive_message(int client_socket, char* buffer, size_t buffer_size);

void *handle_client(void *arg);


int setup_client_connection(void *arg, char *client_ip, int *client_port);
int handle_client_login(int client_socket, pthread_t thread_id, 
                       const char *client_ip, int client_port);
int validate_username(const char *username);
void client_message_loop(int client_socket);
void process_client_command(int client_socket, const char *command);
void cleanup_client_connection(int client_socket);


void handle_join_command(int client_socket, const char *room_name);
void handle_leave_command(int client_socket);
void handle_broadcast_command(int client_socket, const char *message);
void handle_whisper_command(int client_socket, const char *whisper_args);
void handle_sendfile_command(int client_socket, const char *file_args);
void handle_exit_command(int client_socket);

void init_logging(void);
void cleanup_logging(void);
void log_message(log_level_t level, const char *format, ...);
const char* log_level_to_string(log_level_t level);

void shutdown_all_clients(void);
int count_active_threads(void);
void abort_all_file_transfers(void);
void notify_file_transfer_shutdown(void);

#endif // SERVER_HELPER_H