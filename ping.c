#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <stdlib.h> 
#include <fcntl.h> 
#include <time.h> 
#include <signal.h>
#include <getopt.h> 
#include "../header/rblox-api.h"
#define max_value 1400
volatile sig_atomic_t stop_flag = 0;

typedef struct {
    char *side;
    char *dif_name;
    int packet_size;
    int interval_size;
    int packet_amount;
} PingParams;

typedef struct {
    int transmitted_packets;
    int received_packets;
    double total_latency;
    double mode;
    int lost_packet;
} PingStats;

PingStats stats;

typedef struct LatencyNode {
    double latency;
    struct LatencyNode* next;
} LatencyNode;

LatencyNode* latency_head = NULL;


int open_dif() {
    int open_fd = rbx_open();
    if (open_fd < 0) {
        perror("Failed to rbx_open()");
        return -1;
    }

    return open_fd;
}

int register_dif(int open_fd, char *dif_name, char *name) {
    int reg_stat = rbx_register(open_fd, dif_name, name, 0);

    if (reg_stat) {
        printf("%s failed to rbx_register().\n", name);
        close(open_fd);

        return -1;
    } else {
        printf("%s registered on DIF.\n", name);
        
        return reg_stat;
    }
}

int accept_allocation(int open_fd, char **server_name) {
    struct pollfd poll_fds[1];

    poll_fds[0].fd = open_fd;
    poll_fds[0].events = POLLIN;
    poll_fds[0].revents = 0;

    while (1) {
        int events = poll(poll_fds, 1, 10000); 
        if (events > 0) {
            if (poll_fds[0].revents & POLLIN) {
                int accept_fd = rbx_accept(open_fd, server_name, NULL, 0);
                if (accept_fd < 0) {
                    perror("Server failed to rbx_accept().");

                    return -1;
                } else {
                    printf("Flow accepted %d.\n", accept_fd);

                    return accept_fd; 
                }
            }
        } else if (events == -1) {
            perror("Poll error");

            return -1; 
        } else {
            printf("Timeout while waiting for connection.\n");

            return -1;
        }
    }
}

int allocation(char *dif_name, char *server_name, char *client_name) {
    int allocated_fd = rbx_alloc(dif_name, client_name, server_name, NULL, 0);
    if (allocated_fd <=  0) {
        printf("Client failed to allocate with server.\n");

        return -1;
    } else {
        printf("Successful allocation between client and server.\n");

        return allocated_fd;
    }
}

clock_t write_fd(int allocated_fd, int packet_size,int interval_size) { 
    clock_t start;
    int bytes_written = 0;

    char *buffer = malloc(packet_size);
    if (buffer == NULL) {
        perror("Failed to allocate memory.");

        return -1;
    }
    memset(buffer, 'a', packet_size - 1);
    buffer[packet_size - 1] = '\0';
    printf("Starting to write\n");
    start = clock();
    bytes_written = write(allocated_fd, buffer, packet_size);
    stats.transmitted_packets++;
    if (bytes_written < 0) {
        perror("Write failed.");
        free(buffer);

        return -1;
    }
    sleep(interval_size);

    free(buffer);

    return start;
}

clock_t read_fd(int allocated_fd, int packet_size) {
    char *buffer = malloc(packet_size);
    clock_t start, end;
    if (buffer == NULL) {
        perror("Failed to allocate memory for buffer.");

        return -1;
    }

    while (1) {
        start = clock();
        ssize_t bytes_read = read(allocated_fd, buffer, packet_size);
        if (bytes_read < 0) {
            //perror("Read failed.");
            //return -1;
        }
        else {
            end = clock();
            double read_time = (double)(end - start) / CLOCKS_PER_SEC;
            if (read_time > 2.0) {
                printf("Packet lost.\n");
                free(buffer);
                stats.lost_packet++;
                return -1;
            }
            //printf("Read %zd bytes: %s\n", bytes_read, buffer); 
            stats.received_packets++;
            free(buffer);

            return end;
        }
    }
}

void latency_check(clock_t start_time, clock_t end_time) {
    double latency = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    stats.total_latency += latency;
    
    LatencyNode* new_node = (LatencyNode*)malloc(sizeof(LatencyNode));
    if (new_node == NULL) {
        perror("Memory allocation failed for latency node.");
        exit(EXIT_FAILURE);
    }
    
    new_node->latency = latency;
    new_node->next = NULL;

    if (latency_head == NULL) {
        latency_head = new_node;
    } else {
        LatencyNode* current = latency_head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }
    
    printf("Latency is %f seconds\n", latency);
}

double find_mode(LatencyNode* head) {
    if (head == NULL) {
        return 0; 
    }
    
    double mode = head->latency;
    int max_count = 0;

    LatencyNode* current = head;
    while (current != NULL) {
        double current_latency = current->latency;
        int count = 0;

        LatencyNode* temp = head;
        while (temp != NULL) {
            if (temp->latency == current_latency)
                count++;
            temp = temp->next;
        }

        if (count > max_count) {
            max_count = count;
            mode = current_latency;
        }

        current = current->next;
    }

    stats.mode = mode;

    return mode;
}

void print_statistic() {

    double mode = find_mode(latency_head);
    printf("\nPing statistics:\n");
    printf("%d packets transmitted\n", stats.transmitted_packets);
    printf("%d packets received\n", stats.received_packets);
    printf("%.2f%% packet loss\n", stats.lost_packet / (float)stats.transmitted_packets * 100);
    printf("Average latency: %f seconds\n", stats.total_latency / stats.received_packets);
    printf("Mode of latencies: %f seconds\n", mode);
}

void handle_sigint() {
    
    stop_flag = 1;
    print_statistic();
    exit(EXIT_SUCCESS);

}
int client_mode(PingParams *params) {

    char *client_name = "a";
    char *server_name = "b";
    clock_t start_time;
    clock_t end_time;

    signal(SIGINT, handle_sigint); 
    int allocated_fd = allocation(params->dif_name, server_name, client_name);
        if (allocated_fd < 0)
            return -1;
    if (params->packet_amount){
        while (params->packet_amount){
            start_time = write_fd(allocated_fd,params->packet_size, params->interval_size);
            end_time = read_fd(allocated_fd, params->packet_size);
            latency_check(start_time, end_time);
            sleep(params->interval_size); 
            params->packet_amount--;
            //close (allocated_fd);
        }
    handle_sigint();
    }
    else{
        while (1) {
            while (!stop_flag) {
                start_time = write_fd(allocated_fd, params->packet_size, params->interval_size);
                end_time = read_fd(allocated_fd, params->packet_size);
                latency_check(start_time, end_time);
                sleep(params->interval_size);
                }
            }
         }

    return 0;
}

int server_mode(PingParams *params) {

    char *server_name = "b";
    int fd = open_dif();
    if (register_dif(fd, params->dif_name, server_name) == -1)

        return -1;
    int accept_fd = accept_allocation(fd, &server_name);
    if (accept_fd == -1)

        return -1;

    while(1){
    read_fd(accept_fd, params->packet_size);
    write_fd(accept_fd, params->packet_size,params->interval_size);
    }

    return 0;
}

int param_options(int argc, char **argv, PingParams *params) {

    static struct option options[] = {
        {"side", required_argument, 0, 'n'},
        {"dif", required_argument, 0, 'd'},
        {"size", required_argument, 0, 's'},
        {"amount", required_argument, 0, 'a'},
        {"interval", required_argument, 0, 'i'},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "n:d:s:a:i:", options, NULL)) != -1) {
        switch (opt) {
            case 'n':
                params->side = optarg;
                break;
            case 'd':
                params->dif_name = optarg;
                break;
            case 's':
                params->packet_size = atoi(optarg);
                if (params->packet_size > max_value || params->packet_size < 0) {
                    printf("Packet size must be between 0 and %d.\n", max_value);

                    return -1;
                }
                break;
            case 'a':
                params->packet_amount = atoi(optarg);
                if (params->packet_amount < 0) {
                    printf("Packet amount cannot be negative.\n");

                    return -1;
                }
                break;
            case 'i':
                params->interval_size = atoi(optarg);
                if (params->interval_size < 0) {
                    printf("Interval size cannot be negative.\n");

                    return -1;
                }
                break;
            default:
                fprintf(stderr, "Usage: %s --side <side> --dif <dif_name> [--size <size>] [--amount <amount>] [--interval <interval>]\n", argv[0]);

                return -1;
        }
    }
    
     if (params->side == NULL || params->dif_name == NULL) {
        fprintf(stderr, "Side of the application and DIF name are mandatory.\n");

        return -1;
    }else if (params->packet_size == 0 || params->interval_size == 0){
        fprintf(stderr,"Please provide valide number.\n");

        return - 1;
    }

    return 0;
}
int main(int argc, char **argv) {

    if (argc < 2) {
        printf("Invalid number of parameters.\n");
        return -1;
    }
    PingParams params = {
        .packet_size = 64,
        .interval_size = 1,
        .packet_amount = 0
    };

    stats.transmitted_packets = 0;
    stats.received_packets = 0;
    stats.total_latency = 0.0;
    stats.mode = 0;

    if (param_options(argc, argv, &params) != 0) {
        
        return -1;
    }
    if (strcmp(params.side, "c") == 0) {

        return client_mode(&params);
    }else if (strcmp(params.side, "s") == 0){

            return server_mode(&params);
    } else {
        printf("Invalid side of the application. Please specify 'c' for client or 's' for server.\n");

        return -1;
    }

    return 0;
}
