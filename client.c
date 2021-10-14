#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <getopt.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netdb.h>
#include "zlib.h"

#define BUF_SIZE 1024

struct termios original_attributes;
struct termios new_attributes;
char in[BUF_SIZE];
int sockfd;
struct sockaddr_in serv_addr;
struct hostent *server;
int logOpt = 0;
int log_fd = 0;
int compressOpt = 0;
z_stream infstream;
z_stream defstream;

void error(char *msg) {
    perror(msg);
    exit(1);
}

void cleanup_ends() {
    deflateEnd(&defstream);
    inflateEnd(&infstream);
}

void setup_zlib() {

    defstream.zalloc = NULL;
    defstream.zfree = NULL;
    defstream.opaque = NULL;
    if (deflateInit(&defstream, Z_DEFAULT_COMPRESSION) < 0) {
        fprintf(stderr, "Failed to initialize compression.\n");
        exit(1);
    }

    infstream.zalloc = NULL;
    infstream.zfree = NULL;
    infstream.opaque = NULL;
    if (inflateInit(&infstream) < 0) {
        fprintf(stderr, "Failed to initialize compression.\n");
        exit(1);
    }
    atexit(cleanup_ends);
}

int compression(int bytes, int comp_inSize, char *in, char *out) {

    int compressedBytes; // in case there is not enough space
    defstream.avail_in = (uInt)bytes;
    defstream.next_in = (Bytef *)out;
    defstream.avail_out = comp_inSize;
    defstream.next_out = (Bytef *)in; // pass by pointer
    do {
        int def = deflate(&defstream, Z_SYNC_FLUSH);
        if (def == Z_STREAM_ERROR) {
            fprintf(stderr, "Inconsistent stream state: %s", defstream.msg);
            exit(1);
        }
    } while (defstream.avail_in > 0);
    compressedBytes = comp_inSize - defstream.avail_out;
    return compressedBytes;
} // make sure to choose the appropriate ZSYNC option

int decompression(int bytes, int decomp_inSize, char *in, char *out) {

    int decompressedBytes;
    infstream.avail_in = (uInt)bytes;
    infstream.next_in = (Bytef *)out;
    infstream.avail_out = decomp_inSize;
    infstream.next_out = (Bytef *)in; // pass by pointer
    do {
        int inf = inflate(&infstream, Z_SYNC_FLUSH);
        if (inf == Z_STREAM_ERROR) {
            fprintf(stderr, "Inconsistent stream state: %s", infstream.msg);
            exit(1);
        }
    } while (infstream.avail_in > 0);
    decompressedBytes = decomp_inSize - infstream.avail_out;
    return decompressedBytes;
}

void read_write() { // from keyboard to socket

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN | POLLHUP | POLLERR;

    fds[1].fd = sockfd;
    fds[1].events = POLLIN | POLLHUP | POLLERR;

    ssize_t bytes;
    char out[BUF_SIZE];
    char carriage[2] = {'\r', '\n'};

    if (compressOpt == 1) {

        setup_zlib();
    }

    for (;;) {

        poll(fds, 2, 0);

        if (fds[0].revents & POLLIN) {

            bzero(in, BUF_SIZE);
            bytes = read(fds[0].fd, &in, 1023);
            if (bytes < 0)
                error("ERROR reading from stdin");

            for (int i = 0; i < bytes; i++) {
                char curr = in[i];
                switch (curr) {
                case '\r':
                case '\n':
                    out[i] = carriage[1];
                    write(STDOUT_FILENO, &carriage[0], sizeof(char));
                    write(STDOUT_FILENO, &carriage[1], sizeof(char));
                    if (!compressOpt) {
                        write(sockfd, &carriage[1], sizeof(char));
                    }
                    break;
                default:
                    out[i] = curr;
                    write(STDOUT_FILENO, &curr, sizeof(char));
                    if (!compressOpt) {
                        write(sockfd, &curr, sizeof(char));
                    }
                    break;
                }
            }

            if (compressOpt == 1) {
                char compressionTemp[BUF_SIZE];
                memcpy(compressionTemp, out, bytes); // copy in for compression
                bytes = compression(bytes, BUF_SIZE, out, compressionTemp);
                bytes = write(sockfd, out, bytes);
                if (bytes < 0)
                    error("ERROR writing to socket");
            }

            if (logOpt) {
                write(log_fd, "SENT 1 bytes: ", 14);
                write(log_fd, &out, bytes);
                write(log_fd, &carriage[1], sizeof(char));
            }
        }

        if (fds[0].fd & (POLLHUP | POLLERR)) {
            fprintf(stderr, "Server shut down!\n");
            exit(1);
        }

        if (fds[1].revents & POLLIN) {

            bzero(in, BUF_SIZE);
            bytes = read(fds[1].fd, &in, 1023);
            if (bytes < 0)
                error("ERROR reading from socket");

            if (logOpt == 1) {
                dprintf(log_fd, "RECEIVED %ld bytes: ", bytes);
                write(log_fd, &in, bytes);
                write(log_fd, &carriage[1], sizeof(char));
            }

            if (compressOpt == 1) {

                memcpy(out, in, bytes);
                bytes = decompression(bytes, BUF_SIZE, in, out);
            }

            for (int i = 0; i < bytes; i++) {
                char curr = in[i];
                switch (curr) {
                case '\r':
                case '\n':
                    write(STDOUT_FILENO, &carriage[0], sizeof(char));
                    write(STDOUT_FILENO, &carriage[1], sizeof(char));
                    break;
                default:
                    write(STDOUT_FILENO, &curr, sizeof(char));
                    break;
                }
            }
        }

        if (fds[1].revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "Server shut down!\n");
            exit(1);
        }
    }
}

void save_terminal_attributes() {
    int result = tcgetattr(STDIN_FILENO, &original_attributes);
    if (result < 0) {
        fprintf(stderr, "Error in getting attributes. Error: %d, Message: %s\n", errno, strerror(errno));
        exit(1);
    }
}

void reset() {
    if (tcsetattr(STDIN_FILENO, TCSANOW, &original_attributes) < 0) {
        fprintf(stderr, "Could not set the attributes\n");
        exit(1);
    }
}

void set_input_mode() {
    save_terminal_attributes();
    atexit(reset);
    tcgetattr(STDIN_FILENO, &new_attributes);
    new_attributes.c_iflag = ISTRIP;
    new_attributes.c_oflag = 0;
    new_attributes.c_lflag = 0;
    int res = tcsetattr(STDIN_FILENO, TCSANOW, &new_attributes);
    if (res < 0) {
        fprintf(stderr, "Error with setting attributes. Error: %d, Message: %s\n", errno, strerror(errno));
        exit(1);
    }
}

int main(int argc, char *argv[]) {

    static struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"log", required_argument, NULL, 'l'},
        {"compress", no_argument, NULL, 'c'},
        {0, 0, 0, 0}};

    int portno = 0;
    int pflag = 0;
    int opt;

    while ((opt = getopt_long(argc, argv, "p:lc", options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            pflag = 1;
            portno = atoi(optarg);
            break;
        case 'l':
            logOpt = 1;
            log_fd = creat(optarg, S_IRWXU);
            if (log_fd < 0) {
                fprintf(stderr, "Unable to create log file. Error: %d, Message: %s\n", errno, strerror(errno));
                exit(1);
            }
            break;
        case 'c':
            compressOpt = 1;
            break;
        default:
            fprintf(stderr, "Incorrect argument: correct usage is ./client --port=portno [--log=filename] [--compress] \n");
            exit(1);
        }
    }

    if (!pflag) {
        fprintf(stderr, "port not specified\n");
        exit(1);
    }

    set_input_mode();

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    server = gethostbyname("localhost");
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host\n");
        exit(0);
    }

    bzero((char *)&serv_addr, sizeof(serv_addr)); // instead of bzero use memset
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);

    // memcpy((char *)&serv_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
    bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("Error in establishing connection.\n");

    read_write();

    exit(0);
}
