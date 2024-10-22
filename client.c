#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_MSG_LEN 1024
#define IP_ADDR_LEN 16

int sock_fd;
pid_t child_pid;

void sigint_handler(int signo) {
  if (child_pid > 0) {
    kill(child_pid, SIGTERM);
  }
  close(sock_fd);
  exit(0);
}

void receive_messages() {
  char buffer[MAX_MSG_LEN];
  ssize_t n;
  while (1) {
    memset(buffer, 0, MAX_MSG_LEN);
    n = read(sock_fd, buffer, MAX_MSG_LEN - 1);
    if (n <= 0) {
      if (n < 0)
        perror("read error");
      break;
    }
    buffer[n] = '\0';
    printf("%s", buffer);
    fflush(stdout);
  }
  printf("Server disconnected.\n");
  kill(getppid(), SIGTERM);
  exit(0);
}

void send_message(const char *message) {
  ssize_t n = write(sock_fd, message, strlen(message));
  if (n <= 0) {
    perror("write error");
    exit(1);
  }
}

int main() {
  struct sockaddr_in server_addr;
  char buffer[MAX_MSG_LEN];
  char server_ip[IP_ADDR_LEN];
  int server_port;

  // Get server IP address from user
  printf("Enter server IP address: ");
  if (fgets(server_ip, IP_ADDR_LEN, stdin) == NULL) {
    perror("Error reading IP address");
    exit(1);
  }
  server_ip[strcspn(server_ip, "\n")] = 0; // Remove newline if present

  // Get server port from user
  printf("Enter server port: ");
  if (scanf("%d", &server_port) != 1) {
    perror("Error reading port number");
    exit(1);
  }
  while (getchar() != '\n')
    ; // Clear input buffer

  // Create socket
  if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket creation failed");
    exit(1);
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(server_port);

  // Convert IP address from string to binary form
  if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
    perror("Invalid address/ Address not supported");
    exit(1);
  }

  // Connect to the server
  if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("Connection failed");
    exit(1);
  }

  printf("Connected to server. Waiting for login prompt...\n");

  // Set up signal handler
  signal(SIGINT, sigint_handler);

  // Fork to create child process for receiving messages
  child_pid = fork();
  if (child_pid < 0) {
    perror("Failed to fork");
    exit(1);
  } else if (child_pid == 0) {
    // Child process: handle receiving messages
    receive_messages();
  } else {
    // Parent process: handle sending messages
    while (1) {
      memset(buffer, 0, MAX_MSG_LEN);
      ssize_t bytes_read = read(STDIN_FILENO, buffer, MAX_MSG_LEN - 1);
      if (bytes_read <= 0) {
        if (bytes_read < 0)
          perror("read error from stdin");
        break;
      }
      // Remove newline character if present
      if (buffer[bytes_read - 1] == '\n') {
        buffer[bytes_read - 1] = '\0';
        bytes_read--;
      }
      // Check for empty message
      if (bytes_read == 0)
        continue;
      // Check for quit command
      if (strcmp(buffer, "q") == 0) {
        write(sock_fd, buffer, strlen(buffer));
        break;
      }
      // Send the message
      send_message(buffer);
    }
    // Clean up
    kill(child_pid, SIGTERM);
    waitpid(child_pid, NULL, 0);
    close(sock_fd);
  }
  return 0;
}