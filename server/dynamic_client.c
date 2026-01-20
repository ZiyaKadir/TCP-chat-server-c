#include "server_helper.h"

client_info_t *client_list_head = NULL;
int active_client_count = 0;
pthread_mutex_t client_list_mutex = PTHREAD_MUTEX_INITIALIZER;


void init_clients(void) {
    pthread_mutex_lock(&client_list_mutex);
    client_list_head = NULL;
    active_client_count = 0;
    pthread_mutex_unlock(&client_list_mutex);
    // printf("[CLIENT-INIT] Client list initialized\n");
}

void cleanup_clients(void) {
    // printf("[CLIENT-CLEANUP] Cleaning up all clients...\n");
    
    pthread_mutex_lock(&client_list_mutex);
    
    client_info_t *current = client_list_head;
    int cleanup_count = 0;
    
    while (current) {
        client_info_t *next = current->next;
        
        // printf("[CLIENT-CLEANUP] Cleaning up client '%s' (socket %d)\n", 
        //        current->username, current->socket_fd);
        
        if (current->socket_fd >= 0) {
            close(current->socket_fd);
        }
        
        free(current);
        cleanup_count++;
        current = next;
    }
    
    client_list_head = NULL;
    active_client_count = 0;
    
    pthread_mutex_unlock(&client_list_mutex);
    
    // printf("[CLIENT-CLEANUP] Cleaned up %d clients\n", cleanup_count);
}



client_info_t* add_client(const char *username, int socket_fd, pthread_t thread_id, 
                         const char *client_ip, int client_port, const char *file_path) {
    if (!username || socket_fd < 0) {
        return NULL;
    }
    
    // Allocate new client
    client_info_t *new_client = malloc(sizeof(client_info_t));
    if (!new_client) {
        red();
        perror("Failed to allocate memory for new client");
        reset();
        return NULL;
    }
    
    // Initialize client data
    strncpy(new_client->username, username, sizeof(new_client->username) - 1);
    new_client->username[sizeof(new_client->username) - 1] = '\0';
    
    new_client->socket_fd = socket_fd;
    new_client->thread_id = thread_id;
    
    // Clear room information
    new_client->current_room_name[0] = '\0';
    new_client->current_room_index = -1;
    
    // Set connection information
    if (client_ip) {
        strncpy(new_client->client_ip, client_ip, sizeof(new_client->client_ip) - 1);
        new_client->client_ip[sizeof(new_client->client_ip) - 1] = '\0';
    } else {
        strcpy(new_client->client_ip, "unknown");
    }
    new_client->client_port = client_port;
    new_client->login_time = time(NULL);
    
    // Set file path information
    if (file_path) {
        strncpy(new_client->current_file_path, file_path, sizeof(new_client->current_file_path) - 1);
        new_client->current_file_path[sizeof(new_client->current_file_path) - 1] = '\0';
    } else {
        strcpy(new_client->current_file_path, ".");  // Default to current directory
    }
    
    // Set status flags
    new_client->is_active = 1;
    new_client->is_uploading = 0;
    new_client->is_downloading = 0;
    
    // Add to linked list
    pthread_mutex_lock(&client_list_mutex);
    
    // Insert at head of list
    new_client->next = client_list_head;
    client_list_head = new_client;
    active_client_count++;
    
    // printf("[CLIENT-ADD] Added client '%s' (socket %d, path: %s). Total: %d\n",
    //        username, socket_fd, file_path ? file_path : ".", active_client_count);
    
    pthread_mutex_unlock(&client_list_mutex);
    
    return new_client;
}



int remove_client(int socket_fd) {
    pthread_mutex_lock(&client_list_mutex);
    
    client_info_t *current = client_list_head;
    client_info_t *previous = NULL;
    
    while (current) {
        if (current->socket_fd == socket_fd) {
            // Remove from list
            if (previous) {
                previous->next = current->next;
            } else {
                client_list_head = current->next;
            }
            
            // printf("[CLIENT-REMOVE] Removing client '%s' (socket %d)\n", 
            //        current->username, current->socket_fd);
            
            free(current);
            active_client_count--;
            
            // printf("[CLIENT-REMOVE] Client removed. Total: %d\n", active_client_count);
            
            pthread_mutex_unlock(&client_list_mutex);
            return 0;
        }
        
        previous = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&client_list_mutex);
    // printf("[CLIENT-WARNING] Client with socket %d not found\n", socket_fd);
    return -1;
}

int remove_client_by_username(const char *username) {
    if (!username) return -1;
    
    pthread_mutex_lock(&client_list_mutex);
    
    client_info_t *current = client_list_head;
    client_info_t *previous = NULL;
    
    while (current) {
        if (strcmp(current->username, username) == 0) {
            // Remove from list
            if (previous) {
                previous->next = current->next;
            } else {
                client_list_head = current->next;
            }
            
            // printf("[CLIENT-REMOVE] Removing client '%s' by username\n", username);
            
            free(current);
            active_client_count--;
            
            pthread_mutex_unlock(&client_list_mutex);
            return 0;
        }
        
        previous = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&client_list_mutex);
    return -1;
}


client_info_t* find_client_by_username(const char *username) {
    if (!username) return NULL;
    
    pthread_mutex_lock(&client_list_mutex);
    
    client_info_t *current = client_list_head;
    while (current) {
        if (current->is_active && strcmp(current->username, username) == 0) {
            pthread_mutex_unlock(&client_list_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&client_list_mutex);
    return NULL;
}

client_info_t* find_client_by_socket(int socket_fd) {
    pthread_mutex_lock(&client_list_mutex);
    
    client_info_t *current = client_list_head;
    while (current) {
        if (current->socket_fd == socket_fd) {
            pthread_mutex_unlock(&client_list_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&client_list_mutex);
    return NULL;
}

client_info_t* find_client_by_thread(pthread_t thread_id) {
    pthread_mutex_lock(&client_list_mutex);
    
    client_info_t *current = client_list_head;
    while (current) {
        if (pthread_equal(current->thread_id, thread_id)) {
            pthread_mutex_unlock(&client_list_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&client_list_mutex);
    return NULL;
}



void list_clients(void) {
    pthread_mutex_lock(&client_list_mutex);
    
    printf("\n=== CLIENT LIST (%d clients) ===\n", active_client_count);
    
    client_info_t *current = client_list_head;
    int index = 1;
    
    while (current) {
        if (current->is_active) {
            printf("%d. '%s' (socket %d, room: '%s', path: '%s')\n",
                   index++, current->username, current->socket_fd,
                   strlen(current->current_room_name) > 0 ? current->current_room_name : "none",
                   current->current_file_path);
        }
        current = current->next;
    }
    
    printf("===============================\n\n");
    
    pthread_mutex_unlock(&client_list_mutex);
}

int count_clients(void) {
    pthread_mutex_lock(&client_list_mutex);
    int count = active_client_count;
    pthread_mutex_unlock(&client_list_mutex);
    return count;
}


