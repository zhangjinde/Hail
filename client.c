#include <arpa/inet.h>      // struct in_addr, inet_ntop(), etc.
#include <errno.h>          // Standard error codes 
#include <fcntl.h>          // open()
#include <netdb.h>          // getaddrinfo()
// #include <netinet/in.h>     // struct sockaddr_in and struct in_addr
#include <stdbool.h>        // 'true' and 'false' literals
#include <stdio.h>          // fprintf(), etc.
#include <stdlib.h>         // malloc(), calloc(), etc.
#include <string.h>         // strchr(), etc.
#include <sys/socket.h>     // Sockets API
#include <sys/stat.h>       // stat()
#include <sys/types.h>      // Standard types
#include <unistd.h>         // Standard system calls

#include "hail.h"

// Uncomment these header-includes if we need to 
// build and work with the raw networking structs

int main(int argc, char* argv[]) 
{
    // argc is the argument count, the length of argv[]
    // argv is the argument variable array
    // argc is at least 1, as argv[0] is the program name

    // Avoid magic numbers
    const char* HAIL_SERVER = {0};
    const char* HAIL_PORT   = {0};
    const char* FILE_NAME   = {0};

    // Need at least 'hostname port filename'
    // For formatting multi-line literal strings,
    // see: http://stackoverflow.com/questions/1135841/c-multiline-string-literal
    if (argc < 4) {
        printf(
            "\nUsage: \t%s hostname portnumber filename [OPTIONS]\n\n"
            "Send a message another endpoint using the Hail protocol.\n\n"
            "Options:\n"
            "-l L, --loss L     Simulate message loss with probability L in [0,1]\n"
            "-c C, --corrupt C  Simulate message corruption with probability C in [0,1]\n"
            "-s,   --silent     Run silently without activity output to stdout or stderr\n\n", 
            argv[0]
        );

        return EXIT_FAILURE;
    }
    // Handle special options first, so that we can use them later
    // TODO: Use getopt() to avoid this counting madness
    // TODO: Define options booleans here
    else if (argc == 7) {
        printf("Options not yet implemented!\n");
        return EXIT_FAILURE;
    }

    // We're here, so enough options were passed in
    HAIL_SERVER = argv[1];
    HAIL_PORT   = argv[2];
    FILE_NAME   = argv[3];

    // 'hints' is an addrinfo that's given to 
    // getaddrinfo() to specify parameters
    struct addrinfo params; 
    memset(&params, 0, sizeof(struct addrinfo));

    // Use a UDP datagram socket type
    // 'ai' is 'addrinfo'.
    params.ai_socktype = SOCK_DGRAM;
    // TODO: Generalize to IPv4 or IPv6
    params.ai_family = AF_INET;

    // argv[1] should have server name
    int status;
    struct addrinfo* results;
    // int getaddrinfo(const char *hostname, 
    //                 const char *servname, 
    //                 const struct addrinfo *hints, 
    //                 struct addrinfo **res);
    status = getaddrinfo(HAIL_SERVER, HAIL_PORT, &params, &results);
    if (status < 0) {
        // gai_strerror() converts error codes to messages
        // See: http://linux.die.net/man/3/gai_strerror
        fprintf(stderr, "[ERROR]: getaddrinfo() failed: %s\n", gai_strerror(status));
        return EXIT_FAILURE;
    }

    // 'results' now points to a linked-list of struct addrinfo
    // We need to find out which one of them lets us build a socket
    int socketFD = -1;
    struct addrinfo* p = NULL; 
    for (p = results; p != NULL; p = p->ai_next) {
        // int socket(int domain, int type, int protocol)
        // This is where we would have once used PF_INET
        socketFD = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (socketFD != -1) {
            // Successfully made a socket
            break;
        }
    }

    // Failed to make a working socket, so p hit end-of-list
    if (p == NULL) {
        fprintf(stderr, "[ERROR]: getaddrinfo() gave no working sockets\n");
        return EXIT_FAILURE;
    }

    // We only need the working information
    results = p;

    // int open(const char *pathname, int flags)
    int fileDescrip = open(FILE_NAME, O_RDONLY);
    if (fileDescrip < 0) {
        fprintf(stderr, "[ERROR]: open() of %s failed\n", FILE_NAME);
        return EXIT_FAILURE;
    }

    // CERT recommends against fseek() and ftell() for determining file size
    // See: https://is.gd/mwJDph-
    struct stat fileInfo;
    // int stat(const char *pathname, struct stat *buf)
    if (stat(FILE_NAME, &fileInfo) < 0) {
        fprintf(stderr, "[ERROR]: stat() on %s failed\n", FILE_NAME);
        return EXIT_FAILURE;
    }

    // Not a regular file
    if (! S_ISREG(fileInfo.st_mode)) {
        fprintf(stderr, "[ERROR]: stat() on %s: not a regular file\n", FILE_NAME);
        return EXIT_FAILURE;
    }

    // Read file into buffer
    off_t fileSize = fileInfo.st_size;
    char* fileBuffer = (char *)malloc(sizeof(char) * fileSize);
    if (read(fileDescrip, fileBuffer, fileSize) < 0) {
        fprintf(stderr, "[ERROR]: read() of %s into buffer failed\n", FILE_NAME);
        return EXIT_FAILURE;
    }

    // Outside of loop, to avoid creating multiple packets
    // We'd lose track of old ones whenever we overwrote addresses otherwise
    hail_packet_t* packet = (hail_packet_t*)malloc(sizeof(hail_packet_t));
    size_t packet_size = sizeof(hail_packet_t);

    // For now at least, we'll ignore the wasted data space on handshake packets
    char file_data[HAIL_CONTENT_SIZE]; 
    memset(file_data, 0, HAIL_CONTENT_SIZE);

    // sendto() expects a buffer, not a struct
    char* send_buffer = (char*)malloc(packet_size);
    memcpy(send_buffer, packet, packet_size);

    // For later receiving of packets
    char* recv_buffer = (char*)malloc(packet_size);
    memset(recv_buffer, 0, packet_size);

    // Loop until a three-way handshake has been established
    bool is_connected = false;
    while (! is_connected) {
        // Prepare initial Hail packet to start handshake
        char seq_num = 0;
        char ack_num = 0;
        hail_control_code_t control = SYN;
        char version = 0;
        uint64_t file_size = fileSize;

        // Use sendto() rather than bind() + send() as this
        // is a one-time shot (for now; later we'll break up
        // the file into different chunks)
        status = construct_hail_packet(
            packet,
            seq_num,
            ack_num, 
            control, 
            version,
            file_size,
            file_data
        );

        if (status < 0) {
            fprintf(stderr, "construct_hail_packet() failed! [Line: %d]\n", __LINE__);
        }

        status = sendto(
            socketFD,              // int sockfd
            packet,                // const void* msg
            packet_size,           // int len
            0,                     // unsigned int flags
            results->ai_addr,      // const struct sockaddr* to; we set results = p earlier
            results->ai_addrlen    // socklen_t tolen
        );  

        if (status < 0) {
            char IP4address[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, results->ai_addr, IP4address, results->ai_addrlen);
            fprintf(stderr, "[ERROR]: SYN sendto() %s of %s failed\n", IP4address, FILE_NAME);

            return EXIT_FAILURE;
        }

        // Received SYN ACK?
        // Filled by recvfrom(), as OS finds out source address
        // from headers in packets
        struct sockaddr server_addr;
        socklen_t server_addr_len = sizeof(struct sockaddr);
        int bytes_received = 0;

        bytes_received = recvfrom(
            socketFD,        // int sockfd; same socket for some connection
            recv_buffer,     // void* buf
            packet_size,     // int len
            0,               // unsigned int flags
            &server_addr,    // Filled by recvfrom(), as OS finds out source address
            &server_addr_len // from headers in packets
        );

        hail_packet_t recv_packet;
        memset(&recv_packet, 0, packet_size);
        // TODO: Unpack to see if it's a SYN ACK
        status = unpack_hail_packet(
            recv_buffer, 
            &recv_packet
        );

        // Server SYN ACK received; send final ACK
        if (recv_packet.control == SYN_ACK) {
            seq_num = recv_packet.seq_num + 1;
            ack_num = recv_packet.seq_num;
            control = ACK;
            version = 0;
            memset(file_data, 0, HAIL_CONTENT_SIZE);

            status = sendto(
                socketFD,              // int sockfd
                packet,                // const void* msg
                packet_size,           // int len
                0,                     // unsigned int flags
                results->ai_addr,      // const struct sockaddr* to; we set results = p earlier
                results->ai_addrlen    // socklen_t tolen
            ); 
            if (status < 0) {
                char IP4address[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, results->ai_addr, IP4address, results->ai_addrlen);
                fprintf(stderr, "[ERROR]: ACK sendto() %s of %s failed\n", IP4address, FILE_NAME);

                return EXIT_FAILURE;
            }
            else {
                is_connected = true;
                break;
            }
        }
    }
    

    // TODO: Send file in chunks. Update curPos!
    // Keep track of where we are within the buffer
    // size_t curPos = 0;

    // TODO: Handle sequence numbers

    // Need to free up 'results' and everything else
    freeaddrinfo(results);
    close(socketFD);
    free(packet);
    free(send_buffer);
    free(fileBuffer);

    return EXIT_SUCCESS;
}