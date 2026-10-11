clang -Wall -Wextra \
  -o r_accounting server.c \
  -lpthread -g && \
lldb ./r_accounting -- $1 $2
