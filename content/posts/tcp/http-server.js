const net = require("net");

const delimiter = "\r\n";

const server = net.createServer((conn) => {
  conn.setEncoding("utf-8");
  let data = "";

  let method = "";
  let url = "";
  let protocol = "";
  const headers = {};
  let body = "";
  let methodParsed = false;
  let inBody = false;

  function sendResponse() {
    console.log(`method: ${method}, url: ${url}, protocol: ${protocol}`);
    console.log(`headers: ${JSON.stringify(headers, undefined, 2)}`);
    console.log(`body: ${body}`);

    const resBody = `<html>
    <title>Hello HTTP</title>
<body>
  <h1>Hello HTTP</h1>
</body>
</html>`;

    const res = [
      `${protocol} 200 OK`,
      `Content-Type: text/html`,
      `Content-Length: ${resBody.length}`,
      delimiter,
      resBody,
    ];

    conn.write(res.join(delimiter));
    conn.end();
  }

  function isRequestEnd() {
    return body.length === (headers["Content-Length"] || 0);
  }

  conn.on("data", (chunk) => {
    data += chunk;
    if (inBody) {
      if (isRequestEnd()) {
        sendResponse();
      }
      return;
    }

    for (let i = 0; i < data.length; i++) {
      if (data.substr(i, 2) === delimiter) {
        if (data.substr(i + 2, 2) === delimiter) {
          inBody = true;
          data = data.slice(i + 4);
          if (isRequestEnd()) {
            sendResponse();
          }
          break;
        }
        const line = data.slice(0, i);
        if (methodParsed) {
          const [key, value] = line.split(":");
          headers[key] = value;
          console.log(`header: ${key} ${value}`);
        } else {
          [method, url, protocol] = line.split(" ");
          methodParsed = true;
          console.log(`method: ${method}, url: ${url}, protocol: ${protocol}`);
        }
        data = data.slice(i + 2);
      }
    }
  });
});

server.listen(8888);
