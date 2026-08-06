clang -Wall -Wextra -std=c23 \
  -I/opt/homebrew/opt/jemalloc/include \
  -L/opt/homebrew/opt/jemalloc/lib \
  -o server server.c -lpthread -ljemalloc -g && \
./server $1
