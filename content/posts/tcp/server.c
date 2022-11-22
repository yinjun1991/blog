#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[])
{
  /* 1. 创建 socket
   * AF_INET 表示用于创建 IPv4 网络的 socket
   * SOCK_STREAM 表示用于 TCP 网络（还可以使用 SOCK_DGRAM 创建 UDP socket）
   * socket 函数文档：https://man7.org/linux/man-pages/man2/socket.2.html
   */
  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == -1)
  {
    printf("failed to create socket\n");
    return 1;
  }

  // 2. 将 socket 绑定网络接口（IP + 端口）
  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY; // 相当于 0.0.0.0
  address.sin_port = htons(8888);
  // https://man7.org/linux/man-pages/man2/bind.2.html
  int code = bind(socket_fd, &address, sizeof(address));
  if (code < 0)
  {
    printf("failed to bind net interface\n");
    return 1;
  }

  // 3. 开始监听连接
  // 第二个参数称为 backlog，用于指定等待连接的队列长度。
  // listen 函数文档，https://man7.org/linux/man-pages/man2/listen.2.html
  code = listen(socket_fd, 1024);
  if (code < 0)
  {
    printf("failed to listen at 0.0.0.0:8888\n");
    return 1;
  }

  printf("listen at 0.0.0.0:8888\n");

  // 4. pending 队列中取出连接，无限循环
  // https://man7.org/linux/man-pages/man2/accept.2.html
  int conn;
  while ((conn = accept(socket_fd, NULL, NULL)))
  {
    char *message = "Hello\n";
    write(conn, message, strlen(message));
    close(conn);
  }

  return 0;
}
