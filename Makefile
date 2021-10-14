default: server client

server: server.c
	gcc -Wall -Wextra server.c -lz -o server
client: client.c
	gcc -Wall -Wextra client.c -lz -o client
clean:
	rm -f client server
