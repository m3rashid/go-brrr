# syntax=docker/dockerfile:1
# brrr — smallest possible image for a C + SQLite webserver.
#
# Builds every implementation under implementations/ (compile check) and
# ships the LATEST one as the entrypoint. When promoting a snapshot to the
# latest implementation, update Makefile's FINAL and the COPY line below.

FROM alpine:3.20 AS build
RUN apk add --no-cache build-base sqlite-dev
WORKDIR /src
COPY Makefile ./
COPY implementations ./implementations
RUN make

FROM alpine:3.20
# latest implementation: implementations/event-loop-lazy-buffers/brrr.c
RUN apk add --no-cache sqlite-libs \
    && adduser -D -H -s /sbin/nologin brrr \
    && mkdir /data && chown brrr:brrr /data
COPY --from=build /src/brrr /usr/local/bin/brrr
USER brrr
WORKDIR /data
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/brrr"]
# args: port, workers (one event loop per core)
CMD ["8080", "2"]
