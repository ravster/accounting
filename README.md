# Local

```
docker build -t a1 .
docker run --name accounting1 -e POSTGRES_PASSWORD=password -v ${PWD}:/app -d a1
docker start accounting1
docker exec -it accounting1 sh
psql -U postgres

gcc -Wall -Wextra server.c -o run -lpthread -lpq

# Backup
pg_dump -Fc -U postgres -d accounting > cafe_accounting.dump
```

On mac, because I'm tired of docker taking minutes to recognize that the source-file has changed and update that inside the container. Urgh, so annoying. I didn't spend all this time programming in C just for something silly to slow me down.
```
brew install libpq postgresql jemalloc

LC_ALL="en_US.UTF-8" /opt/homebrew/opt/postgresql@18/bin/postgres -D /opt/homebrew/var/postgresql@18

clang -Wall -Wextra \
  -I/opt/homebrew/opt/libpq/include \
  -L/opt/homebrew/opt/libpq/lib \
  -I/opt/homebrew/opt/jemalloc/include \
  -L/opt/homebrew/opt/jemalloc/lib \
  -o run server.c -lpthread -lpq -ljemalloc && \
  PGDATABASE=accounting PGUSER=postgres PGPASSWORD=password ./run
```

pg on mac caveats
==> Caveats
==> postgresql@18
This formula has created a default database cluster with:
  initdb --locale=en_US.UTF-8 -E UTF-8 /opt/homebrew/var/postgresql@18

When uninstalling, some dead symlinks are left behind so you may want to run:
  brew cleanup --prune-prefix

To start postgresql@18 now and restart at login:
  brew services start postgresql@18
Or, if you don't want/need a background service you can just run:
  LC_ALL="en_US.UTF-8" /opt/homebrew/opt/postgresql@18/bin/postgres -D /opt/homebrew/var/postgresql@18
