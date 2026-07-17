# Accounting program

## History

I've been doing my personal accounting for years using spreadsheets (Google Sheets). It's nice, but I want to play around with something different now. This started out as an Odin program, then a Golang program because I was having too many issues getting Odin running inside Docker on a mac.

Then I read an article explaining all things going on in the golang runtime (GC, scheduler, etc) and it grossed me out the way nodejs grosses me out. I use RubyOnRails professionally, and the amount of wastage in those codebases also grosses me out nowadays. So I switched to building this in C.

I've been very pleasantly surprised by how easy it has been to build a basic webapp in C. TCP is provided right out the gate by standard libraries, as is a thread-pool. The only dependency I have is libpq to talk to the DB. String-handling in C isn't as bad as people make it out to be, at least at the scale of this program.

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

On mac, because I'm tired of docker taking minutes to recognize that the source-file has changed and update that inside the container. Urgh, so annoying. I didn't spend all this time programming in C just for something silly in Docker to slow me down.
```
brew install libpq postgresql jemalloc

LC_ALL="en_US.UTF-8" /opt/homebrew/opt/postgresql@18/bin/postgres -D /opt/homebrew/var/postgresql@18

clang -Wall -Wextra -g \
  -I/opt/homebrew/opt/libpq/include \
  -L/opt/homebrew/opt/libpq/lib \
  -I/opt/homebrew/opt/jemalloc/include \
  -L/opt/homebrew/opt/jemalloc/lib \
  -o server server.c -lpthread -lpq -ljemalloc && \
  PGDATABASE=accounting PGUSER=ravidesai PGPASSWORD=password ./server
```

pg on mac caveats
```
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
```
