
all: clean client server

client: client.c rdma_common.c
	gcc -o $@ -Wall $^ -libverbs

server: server.c rdma_common.c
	gcc -o $@ -Wall $^ -lrdmacm -libverbs

clean:
	rm -f server client

cscope:
	cscope -bqR
