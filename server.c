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

int fd0[2], fd1[2];
pid_t pid;
int compressOpt;
int sockfd, newsockfd;
z_stream infstream;
z_stream defstream;

void error(char *msg) {
    perror(msg);
    exit(1);
}

void exit_msg() {
    int status;
    waitpid(pid, &status, 0);
    fprintf(stderr, "SHELL EXIT SIGNAL=%d STATUS=%d\r\n", WTERMSIG(status), WEXITSTATUS(status));
    close(fd0[1]);
    close(fd1[1]);
    close(fd0[0]);
    close(fd1[0]);
    shutdown(sockfd, 2);
    shutdown(newsockfd, 2);
    exit(0);
}

void handler(int num) {
    if (num == SIGPIPE) {
        fprintf(stderr, "SIGPIPE received\n");
        exit(0);
    }
    if (num == SIGINT) {
        if (kill(pid, SIGINT) < 0) {
            fprintf(stderr, "Failed to kill process: Error:%d, Message: %s\n", errno, strerror(errno));
            exit(1);
        }
    }
}

void cleanup_ends() {
    deflateEnd(&defstream);
    inflateEnd(&infstream);
}

void setup_zlib() {

    defstream.zalloc = Z_NULL;
    defstream.zfree = Z_NULL;
    defstream.opaque = Z_NULL;
    if (deflateInit(&defstream, Z_DEFAULT_COMPRESSION) != Z_OK) {
        fprintf(stderr, "Failed to initialize compression.\n");
        exit(1);
    }

    infstream.zalloc = Z_NULL;
    infstream.zfree = Z_NULL;
    infstream.opaque = Z_NULL;
    if (inflateInit(&infstream) != Z_OK) {
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

int main(int argc, char *argv[]) {

    int portno, clilen;
    struct sockaddr_in serv_addr, cli_addr;
    ssize_t bytes;

    struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"compress", no_argument, NULL, 'c'},
        {0, 0, 0, 0}};

    int pflag = 0;
    int opt;

    while ((opt = getopt_long(argc, argv, "p:c", options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            portno = atoi(optarg);
            pflag = 1;
            break;
        case 'c':
            compressOpt = 1;
            break;
        default: {
            fprintf(stderr, "Incorrect argument: correct usage is ./server --port=portno [--compress] \n");
            exit(1);
        }
        }
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    if (!pflag) {
        fprintf(stderr, "port not specified\n");
        exit(1);
    }

    bzero((char *)&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr *)&serv_addr,
             sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(sockfd, 5);

    clilen = sizeof(cli_addr);

    newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, (socklen_t *)&clilen);
    if (newsockfd < 0)
        error("ERROR on accept");

    pipe(fd0);
    pipe(fd1);

    pid = fork();

    if (pid < 0) {
        fprintf(stderr, "fork Failed\n");
        shutdown(sockfd, 2);
        shutdown(newsockfd, 2);
        exit(1);
    }
    else if (pid == 0) {

        close(fd0[1]);
        close(fd1[0]);
        close(STDIN_FILENO);
        dup(fd0[0]);
        close(fd0[0]);
        close(STDOUT_FILENO);
        dup(fd1[1]);
        close(STDERR_FILENO);
        dup(fd1[1]);
        close(fd1[1]);

        execl("/bin/bash", "sh", (char *)NULL);
        fprintf(stderr, "ERROR in executing shell\n");
        exit(1);
    }
    else {

        signal(SIGINT, handler);
        signal(SIGPIPE, handler);

        close(fd0[0]);
        close(fd1[1]);

        struct pollfd fds[2];
        fds[0].fd = newsockfd;
        fds[0].events = POLLIN | POLLHUP | POLLERR;
        
        fds[1].fd = fd1[0];
        fds[1].events = POLLIN | POLLHUP | POLLERR;        

        char in[BUF_SIZE];
        char out[BUF_SIZE];
        char carriage[2] = {'\r', '\n'};

        if (compressOpt == 1) {

            setup_zlib();
        }

        for (;;) {

            poll(fds, 2, 0);

            if (fds[0].revents & POLLIN) {

                bzero(in, BUF_SIZE);
                bytes = read(newsockfd, in, 1023);
                if (bytes < 0)
                    error("ERROR reading from socket");

                if (compressOpt == 1) {

                    bzero(out, BUF_SIZE);
                    memcpy(out, in, bytes);
                    bytes = decompression(bytes, BUF_SIZE, in, out);

                }

                for (int i = 0; i < bytes; i++) {
                    char curr = in[i];
                    switch (curr) {
                    case '\r':
                    case '\n':
                        write(fd0[1], &carriage[1], sizeof(char));
                        break;
                    case 0x04:
                        close(fd0[1]);
                        break;
                    case 0x03:
                        if (kill(pid, SIGINT) < 0) {
                            fprintf(stderr, "Failed to kill process: Error:%d, Message: %s\n", errno, strerror(errno));
                            exit(1);
                        }
                        break;
                    default:
                        write(fd0[1], &curr, sizeof(char));
                        break;
                    }
                }
            }

            if (fds[0].revents & (POLLHUP | POLLERR)) {
                exit(0);
            }

            if (fds[1].revents & POLLIN) {

                bzero(in, BUF_SIZE);
                bytes = read(fd1[0], in, 1023);
                if (bytes < 0)
                    error("ERROR reading from pipe");

                if (compressOpt == 1) {

                    // bzero(out, BUF_SIZE);
                    memcpy(out, in, bytes);
                    bytes = compression(bytes, BUF_SIZE, in, out);
                }

                write(newsockfd, in, bytes);
            }

            if (fds[1].revents & (POLLHUP | POLLERR)) {
                exit(0);
            }
        }
    }
    exit(0);
}