#include "server_helper.h"



room_info_t *room_list_head = NULL;
int total_room_count = 0;
pthread_mutex_t room_list_mutex = PTHREAD_MUTEX_INITIALIZER;



void init_rooms(void) {
    pthread_mutex_lock(&room_list_mutex);
    room_list_head = NULL;
    total_room_count = 0;
    pthread_mutex_unlock(&room_list_mutex);
    printf("[ROOM-INIT] Room list initialized\n");
}

void cleanup_rooms(void) {
    printf("[ROOM-CLEANUP] Cleaning up all rooms...\n");
    
    pthread_mutex_lock(&room_list_mutex);
    
    room_info_t *current = room_list_head;
    int cleanup_count = 0;
    
    while (current) {
        room_info_t *next = current->next;
        
        printf("[ROOM-CLEANUP] Cleaning up room '%s'\n", current->room_name);
        
        // Destroy room mutex
        pthread_mutex_destroy(&current->room_mutex);
        
        free(current);
        cleanup_count++;
        current = next;
    }
    
    room_list_head = NULL;
    total_room_count = 0;
    
    pthread_mutex_unlock(&room_list_mutex);
    
    printf("[ROOM-CLEANUP] Cleaned up %d rooms\n", cleanup_count);
}



room_info_t* add_room(const char *room_name) {
    if (!room_name) {
        return NULL;
    }
    
    pthread_mutex_lock(&room_list_mutex);
    
    // Check if room already exists
    room_info_t *existing = room_list_head;
    while (existing) {
        if (strcmp(existing->room_name, room_name) == 0) {
            pthread_mutex_unlock(&room_list_mutex);
            printf("[ROOM-INFO] Room '%s' already exists\n", room_name);
            return existing;  // Return existing room
        }
        existing = existing->next;
    }
    
    // Allocate new room
    room_info_t *new_room = malloc(sizeof(room_info_t));
    if (!new_room) {
        pthread_mutex_unlock(&room_list_mutex);
        perror("[ROOM-ERROR] Failed to allocate memory for new room");
        return NULL;
    }
    
    // Initialize room data
    strncpy(new_room->room_name, room_name, sizeof(new_room->room_name) - 1);
    new_room->room_name[sizeof(new_room->room_name) - 1] = '\0';
    
    new_room->created_time = time(NULL);
    new_room->client_count = 0;
    new_room->total_messages_sent = 0;
    new_room->last_activity = new_room->created_time;
    
    // Initialize client array
    for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
        new_room->clients[i] = NULL;
    }
    
    // Initialize room mutex
    if (pthread_mutex_init(&new_room->room_mutex, NULL) != 0) {
        pthread_mutex_unlock(&room_list_mutex);
        free(new_room);
        perror("[ROOM-ERROR] Failed to initialize room mutex");
        return NULL;
    }
    
    // Add to linked list
    new_room->next = room_list_head;
    room_list_head = new_room;
    total_room_count++;
    
    printf("[ROOM-ADD] Added room '%s'. Total: %d\n", room_name, total_room_count);
    
    pthread_mutex_unlock(&room_list_mutex);
    
    return new_room;
}



int remove_room(const char *room_name) {
    if (!room_name) return -1;
    
    pthread_mutex_lock(&room_list_mutex);
    
    room_info_t *current = room_list_head;
    room_info_t *previous = NULL;
    
    while (current) {
        if (strcmp(current->room_name, room_name) == 0) {
            // Check if room is empty
            pthread_mutex_lock(&current->room_mutex);
            if (current->client_count > 0) {
                pthread_mutex_unlock(&current->room_mutex);
                pthread_mutex_unlock(&room_list_mutex);
                printf("[ROOM-ERROR] Cannot remove non-empty room '%s'\n", room_name);
                return -1;
            }
            pthread_mutex_unlock(&current->room_mutex);
            
            // Remove from list
            if (previous) {
                previous->next = current->next;
            } else {
                room_list_head = current->next;
            }
            
            printf("[ROOM-REMOVE] Removing room '%s'\n", room_name);
            
            // Destroy mutex and free room
            pthread_mutex_destroy(&current->room_mutex);
            free(current);
            total_room_count--;
            
            printf("[ROOM-REMOVE] Room removed. Total: %d\n", total_room_count);
            
            pthread_mutex_unlock(&room_list_mutex);
            return 0;
        }
        
        previous = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&room_list_mutex);
    printf("[ROOM-WARNING] Room '%s' not found\n", room_name);
    return -1;
}



room_info_t* find_room(const char *room_name) {
    if (!room_name) return NULL;
    
    pthread_mutex_lock(&room_list_mutex);
    
    room_info_t *current = room_list_head;
    while (current) {
        if (strcmp(current->room_name, room_name) == 0) {
            pthread_mutex_unlock(&room_list_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&room_list_mutex);
    return NULL;
}

room_info_t* get_room_by_index(int index) {
    if (index < 0) return NULL;
    
    pthread_mutex_lock(&room_list_mutex);
    
    room_info_t *current = room_list_head;
    int current_index = 0;
    
    while (current) {
        if (current_index == index) {
            pthread_mutex_unlock(&room_list_mutex);
            return current;
        }
        current = current->next;
        current_index++;
    }
    
    pthread_mutex_unlock(&room_list_mutex);
    return NULL;
}

int get_room_index(const char *room_name) {
    if (!room_name) return -1;
    
    pthread_mutex_lock(&room_list_mutex);
    
    room_info_t *current = room_list_head;
    int index = 0;
    
    while (current) {
        if (strcmp(current->room_name, room_name) == 0) {
            pthread_mutex_unlock(&room_list_mutex);
            return index;
        }
        current = current->next;
        index++;
    }
    
    pthread_mutex_unlock(&room_list_mutex);
    return -1;
}



void list_rooms(void) {
    pthread_mutex_lock(&room_list_mutex);
    
    printf("\n=== ROOM LIST (%d rooms) ===\n", total_room_count);
    
    room_info_t *current = room_list_head;
    int index = 1;
    
    while (current) {
        pthread_mutex_lock(&current->room_mutex);
        
        printf("%d. '%s' (%d/%d clients)\n",
               index++, current->room_name, current->client_count, MAX_CLIENTS_PER_ROOM);
        
        // List clients in room
        if (current->client_count > 0) {
            printf("   Clients: ");
            for (int i = 0; i < MAX_CLIENTS_PER_ROOM; i++) {
                if (current->clients[i] && current->clients[i]->is_active) {
                    printf("'%s' ", current->clients[i]->username);
                }
            }
            printf("\n");
        }
        
        pthread_mutex_unlock(&current->room_mutex);
        current = current->next;
    }
    
    printf("========================\n\n");
    
    pthread_mutex_unlock(&room_list_mutex);
}

int count_rooms(void) {
    pthread_mutex_lock(&room_list_mutex);
    int count = total_room_count;
    pthread_mutex_unlock(&room_list_mutex);
    return count;
}


