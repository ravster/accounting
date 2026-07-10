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
