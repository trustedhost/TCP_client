#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEBUG

#define TCP_PORT 5100 /* 서버의 포트 번호 */
#define MAX_CLIENTS 5 /* 최대 클라이언트 수 */
#define MAX_GROUPS 2  /* 최대 그룹 수 */

typedef struct {
  int socket;
  char name[50];
  int group;
  pid_t pid;
} Client;

typedef struct {
  char name[50];
  int member_count;
} Group;

static volatile sig_atomic_t keep_running = 1;
static Client clients[MAX_CLIENTS];
static Group groups[MAX_GROUPS];
static int pipe_fds_PtoC[MAX_CLIENTS][2]; // Parent to Child
static int pipe_fds_CtoP[MAX_CLIENTS][2]; // Child to Parent

static int g_noc = 0; /* 연결된 총 클라이언트 수 */

ssize_t read_nonblock(int fd, char *buffer, size_t size) {
  ssize_t bytes_read = read(fd, buffer, size);
  if (bytes_read < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0; // No data available
    } else {
      return -1; // Error
    }
  }
  return bytes_read;
}

ssize_t write_nonblock(int fd, const char *buffer, size_t size) {
  ssize_t bytes_written = write(fd, buffer, size);
  if (bytes_written < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0; // Would block
    } else {
      return -1; // Error
    }
  }
  return bytes_written;
}

void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl F_GETFL");
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("fcntl F_SETFL O_NONBLOCK");
  }
}

void sigfunc_sigint(int no) { /* SIGINT : Ctrl + C */
                              //   printf("Signal : %d\n", no);
  keep_running = 0;
}

void sigfunc_sigchld(int no) { /* SIGCHLD : 자식 소켓이 끊어졌을 때. */
                               //   printf("Signal : %d(%d)\n", no, g_noc);
  pid_t pid;
  int status;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i].pid == pid) {
        // printf("Client %d is disconnected\n", i);
        close(clients[i].socket);
        close(pipe_fds_PtoC[i][0]);
        close(pipe_fds_PtoC[i][1]);
        close(pipe_fds_CtoP[i][0]);
        close(pipe_fds_CtoP[i][1]);
        clients[i].socket = 0;
        clients[i].pid = 0;
        g_noc--;
        break;
      }
    }
  }
  if (g_noc == 0)
    exit(0);
}

void sigfunc_sigusr1(
    int no) { /* SIGUSR1 : 자식이 데이터를 pipe 에 데이터를 보냈을 때. */
              //   printf("Signal : %d\n", no);
  // Check for messages from child processes
  for (int j = 0; j < MAX_CLIENTS; j++) {
    if (clients[j].socket != 0) {
      set_nonblocking(pipe_fds_CtoP[j][0]);
      char pipe_msg[BUFSIZ + 64];
      ssize_t n =
          read_nonblock(pipe_fds_CtoP[j][0], pipe_msg, sizeof(pipe_msg) - 1);
      if (n > 0) {
        pipe_msg[n] = '\0';
        int sender_index = j;
        char message[BUFSIZ] = "";
        char index_string[10];
        sprintf(index_string, "(%d) ", sender_index);
        // make j to string
        strcat(message, index_string);
        strcat(message, clients[sender_index].name);
        strcat(message, ": ");
        strcat(message, pipe_msg);
        strcat(message, "\n");

        // printf("Parent received from client %d: %s", sender_index, message);
        // Handle the message (e.g., send to a group)
        // This is where you'd implement your group messaging logic
        for (int k = 0; k < MAX_CLIENTS; k++) {
          if (k != sender_index && clients[k].socket != 0 &&
              clients[k].group == clients[sender_index].group) {
            // printf("sending to client %d\n", k);
            // printf("message : %s\n", message);
            if (write(pipe_fds_PtoC[k][1], message, strlen(message)) <= 0) {
              perror("write()");
            }
            // printf("message sent to client %d\n", k);
          }
        }
      }
    }
  }
}

/* 서버 소켓 생성 함수 */
int create_server_socket(int port) {
  int ssock;
  struct sockaddr_in servaddr;

  /* 서버 소켓 생성 */
  if ((ssock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket()");
    return -1;
  }

  /* 주소 구조체에 주소 지정 */
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(port); /* 사용할 포트 지정 */

  /* bind 함수를 사용하여 서버 소켓의 주소 설정 */
  if (bind(ssock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
    perror("bind()");
    close(ssock);
    return -1;
  }

  /* 동시에 접속하는 클라이언트의 처리를 위한 대기 큐를 설정 */
  if (listen(ssock, 8) < 0) {
    perror("listen()");
    close(ssock);
    return -1;
  }

  return ssock;
}

void login_process(int index) {
  char welcome_msg[] = "Welcome! Please enter your name: ";
  char name_buffer[50];
  ssize_t bytes_read;

  // Send welcome message
  if (write(clients[index].socket, welcome_msg, strlen(welcome_msg)) <= 0) {
    perror("write() welcome message");
    exit(1);
  }

  // Read the client's name
  bytes_read =
      read(clients[index].socket, name_buffer, sizeof(name_buffer) - 1);
  if (bytes_read <= 0) {
    perror("read() client name");
    exit(1);
  }

  // Null-terminate the name and remove newline character if present
  name_buffer[bytes_read] = '\0';
  char *newline = strchr(name_buffer, '\n');
  if (newline)
    *newline = '\0';

  // Copy the name to the client struct
  strncpy(clients[index].name, name_buffer, sizeof(clients[index].name) - 1);
  clients[index].name[sizeof(clients[index].name) - 1] = '\0';

  // Send confirmation message
  char confirm_msg[100];
  snprintf(confirm_msg, sizeof(confirm_msg),
           "Hello, %s! You're now logged in.\n", clients[index].name);
  if (write(clients[index].socket, confirm_msg, strlen(confirm_msg)) <= 0) {
    perror("write() confirmation message");
    exit(1);
  }

  //   printf("Client %d logged in as: %s\n", index, clients[index].name);
}

void handle_client(int index) {
  set_nonblocking(clients[index].socket);
  // set_nonblocking(pipe_fds_CtoP[index][1]);
  set_nonblocking(pipe_fds_PtoC[index][0]);
  char mesg[BUFSIZ];
  int n;
  close(pipe_fds_CtoP[index][0]); // Close read end
  close(pipe_fds_PtoC[index][1]); // Close write end
  while (1) {
    memset(mesg, 0, BUFSIZ);
    n = read_nonblock(clients[index].socket, mesg, BUFSIZ);
    if (n > 0) {
      /* pipe parent 에게 전달 */
      if (write(pipe_fds_CtoP[index][1], mesg, n) <= 0) {
        perror("write()");
        break;
      } else {
        kill(getppid(), SIGUSR1); // Send signal to parent
      }
    }
    n = read_nonblock(pipe_fds_PtoC[index][0], mesg, BUFSIZ);
    if (n > 0) {
      // printf("Received data from parent : %s", mesg);
      if (write(clients[index].socket, mesg, n) <= 0) {
        perror("write()");
        break;
      }
    }

    /* 클라이언트로 buf에 있는 문자열 전송 */
    // if (write(clients[index].socket, mesg, n) <= 0) {
    //   perror("write()");
    //   break;
    // }

    if (strncmp(mesg, "q", 1) == 0)
      break;
  }

  close(clients[index].socket); /* 클라이언트 소켓을 닫음 */
  exit(0);                      /* 자식 프로세스 종료 */
}

int main(int argc, char **argv) {
  int ssock, portno, n;                 /* 소켓 디스크립트 정의 */
  struct sockaddr_in servaddr, cliaddr; /* 주소 구조체 정의 */
  socklen_t clen = sizeof(cliaddr);
  pid_t pid; /* fork( ) 함수를 위한 PID */
  char mesg[BUFSIZ];
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    exit(1);
  }
  portno = (argc == 2) ? atoi(argv[1]) : TCP_PORT;

  signal(SIGCHLD, sigfunc_sigchld);
  signal(SIGINT, sigfunc_sigint); // Server side socket binding
  signal(SIGUSR1, sigfunc_sigusr1);

  ssock = create_server_socket(portno);
  //   printf("Server listening on port %d\n", portno);

  // Initialize clients
  for (int i = 0; i < MAX_CLIENTS; i++) {
    clients[i].socket = 0;
    clients[i].pid = 0;
  }
  // Initialize groups
  for (int i = 0; i < MAX_GROUPS; i++) {
    snprintf(groups[i].name, 50, "Group %d", i);
    groups[i].member_count = 0;
  }

  while (keep_running) {
    /* 클라이언트가 접속하면 접속을 허용하고 클라이언트 소켓 생성 */
    int csock = accept(ssock, (struct sockaddr *)&cliaddr, &clen);
    if (csock < 0) {
      //   perror("accept()");
      continue;
    }
    if (g_noc >= MAX_CLIENTS) {
      //   printf("Connection rejected: max clients reached\n");
      close(csock);
      continue;
    }
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i].socket == 0) {
        clients[i].socket = csock;
        clients[i].group = -1; // Not in any group initially
        break;
      }
    }

    /* 네트워크 주소를 문자열로 변경 */
    inet_ntop(AF_INET, &cliaddr.sin_addr, mesg, BUFSIZ);
    // printf("Client is connected : %s\n", mesg);
    // printf("Client socket : %d on %dth index \n", csock, i);
    login_process(i);
    g_noc++;

    if (pipe(pipe_fds_PtoC[i]) == -1) {
      perror("pipe_fds_PtoC");
      exit(1);
    }
    if (pipe(pipe_fds_CtoP[i]) == -1) {
      perror("pipe_fds_CtoP");
      exit(1);
    }

    /* 연결되는 클라이언트와의 통신을 위한 자식 프로세스 생성 */
    if ((pid = fork()) < 0) {
      perror("fork()");
      close(csock);

    } else if (pid == 0) { /* 자식 프로세스 */
      close(ssock); /* 자식 프로세스에서 서버 소켓을 닫음 */
      for (int j = 0; j < MAX_CLIENTS; j++) {
        if (j != i) {
          close(pipe_fds_CtoP[j][0]);
          close(pipe_fds_CtoP[j][1]);
          close(pipe_fds_PtoC[j][0]);
          close(pipe_fds_PtoC[j][1]);
        }
      }
      handle_client(i);
    } else { /* 부모 프로세스 */
      clients[i].pid = pid;
      //   printf("Client %d's pid : %d \n", i, pid);
      close(csock); /* 부모 프로세스에서는 클라이언트 소켓을 닫음 */
      // waitpid(pid, NULL, 0);  /* 자식 프로세스의 종료를 기다림 */
    }
  }
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].socket != 0) {
      close(clients[i].socket);
      close(pipe_fds_PtoC[i][0]);
      close(pipe_fds_PtoC[i][1]);
      close(pipe_fds_CtoP[i][0]);
      close(pipe_fds_CtoP[i][1]);
    }
  }

  close(ssock); /* 서버 소켓을 닫음 */
  return 0;
}