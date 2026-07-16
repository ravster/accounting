clang -Wall -Wextra -std=c23 \
  -I/opt/homebrew/opt/libpq/include \
  -L/opt/homebrew/opt/libpq/lib \
  -I/opt/homebrew/opt/jemalloc/include \
  -L/opt/homebrew/opt/jemalloc/lib \
  -o server server.c -lpthread -lpq -ljemalloc -g && \
  PGDATABASE=accounting PGUSER=ravidesai PGPASSWORD=password ./server
