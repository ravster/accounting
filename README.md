# Local

```
docker build -t a1 .
docker run --name accounting1 -e POSTGRES_PASSWORD=password -v ${PWD}:/app -d a1
docker exec -it b1 sh
psql -U postgres
go version
go run .
```
