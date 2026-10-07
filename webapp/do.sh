clang -std=c23 \
  -Wall -Wextra -Wconversion -Wsign-compare -Werror \
  -fsanitize=address,undefined \
  -I/opt/homebrew/opt/jemalloc/include \
  -L/opt/homebrew/opt/jemalloc/lib \
  -o r_accounting server.c -lpthread -ljemalloc -g && \
./r_accounting $1 $2
