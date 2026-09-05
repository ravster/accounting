# Accounting program

## Context

I've been doing my personal accounting for years using spreadsheets (Google Sheets). It's nice, but I want to play around with something different now. This started out as an Odin program, then a Golang program because I was having too many issues getting Odin running inside Docker on a mac.

Then I read an article explaining all things going on in the golang runtime (GC, scheduler, etc) and it grossed me out the way nodejs grosses me out. I use RubyOnRails professionally, and the amount of wastage in those codebases also grosses me out nowadays. So I switched to building this in C.

I've been very pleasantly surprised by how easy it has been to build a basic webapp in C. It took ~1kloc; ridiculously easy. TCP is provided right out the gate by standard libraries, and a thread-pool is just a queue with many consumers. The only dependency I have is libpq to talk to the DB. String-handling in C isn't as bad as people make it out to be, at least at the scale of this program.

# Local

On mac, because I'm tired of docker taking minutes to recognize that the source-file has changed and update that inside the container. Urgh, so annoying. I didn't spend all this time programming in C just for something silly in Docker to slow me down.
```
brew install jemalloc
```

## Check RAM usage

```
ps -o pid,rss,vsz,comm | grep r_accounting
for i in {1..20}; do curl 'http://localhost:3002/2'; done
```

# History
- 20260805: Remove Golang code from the project. Nothing in here needs a GC and a scheduler process constantly churning away.
    Switched to building the whole webapp in C. This is a lot easier than I thought it would be; HTTP is just TCP sockets with string-parsing. TCP sockets are provided out of the box by the C stdlib, and string-manipulation in C is nowhere close to as difficult as people make it out to be, at least in a program of this scale. Plenty of C programs manipulate strings, and I can just have my own custom strings struct made in a few hundred LOC.
- 20260806: Purge PQ out of the codebase. We don't need a database for this program. It's a single-tenant program that has 2 tables. This can be a simple dir of flat-files. KISS
    Removing libpq from the program immediately dropped the RAM usage from 20-30MB to 2-4MB. It seems the PQResult struct takes up a lot of space. Don't need that. Deleted. Booyah.
- 20260905: Ruby version of the code is now close enough to parity that I consider it equivalent. The response time of the ruby server (460loc) is the same as the C server (1250loc), completely out of my expectations. This is pretty cool. I've gotten so used to Rails being slow at workplaces that I didn't realize that Ruby is so fast.
