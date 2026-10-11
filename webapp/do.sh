clang \
  -Wall -Wextra -Wconversion -Wsign-compare -Werror \
  -o r_accounting server.c \
  -lpthread -g && \
./r_accounting $1 $2
