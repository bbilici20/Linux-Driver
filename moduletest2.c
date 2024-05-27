#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

#define NUM_WRITERS 2  // Number of writer processes
#define ITS 10

void read_all(int fd, void *buf, int count) {
    while (count > 0) {
        int ret;
        ret = read(fd, buf, count);
        if (ret == -1) {
            perror("read");
            exit(1);
        }

        count -= ret;
        buf += ret;
    }
}

void write_all(int fd, void *buf, int count) {
    while (count > 0) {
        int ret;
        ret = write(fd, buf, count);
        if (ret == -1) {
            perror("write");
            exit(1);
        }

        count -= ret;
        buf += ret;
    }
}

int main(int argc, char *argv[])
{
    pid_t pid;
    int fd;
    int sum = 0, i;
    int val;
    int cnt;

    for (int writer = 0; writer < NUM_WRITERS; writer++) {
        pid = fork();

        if (pid == -1) {
            perror("fork");
            exit(1);
        }

        if (pid == 0) {
            fd = open("/dev/dm510-0", O_RDWR);
            perror("w open");   
            for (i = 0; i < ITS; i++) {
                val++;
                sum += val;
                cnt = 4;
                write_all(fd, &val, 4);
            }
            printf("Writer %d: expected result: %d\n", writer, sum);
            exit(0);
        }
    }

    fd = open("/dev/dm510-1", O_RDWR);
    perror("r open");

    for (i = 0; i < NUM_WRITERS * ITS; i++) {
        read_all(fd, &val, 4);
        sum += val;
    }
    printf("Reader: result: %d\n", sum);

    for (int writer = 0; writer < NUM_WRITERS; writer++) {
        wait(NULL);
    }

    return 0;
}
