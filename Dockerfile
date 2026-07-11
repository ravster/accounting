FROM postgres:18.4-alpine3.24

RUN apk update && apk add postgresql-dev gcc libc-dev jemalloc-dev

WORKDIR /app

CMD ["postgres"]
