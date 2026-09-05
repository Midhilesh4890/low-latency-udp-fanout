"""Bounded SQLite spooling and at-least-once relay delivery.

Remote ACK means committed to the receiving relay's inbox, not consumed by the
application. Local publish ACK permits inbox retirement; an ambiguous local
ACK can replay a frame. Application side effects require idempotent consumers.
"""
import fcntl
import os
from pathlib import Path
import socket
import sqlite3
import ssl
import struct
import threading
import time
import uuid

MAX_FRAME = 1100

class Store:
    def __init__(self, path, limit=64*1024*1024, identity=""):
        self.path = Path(path)
        self.lockfile = open(str(path) + ".lock", "a+b")
        fcntl.flock(self.lockfile, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.mutex = threading.RLock()
        self.limit = limit
        fd=os.open(path,os.O_CREAT|os.O_RDWR,0o600);os.close(fd)
        self.db = sqlite3.connect(path, check_same_thread=False)
        os.chmod(path, 0o600)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("PRAGMA journal_size_limit=4194304")
        self.db.execute("PRAGMA wal_autocheckpoint=128")
        self.db.execute(f"PRAGMA max_page_count={limit//4096 + 1024}")
        self.db.executescript("""
          CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS pending(id INTEGER PRIMARY KEY AUTOINCREMENT,
              session TEXT NOT NULL, remote_id INTEGER NOT NULL, payload BLOB NOT NULL);
          CREATE TABLE IF NOT EXISTS received(session TEXT PRIMARY KEY, high INTEGER NOT NULL);
          CREATE TABLE IF NOT EXISTS usage(total INTEGER NOT NULL);
          INSERT INTO usage SELECT coalesce(sum(length(payload)),0) FROM pending
            HAVING NOT EXISTS(SELECT 1 FROM usage);
          CREATE TRIGGER IF NOT EXISTS pending_added AFTER INSERT ON pending BEGIN
            UPDATE usage SET total=total+length(new.payload);
          END;
          CREATE TRIGGER IF NOT EXISTS pending_removed AFTER DELETE ON pending BEGIN
            UPDATE usage SET total=total-length(old.payload);
          END;
        """)
        with self.db:
            self.db.execute("INSERT OR IGNORE INTO meta VALUES ('session',?)", (uuid.uuid4().hex,))
            self.db.execute("INSERT OR IGNORE INTO meta VALUES ('identity',?)", (identity,))
        if self.db.execute("SELECT value FROM meta WHERE key='identity'").fetchone()[0] != identity:
            self.close()
            raise ValueError("spool identity/configuration differs from its original owner")
        self.session = self.db.execute("SELECT value FROM meta WHERE key='session'").fetchone()[0]

    def bytes(self):
        with self.mutex:
            return self.db.execute("SELECT total FROM usage").fetchone()[0]

    def enqueue(self, payload):
        if not 1 <= len(payload) <= MAX_FRAME:
            raise ValueError("invalid durable payload")
        with self.mutex, self.db:
            if self.bytes() + len(payload) > self.limit:
                raise BufferError("durable spool is full")
            cursor = self.db.execute("INSERT INTO pending(session,remote_id,payload) VALUES (?,0,?)",
                                     (self.session, payload))
            return cursor.lastrowid

    def accept(self, session, sequence, payload):
        if not 1 <= sequence < 2**63 or not 1 <= len(payload) <= MAX_FRAME:
            raise ValueError("invalid durable frame")
        with self.mutex, self.db:
            row = self.db.execute("SELECT high FROM received WHERE session=?", (session,)).fetchone()
            high = row[0] if row else 0
            if sequence <= high:
                return False
            if sequence != high + 1:
                raise ValueError("durable sequence gap")
            if not row and self.db.execute("SELECT count(*) FROM received").fetchone()[0] >= 64:
                raise BufferError("durable session limit reached")
            if self.bytes() + len(payload) > self.limit:
                raise BufferError("durable inbox is full")
            self.db.execute("INSERT INTO pending(session,remote_id,payload) VALUES (?,?,?)",
                            (session, sequence, payload))
            self.db.execute("INSERT INTO received VALUES (?,?) ON CONFLICT(session) DO UPDATE SET high=excluded.high",
                            (session, sequence))
            return True

    def first(self):
        with self.mutex:
            return self.db.execute("SELECT id,session,remote_id,payload FROM pending ORDER BY id LIMIT 1").fetchone()

    def retire(self, row_id):
        with self.mutex, self.db:
            self.db.execute("DELETE FROM pending WHERE id=?", (row_id,))

    def close(self):
        if self.db:
            self.db.close()
            self.db = None
        self.lockfile.close()

def exact(stream, size):
    out = bytearray()
    while len(out) < size:
        chunk = stream.recv(size-len(out))
        if not chunk:
            if out:
                raise ConnectionError("truncated durable frame")
            return None
        out.extend(chunk)
    return bytes(out)

def local_frame(stream):
    prefix = exact(stream, 2)
    if prefix is None:
        return None
    size = struct.unpack("!H", prefix)[0]
    if not 1 <= size <= MAX_FRAME:
        raise ValueError("invalid local frame")
    body = exact(stream, size)
    if body is None:
        raise ConnectionError("missing local frame body")
    return body

def retryable(error):
    return isinstance(error, (OSError, ConnectionError)) and not isinstance(error, ssl.SSLCertVerificationError)

def send(args, context):
    # Persist the intended peer identity to prevent replay to a different peer.
    store = Store(args.durable_db, args.spool_bytes, "send:" + args.peer_name)
    stop, ingress_done = threading.Event(), threading.Event()
    failures = []
    def transmit():
        stream = None
        deadline = time.monotonic() + args.retry_seconds
        try:
            while not stop.is_set():
                row = store.first()
                if row is None:
                    if ingress_done.is_set():
                        break
                    deadline = time.monotonic() + args.retry_seconds
                    time.sleep(.01)
                    continue
                try:
                    if stream is None:
                        tcp = socket.create_connection((args.host,args.port),args.timeout)
                        try:
                            stream = context.wrap_socket(tcp,server_hostname=args.peer_name)
                        except BaseException:
                            tcp.close()
                            raise
                    row_id, session, _, payload = row
                    stream.sendall(b"PFD1" + bytes.fromhex(session) +
                                   struct.pack("!QH",row_id,len(payload)) + payload)
                    ack = exact(stream, 8)
                    if ack is None or struct.unpack("!Q",ack)[0] != row_id:
                        raise ConnectionError("missing durable acknowledgement")
                    store.retire(row_id)
                    deadline = time.monotonic() + args.retry_seconds
                except Exception as error:
                    if stream is not None:
                        stream.close(); stream=None
                    if not retryable(error) or time.monotonic() >= deadline:
                        raise
                    time.sleep(.05)
            if stream is not None:
                stream.unwrap().close()
                stream=None
        except Exception as error:
            failures.append(error)
            stop.set()
        finally:
            if stream is not None:
                stream.close()
    worker = threading.Thread(target=transmit)
    worker.start()
    bound = False
    try:
        if not args.replay_only:
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as listener:
                listener.bind(args.unix_path);bound=True
                os.chmod(args.unix_path,0o600)
                listener.listen(1);listener.settimeout(.2)
                print("READY",flush=True)
                connection = None
                deadline = time.monotonic()+args.timeout
                while not stop.is_set() and time.monotonic()<deadline:
                    try:
                        connection,_=listener.accept();break
                    except socket.timeout:
                        pass
                if connection is None:
                    raise TimeoutError("durable local accept timed out")
                with connection:
                    connection.settimeout(args.timeout)
                    while not stop.is_set():
                        payload=local_frame(connection)
                        if payload is None:
                            break
                        deadline=time.monotonic()+args.retry_seconds
                        while True:
                            try:
                                store.enqueue(payload);break
                            except BufferError:
                                if stop.is_set() or time.monotonic()>=deadline:
                                    raise
                                time.sleep(.01)
                        # The C++ sender opts into this ACK with --durable-acks.
                        connection.sendall(b"\x01")
        ingress_done.set()
        worker.join(args.retry_seconds+args.timeout+1)
        if worker.is_alive():
            raise TimeoutError("durable drain deadline exceeded")
        if failures:
            raise failures[0]
    finally:
        stop.set()
        worker.join(args.timeout+1)
        if bound:
            Path(args.unix_path).unlink(missing_ok=True)
        # Socket deadlines ensure the worker exits before closing its DB.
        if worker.is_alive():
            worker.join()
        store.close()

def receive(args, context):
    store = Store(args.durable_db,args.spool_bytes,"receive:"+args.peer_name)
    stop = threading.Event()
    failures=[]
    def deliver():
        local=None
        deadline=time.monotonic()+args.retry_seconds
        try:
            while not stop.is_set():
                row=store.first()
                if row is None:
                    deadline=time.monotonic()+args.retry_seconds
                    time.sleep(.01);continue
                try:
                    if local is None:
                        local=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
                        local.settimeout(args.timeout);local.connect(args.unix_path)
                    row_id,_,_,payload=row
                    local.sendall(struct.pack("!H",len(payload))+payload)
                    if exact(local,1)!=b"\x01":
                        raise ConnectionError("receiver did not acknowledge publication")
                    store.retire(row_id)
                    deadline=time.monotonic()+args.retry_seconds
                except Exception as error:
                    if local is not None:
                        local.close();local=None
                    if not retryable(error) or time.monotonic()>=deadline:
                        raise
                    time.sleep(.05)
        except Exception as error:
            failures.append(error);stop.set()
        finally:
            if local is not None:
                local.close()
    worker=threading.Thread(target=deliver)
    worker.start()
    try:
        with socket.create_server((args.host,args.port)) as listener:
            listener.settimeout(.2)
            print("LISTENING",flush=True)
            while not stop.is_set():
                try:
                    tcp,_=listener.accept()
                except socket.timeout:
                    continue
                with tcp:
                    tcp.settimeout(args.timeout)
                    try:
                        with context.wrap_socket(tcp,server_side=True) as stream:
                            names=[value.lower() for kind,value in stream.getpeercert().get("subjectAltName",()) if kind=="DNS"]
                            if args.peer_name.lower() not in names:
                                raise ssl.SSLError("unexpected client certificate identity")
                            print("READY",flush=True)
                            while not stop.is_set():
                                header=exact(stream,30)
                                if header is None:
                                    stream.unwrap().close()
                                    break
                                if header[:4]!=b"PFD1":
                                    raise ValueError("durable protocol mismatch")
                                sequence,size=struct.unpack("!QH",header[20:])
                                if not 1<=size<=MAX_FRAME:
                                    raise ValueError("invalid durable frame length")
                                payload=exact(stream,size)
                                if payload is None:
                                    raise ConnectionError("missing durable payload")
                                store.accept(header[4:20].hex(),sequence,payload)
                                stream.sendall(struct.pack("!Q",sequence))
                    except (OSError,ValueError,BufferError) as error:
                        # Failed peer connections do not remove committed inbox
                        # entries or terminate other future authenticated sessions.
                        print(f"durable connection rejected: {error}",flush=True)
        if failures:
            raise failures[0]
    finally:
        stop.set();worker.join(args.timeout+1)
        if worker.is_alive():
            worker.join()
        store.close()

def run(args, context_factory):
    if not args.unix_path:
        raise ValueError("durable mode requires Unix stream endpoints")
    if args.replay_only and args.mode!="send":
        raise ValueError("replay-only is a sender option")
    ctx=context_factory(args,args.mode=="receive")
    (send if args.mode=="send" else receive)(args,ctx)
