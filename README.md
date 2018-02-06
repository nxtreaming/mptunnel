# mptunnel
MultiPath Tunnel (Simpified user space MPUDP)

[中文说明](README.zh_CN.md)

## ABOUT

MultiPath Tunnel is a multipath UDP implementation in user space. Like MultiPath TCP, you can establish
several connections from local host to remote server.

MPTCP(MultiPath TCP) is a good idea to make network connection robust, but it only works on TCP. I was
searching for MPUDP implementation but got nothing, so I write this tool.


## CONCEPTION

```
                        .---- bridge server 1 ----.
                       /                            \
 Server A --- mpclient ------ bridge server 2 ------- mpserver --- Server B
                       \                            /
                        `---- bridge server 3 ----`
```

There are two servers named Server A and Server B. The network connection between Server A and Server B is
unstable (with high packet loss ratio). Thus, we like to establish a multipath tunnel between Server A
and Server B, hoping the connection between Server A and Server B becomes more stabilty (decrease
packet loss ratio).

_mpclient_ is the client part of mptunnel, it could be run on Server A. You must tell mpclient the
configuration of bridge servers. Once mpclient is started, it opens a local UDP port for listening, forwards
any packet to/from bridge servers.

_mpserver_ is the server part of mptunnel, it could be run on Server B. You must tell mpserver the
configuration of Server B. Once mpserver is started, it will forward any packet to/from Server B.

Bridge servers are simple, it will only forward packets between mpclient and mpserver. You can use _nc_ or _socat_ to deploy a bridge server.


## EXAMPLE

I want to connect to my OpenVPN server, but the connection is unstable, packet loss ratio is high. The
TCP throughput over the OpenVPN tunnel is very small due to high packet loss ratio. To increase TCP
throughput (decrease packet loss ratio), I can run a MPUDP to OpenVPN server and establish OpenVPN connection
on it.

OpenVPN listens on UDP port 1194, I run mpserver on OpenVPN server like this:

```
mpserver 2000 127.0.0.1 1194
```

On my local host, run mpclient:

```
mpclient mpclient.conf
```

Below is the content of mpclient.conf 

```
1.2.3.4 4000
bridge1.myhost.com 4000
bridge2.myhost.com 4000
```

1.2.3.4 is the IP of OpenVPN server. It's okay to use it as a bridge server.

On each bridge server, I use _socat_ to forward packets:

```
socat udp-listen:4000 udp4:1.2.3.4:2000
```

Bridge server will listen on UDP port 4000, it will forward any recieved packet to 1.2.3.4:2000, and vice versa.


Now I make OpenVPN client to connect localhost:3000 which mpclient is listening on, then OpenVPN will
establish an OpenVPN connection over MultiPath UDP tunnel.


## KNOWN ISSUES

* mptunnel adds some control information into packets, including synchronous information. mpserver and mpclient must be started at the same time. If mpclient or mpserver terminated, you have to restart both mpserver and mpclient to re-establish the tunnel.

* Currently you can only specify a single target host. Does any one know if there is any C library of SOCKS5 proxy? I think making mpclient as a SOCKS proxy server will make it more easy to use.

* mptunnel encrypts packets defaultly, but it will decrease the throughput. I do some tests on my PC with Athlon II P320 Processor, the actual throughput is 3Mbps while using three tunnels, after I disable encryption the throughput increase to 300Mbps. If you dont't like mptunnel to encrypt packets, set environment variable MPTUNNEL_ENCRYPT=0

## DEPENDENCIES

To compile mptunnel, these libraries are required:

* libev

## SEE ALSO

[mlvpn](https://github.com/zehome/MLVPN/), A similar solution for multipath UDP.
