const net = require("net");

const client = net.connect(8888, "localhost", (conn) => {
  console.log("connected to server.");
});

const hello = Buffer.concat([
  Buffer.from(new Uint8Array([1, 5, 0, 0])),
  Buffer.from("hello", "ascii"),
]);

const world = Buffer.concat([
  Buffer.from(new Uint8Array([2, 5, 0, 0])),
  Buffer.from("world", "ascii"),
]);

client.write(Buffer.concat([hello, world]));
