clang \
  -Wall -Wextra -Wconversion -Wsign-compare -Werror \
  -I/opt/homebrew/opt/jemalloc/include \
  -L/opt/homebrew/opt/jemalloc/lib \
  -o r_accounting server.c -lpthread -ljemalloc -g && \
./r_accounting $1 $2
