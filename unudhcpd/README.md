## unudhcpd

> Vendored into j4-debian-attempt from the archived (read-only)
> https://gitlab.com/postmarketOS/unudhcpd at commit `128773d`, rather than
> a git submodule, because it carries a local patch (dropped the
> `getprotobyname("udp")` NSS lookup in `server.c`, which segfaults when
> statically linked - see `build_unudhcpd.sh`) that can't be pushed
> upstream.

This is an extremely basic DHCP server that only issues 1 IP address to any client that sends a DHCP{OFFER,REQUEST}.

It is meant to be used on an interface that will only ever see 1 client at a
time, but where the client's MAC may change, making it inconvenient to try and
configure leasing, etc on other DHCP servers. With unudhcpd, the client always
gets an IP quickly.


## Usage
```
Usage:
        unudhcpd -i <interface> [-s <server IP>] [-p <server port] [-c <client IP>]
Where:
        -i  network interface to bind to
        -s  server IP {default: 172.16.1.1}
        -p  server port {default: 67}
        -c  client IP to issue for DHCP requests {default: 172.16.1.2}
        -v  print version and quit
```

## Installation

Project uses meson, so:

```
$ meson _build
...
$ meson compile -C _build
...
$ sudo meson install -C _build
...
```
