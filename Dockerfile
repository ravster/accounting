FROM postgres:18.4-alpine3.24

RUN apk update && apk add postgresql-dev go

WORKDIR /app

CMD ["postgres"]
