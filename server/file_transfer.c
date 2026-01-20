// file_transfer.c - Server-Side File Transfer with File Descriptors

#include "server_helper.h"
#include <sys/stat.h>
#include <fcntl.h>

file_queue_t global_file_queue;

static const char* ALLOWED_EXTENSIONS[] = {".txt", ".pdf", ".jpg", ".png" , ".mp4", NULL};



int init_file_queue(void) {
    global_file_queue.count = 0;
    
    // Initialize all file_data pointers to NULL
    for (int i = 0; i < MAX_UPLOAD_QUEUE; i++) {
        global_file_queue.items[i].file_data = NULL;
    }
    
    if (pthread_mutex_init(&global_file_queue.mutex, NULL) != 0) {
        perror("[FILE-QUEUE] Failed to initialize mutex");
        return -1;
    }
    
    printf("[FILE-QUEUE] File queue initialized (max %d items)\n", MAX_UPLOAD_QUEUE);
    return 0;
}

void cleanup_file_queue(void) {
    printf("[FILE-QUEUE] Starting file queue cleanup...\n");
    
    pthread_mutex_lock(&global_file_queue.mutex);
    
    // Free any remaining file data
    int freed_count = 0;
    size_t total_freed = 0;
    
    for (int i = 0; i < global_file_queue.count; i++) {
        if (global_file_queue.items[i].file_data) {
            total_freed += global_file_queue.items[i].file_size;
            free(global_file_queue.items[i].file_data);
            global_file_queue.items[i].file_data = NULL;
            freed_count++;
        }
    }
    
    if (freed_count > 0) {
        printf("[FILE-QUEUE] Freed %d file data blocks (%zu bytes total)\n", freed_count, total_freed);
    }
    
    global_file_queue.count = 0;
    
    pthread_mutex_unlock(&global_file_queue.mutex);
    pthread_mutex_destroy(&global_file_queue.mutex);
    
    printf("[FILE-QUEUE] File queue cleaned up (freed %d items, %zu bytes)\n", freed_count, total_freed);
}

int add_to_file_queue(const char *filename, const char *sender, const char *receiver,
                     char *file_data, size_t file_size, int sender_socket, int receiver_socket) {
    pthread_mutex_lock(&global_file_queue.mutex);
    
    if (global_file_queue.count >= MAX_UPLOAD_QUEUE) {
        pthread_mutex_unlock(&global_file_queue.mutex);
        return -1;  // Queue full
    }
    
    // Add to queue
    file_queue_item_t *item = &global_file_queue.items[global_file_queue.count];
    
    strncpy(item->filename, filename, sizeof(item->filename) - 1);
    item->filename[sizeof(item->filename) - 1] = '\0';
    
    strncpy(item->sender_username, sender, sizeof(item->sender_username) - 1);
    item->sender_username[sizeof(item->sender_username) - 1] = '\0';
    
    strncpy(item->receiver_username, receiver, sizeof(item->receiver_username) - 1);
    item->receiver_username[sizeof(item->receiver_username) - 1] = '\0';
    
    item->file_data = file_data;  // Transfer ownership of allocated memory
    item->file_size = file_size;
    item->created_time = time(NULL);
    item->sender_socket = sender_socket;
    item->receiver_socket = receiver_socket;
    
    global_file_queue.count++;
    
    printf("[FILE-QUEUE] Added: %s -> %s (%s, %zu bytes) [%d/%d]\n",
           sender, receiver, filename, file_size, global_file_queue.count, MAX_UPLOAD_QUEUE);
    
    pthread_mutex_unlock(&global_file_queue.mutex);
    return global_file_queue.count - 1;  // Return index
}

int remove_from_file_queue(int index) {
    pthread_mutex_lock(&global_file_queue.mutex);
    
    if (index < 0 || index >= global_file_queue.count) {
        pthread_mutex_unlock(&global_file_queue.mutex);
        return -1;
    }
    
    // Free the file data
    if (global_file_queue.items[index].file_data) {
        free(global_file_queue.items[index].file_data);
    }
    
    // Shift remaining items
    for (int i = index; i < global_file_queue.count - 1; i++) {
        global_file_queue.items[i] = global_file_queue.items[i + 1];
    }
    
    global_file_queue.count--;
    
    pthread_mutex_unlock(&global_file_queue.mutex);
    return 0;
}

int is_file_queue_full(void) {
    pthread_mutex_lock(&global_file_queue.mutex);
    int full = (global_file_queue.count >= MAX_UPLOAD_QUEUE);
    pthread_mutex_unlock(&global_file_queue.mutex);
    return full;
}

int get_file_queue_count(void) {
    pthread_mutex_lock(&global_file_queue.mutex);
    int count = global_file_queue.count;
    pthread_mutex_unlock(&global_file_queue.mutex);
    return count;
}

// ==========================================
// FILE VALIDATION
// ==========================================

int validate_file_extension(const char *filename) {
    if (!filename) return 0;
    
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;  // No extension
    
    for (int i = 0; ALLOWED_EXTENSIONS[i] != NULL; i++) {
        if (strcasecmp(dot, ALLOWED_EXTENSIONS[i]) == 0) {
            return 1;  // Valid extension
        }
    }
    
    return 0;  // Invalid extension
}

int validate_file_size_limit(size_t file_size) {
    return (file_size <= MAX_FILE_SIZE) ? 1 : 0;
}



int receive_file_from_client(int client_socket, const char *filename, char **file_data, size_t *file_size) {
    uint32_t network_size;
    ssize_t received = recv(client_socket, &network_size, sizeof(network_size), MSG_WAITALL);
    if (received != sizeof(network_size)) {
        printf("[FILE-RECV] Failed to receive file size from client\n");
        return -1;
    }
    
    *file_size = ntohl(network_size);
    printf("[FILE-RECV] Receiving file: %s (%zu bytes)\n", filename, *file_size);
    
    if (!validate_file_size_limit(*file_size)) {
        printf("[FILE-RECV] File too large: %zu bytes (max %d)\n", *file_size, MAX_FILE_SIZE);
        return -1;
    }
    
    *file_data = malloc(*file_size);
    if (!*file_data) {
        printf("[FILE-RECV] Failed to allocate memory for file data\n");
        return -1;
    }
    
    // Receive file data in chunks
    size_t total_received = 0;
    char *buffer_ptr = *file_data;
    
    while (total_received < *file_size) {
        size_t remaining = *file_size - total_received;
        size_t chunk_size = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        
        ssize_t chunk_received = recv(client_socket, buffer_ptr + total_received, chunk_size, 0);
        if (chunk_received <= 0) {
            printf("[FILE-RECV] Connection lost during transfer (received %zd)\n", chunk_received);
            free(*file_data);
            *file_data = NULL;
            return -1;
        }
        
        total_received += chunk_received;
        
        // Show progress
        int progress = (int)((total_received * 100) / *file_size);
        if (progress % 10 == 0 || total_received == *file_size) {
            printf("[FILE-RECV] Progress: %zu/%zu bytes (%d%%)\n", 
                   total_received, *file_size, progress);
        }
    }
    
    printf("[FILE-RECV] Successfully received: %s (%zu bytes)\n", filename, total_received);
    return 0;
}

int send_file_to_client(int client_socket, const char *filename, const char *sender,
                       const char *file_data, size_t file_size) {
    printf("[FILE-SEND] Sending file: %s (%zu bytes) to client\n", filename, file_size);
    
    // Send file download header message
    char header[512];
    snprintf(header, sizeof(header), "FILE_DOWNLOAD:%s:%zu:%s", filename, file_size, sender);
    if (send_message(client_socket, header) != 0) {
        printf("[FILE-SEND] Failed to send download header\n");
        return -1;
    }
    
    // Send file size
    uint32_t network_size = htonl((uint32_t)file_size);
    if (send(client_socket, &network_size, sizeof(network_size), 0) != sizeof(network_size)) {
        printf("[FILE-SEND] Failed to send file size\n");
        return -1;
    }
    
    // Send file data in chunks
    size_t total_sent = 0;
    const char *buffer_ptr = file_data;
    
    while (total_sent < file_size) {
        size_t remaining = file_size - total_sent;
        size_t chunk_size = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        
        ssize_t sent = send(client_socket, buffer_ptr + total_sent, chunk_size, 0);
        if (sent <= 0) {
            printf("[FILE-SEND] Connection lost during transfer (sent %zd)\n", sent);
            return -1;
        }
        
        total_sent += sent;
        
        // Show progress
        int progress = (int)((total_sent * 100) / file_size);
        if (progress % 10 == 0 || total_sent == file_size) {
            printf("[FILE-SEND] Progress: %zu/%zu bytes (%d%%)\n", 
                   total_sent, file_size, progress);
        }
    }
    
    printf("[FILE-SEND] Successfully sent: %s (%zu bytes)\n", filename, total_sent);
    return 0;
}


