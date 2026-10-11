clang -Wall -Wextra \
  -I/opt/homebrew/opt/jemalloc/include \
  -L/opt/homebrew/opt/jemalloc/lib \
  -o r_accounting server.c -lpthread -ljemalloc -g && \
lldb ./r_accounting -- $1 $2
