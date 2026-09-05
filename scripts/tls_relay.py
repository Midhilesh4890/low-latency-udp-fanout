#!/usr/bin/env python3
"""Mutually authenticated TLS 1.3 transport between loopback UDP endpoints.

TCP supplies congestion control and retransmission on the remote link. Local
UDP ingress/egress remains best effort. Failure closes the relay; it never
silently reconnects or replays messages with ambiguous delivery.
"""
import argparse
import ipaddress
import socket
import ssl
import struct
import os
import signal
from contextlib import contextmanager

MAX_FRAME = 1100

def loopback(value):
    if not ipaddress.ip_address(value).is_loopback:
        raise argparse.ArgumentTypeError("UDP endpoints must be loopback IP addresses")
    return value

def context(args, server):
    ctx = ssl.create_default_context(
        ssl.Purpose.CLIENT_AUTH if server else ssl.Purpose.SERVER_AUTH,
        cafile=args.ca)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_cert_chain(args.cert, args.key)
    return ctx

def exact(stream, length):
    out = bytearray()
    while len(out) < length:
        chunk = stream.recv(length - len(out))
        if not chunk:
            if not out:
                return None
            raise ConnectionError("truncated TLS frame")
        out.extend(chunk)
    return bytes(out)

def receive_frames(stream, udp, destination):
    while True:
        prefix = exact(stream, 2)
        if prefix is None:
            return
        size = struct.unpack("!H", prefix)[0]
        if not 1 <= size <= MAX_FRAME:
            raise ValueError("invalid TLS frame length")
        frame = exact(stream, size)
        if frame is None:
            raise ConnectionError("missing TLS frame body")
        if udp.sendto(frame, destination) != size:
            raise OSError("short local UDP send")

@contextmanager
def local_stream(path, listener=False):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        if listener:
            sock.bind(path)  # Never remove a pre-existing socket.
            os.chmod(path, 0o600)
            try:
                sock.listen(1)
                sock.settimeout(30)
                print("READY", flush=True)
                connection, _ = sock.accept()
                with connection:
                    yield connection
            finally:
                os.unlink(path)
        else:
            sock.connect(path)
            yield sock

def copy_frames(source, destination):
    while True:
        prefix = exact(source, 2)
        if prefix is None:
            return
        size = struct.unpack("!H", prefix)[0]
        if not 1 <= size <= MAX_FRAME:
            raise ValueError("invalid stream frame length")
        body = exact(source, size)
        if body is None:
            raise ConnectionError("missing stream frame body")
        destination.sendall(prefix + body)

def run(args):
    if args.durable_db:
        import durable_relay
        return durable_relay.run(args,context)
    if args.mode == "send":
        ctx = context(args, False)
        with socket.create_connection((args.host, args.port), args.timeout) as tcp:
            with ctx.wrap_socket(tcp, server_hostname=args.peer_name) as stream:
                if args.unix_path:
                    with local_stream(args.unix_path, True) as local:
                        local.settimeout(args.timeout)
                        copy_frames(local, stream)
                    stream.unwrap().close()
                    return
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                    udp.bind((args.udp_host, args.udp_port))
                    udp.settimeout(args.timeout)
                    print("READY", flush=True)
                    while True:
                        frame = udp.recv(MAX_FRAME + 1)
                        if not 1 <= len(frame) <= MAX_FRAME:
                            raise ValueError("invalid local datagram length")
                        stream.sendall(struct.pack("!H", len(frame)) + frame)
    else:
        ctx = context(args, True)
        with socket.create_server((args.host, args.port), reuse_port=False) as listener:
            listener.settimeout(args.timeout)
            print("LISTENING", flush=True)
            tcp, _ = listener.accept()
            with tcp:
                tcp.settimeout(args.timeout)
                with ctx.wrap_socket(tcp, server_side=True) as stream:
                    # Pin an exact DNS SAN identity in addition to CA validation.
                    names = [name.lower() for kind, name in
                             stream.getpeercert().get("subjectAltName", ())
                             if kind == "DNS"]
                    if args.peer_name.lower() not in names:
                        raise ssl.SSLError("unexpected client certificate identity")
                    print("READY", flush=True)
                    if args.unix_path:
                        with local_stream(args.unix_path) as local:
                            local.settimeout(args.timeout)
                            copy_frames(stream, local)
                        stream.unwrap().close()
                        return
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                        receive_frames(stream, udp, (args.udp_host, args.udp_port))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode", choices=("send", "receive"))
    p.add_argument("--host", required=True, help="remote TLS host or local TLS bind address")
    p.add_argument("--port", type=int, default=9443)
    p.add_argument("--udp-host", type=loopback, default="127.0.0.1")
    endpoint = p.add_mutually_exclusive_group(required=True)
    endpoint.add_argument("--udp-port", type=int)
    endpoint.add_argument("--unix-path", help="reliable local stream endpoint (recommended)")
    p.add_argument("--ca", required=True)
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--peer-name", required=True)
    p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--durable-db", help="SQLite outbox/inbox; enables acknowledged durable mode on both peers")
    p.add_argument("--spool-bytes",type=int,default=64*1024*1024)
    p.add_argument("--retry-seconds",type=float,default=30)
    p.add_argument("--replay-only",action="store_true",help="drain an existing durable outbox without local ingress")
    args = p.parse_args()
    if args.spool_bytes<1024*1024 or args.spool_bytes>1024*1024*1024 or args.retry_seconds<=0:
        p.error("spool-bytes must be 1 MiB..1 GiB and retry-seconds positive")
    if args.replay_only and not args.durable_db:
        p.error("replay-only requires durable-db")
    if args.timeout <= 0:
        p.error("timeout must be positive")
    def stop(signum,frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM,stop)
    try:
        run(args)
    except KeyboardInterrupt:
        p.exit(130,"relay stopped\n")
    except (OSError, ValueError, BufferError) as error:
        p.exit(1, f"TLS relay failed: {error}\n")

if __name__ == "__main__":
    main()
