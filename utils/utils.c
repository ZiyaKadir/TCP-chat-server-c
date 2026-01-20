#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int parse_client_args(int argc, char **argv, struct client_parameter *params) {
    if (argc != 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        return -1;
    }

    // Store server IP
    strncpy(params->server_ip, argv[1], sizeof(params->server_ip) - 1);
    params->server_ip[sizeof(params->server_ip) - 1] = '\0'; // Ensure null termination

    // Parse port number
    params->port = atoi(argv[2]);
    if (params->port <= 0 || params->port > 65535) {
        printf("Invalid port number. Must be a positive integer between 1 and 65535.\n");
        return -1;
    }

    return 0;
}

int parse_server_args(int argc, char **argv, struct server_parameter *params) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return -1;
    }

    // Parse port number
    params->port = atoi(argv[1]);
    if (params->port <= 0 || params->port > 65535) {
        printf("Invalid port number. Must be a positive integer between 1 and 65535.\n");
        return -1;
    }

    return 0;
}

// Color function implementations
void red(void) {
    printf("\033[0;31m");
}

void green(void) {
    printf("\033[0;32m");
}

void yellow(void) {
    printf("\033[0;33m");
}

void blue(void) {
    printf("\033[0;34m");
}

void magenta(void) {
    printf("\033[0;35m");
}

void cyan(void) {
    printf("\033[0;36m");
}

void white(void) {
    printf("\033[0;37m");
}

void reset(void) {
    printf("\033[0m");
}