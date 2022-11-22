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
  char msg_hd[4] = {0, 0, 0, 0};
  int conn = accept(socket_fd, NULL, NULL);
  while (1)
  {
    // 先读取消息 code 和长度
    for (int i = 0; i < 4; i++)
    {
      msg_hd[i] = 0;
    }
    int count = read(conn, &msg_hd, 4);
    if (count < 4)
    {
      printf("connection end: short head");
      close(conn);
      break;
    }
    printf("receive message from client:\n");
    char code = msg_hd[0];
    int len = msg_hd[1] + (msg_hd[2] << 8) + (msg_hd[3] << 16);
    printf("code: %d, message length: %d\n", code, len);
    // 读取消息体
    if (len > 0)
    {
      char buffer[len + 1];
      int count = read(conn, &buffer, len);
      if (count < len)
      {
        printf("connection end: short body");
        close(conn);
        break;
      }
      buffer[count] = 0;
      printf("message [%s]\n", buffer);
    }
  }

  close(socket_fd);
  return 0;
}
