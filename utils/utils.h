#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct server_parameter {
    int port;
};

struct client_parameter {
    char server_ip[64];
    int port;
};

void red(void);
void green(void);
void yellow(void);
void blue(void);
void magenta(void);
void cyan(void);
void white(void);
void reset(void);


int parse_client_args(int argc, char **argv, struct client_parameter *params);

int parse_server_args(int argc, char **argv, struct server_parameter *params);

#endif // UTILS_H