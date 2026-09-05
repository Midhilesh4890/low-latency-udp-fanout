#!/usr/bin/env python3
import fcntl
import os
import re
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import sqlite3
import struct
import http.client
import json

ROOT = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get("PULSEFANOUT_BIN", ROOT / "build/bin")).resolve()

def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pulse_test_")
        self.root = Path(self.temp.name)
        self.processes = []
        self.logs = []
        self.rings = []

    def tearDown(self):
        for p in self.processes:
            if p.poll() is None:
                p.terminate()
        for p in self.processes:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill(); p.wait()
        for log in self.logs:
            log.close()
        for ring in self.rings:
            Path("/dev/shm/" + ring[1:]).unlink(missing_ok=True)
        self.temp.cleanup()

    def start(self, *args):
        log = open(self.root / f"log{len(self.logs)}", "w+")
        self.logs.append(log)
        p = subprocess.Popen(list(map(str, args)), stdout=log, stderr=log)
        p.log = log
        self.processes.append(p)
        return p

    def text(self, p):
        p.log.flush()
        return Path(p.log.name).read_text()

    def wait_text(self, p, needle):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if needle in self.text(p):
                return
            if p.poll() is not None:
                self.fail(self.text(p))
            time.sleep(.01)
        self.fail("readiness timeout: " + self.text(p))

    def ring(self):
        name = "/pulse_test_" + uuid.uuid4().hex
        self.rings.append(name)
        return name

    def wait_ring(self, name, p):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                with open("/dev/shm/" + name[1:], "rb") as f:
                    fcntl.flock(f, fcntl.LOCK_SH | fcntl.LOCK_NB)
                    if os.fstat(f.fileno()).st_size:
                        return
            except (FileNotFoundError, BlockingIOError):
                pass
            if p.poll() is not None:
                self.fail(self.text(p))
            time.sleep(.01)
        self.fail("ring timeout")

    def success(self, p):
        self.assertEqual(p.wait(timeout=15), 0, "\n".join(self.text(child) for child in self.processes))

    def certificates(self):
        def openssl(*args):
            subprocess.run(["openssl", *args], cwd=self.root, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                "-subj", "/CN=test-ca", "-keyout", "ca.key", "-out", "ca.pem")
        for identity in ("sender", "receiver"):
            openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={identity}",
                    "-keyout", identity + ".key", "-out", identity + ".csr")
            (self.root / "ext").write_text(
                f"subjectAltName=DNS:{identity}\nextendedKeyUsage=serverAuth,clientAuth\n")
            openssl("x509", "-req", "-in", identity + ".csr", "-CA", "ca.pem",
                    "-CAkey", "ca.key", "-CAcreateserial", "-days", "1",
                    "-extfile", "ext", "-out", identity + ".pem")

    def relay(self, mode, endpoint, tls_port, peer=None):
        identity = "sender" if mode == "send" else "receiver"
        return self.start(sys.executable, ROOT / "scripts/tls_relay.py", mode,
                          "--host", "127.0.0.1", "--port", tls_port,
                          "--unix-path", endpoint, "--ca", self.root / "ca.pem",
                          "--cert", self.root / (identity + ".pem"),
                          "--key", self.root / (identity + ".key"),
                          "--peer-name", peer or ("receiver" if mode == "send" else "sender"),
                          "--timeout", "5")

    def test_parallel_fanout(self):
        receivers = []
        destinations = []
        for _ in range(2):
            name, udp_port = self.ring(), port()
            p = self.start(BIN / "receiver", "--allow-insecure-udp", "--out-shm", name, "--slots", 8192,
                           "--bind", "127.0.0.1", "--port", udp_port, "--count", 1000)
            self.wait_ring(name, p)
            receivers.append(p)
            destinations.extend(["--dst", f"127.0.0.1:{udp_port}"])
        input_name = self.ring()
        producer = self.start(BIN / "producer", "--shm", input_name, "--slots", 8192,
                              "--count", 1000, "--rate", 10000, "--wait-readers", 1)
        self.wait_ring(input_name, producer)
        sender = self.start(BIN / "sender", "--allow-insecure-udp", "--in-shm", input_name, "--slots", 8192,
                            "--count", 1000, "--send-workers", 2, *destinations)
        for p in [producer, sender, *receivers]:
            self.success(p)
        for p in receivers:
            self.assertIn("published=1000", self.text(p))
            self.assertIn("rejected=0", self.text(p))

    def test_exclusive_creator_and_attach_before_create(self):
        name = self.ring()
        consumer = self.start(BIN / "consumer", "--shm", name, "--slots", 8192,
                              "--count", 100)
        time.sleep(.05)
        producer = self.start(BIN / "producer", "--shm", name, "--slots", 8192,
                              "--count", 100, "--rate", 1000, "--start-delay-ms", 300,
                              "--wait-readers", 1)
        self.wait_ring(name, producer)
        duplicate = self.start(BIN / "producer", "--shm", name, "--slots", 8192, "--count", 1)
        self.assertNotEqual(duplicate.wait(timeout=3), 0)
        self.assertIn("File exists", self.text(duplicate))
        self.success(producer); self.success(consumer)
        self.assertRegex(self.text(consumer), r"received\s*:\s*100\b")

    def test_tls_pipeline(self):
        self.certificates()
        output, input_name = self.ring(), self.ring()
        rx_path, tx_path = self.root / "rx.sock", self.root / "tx.sock"
        receiver = self.start(BIN / "receiver", "--allow-insecure-udp", "--out-shm", output, "--slots", 8192,
                              "--unix-listen", rx_path, "--count", 1000, "--idle-ms", 10000)
        self.wait_ring(output, receiver)
        consumer = self.start(BIN / "consumer", "--shm", output, "--slots", 8192,
                              "--count", 1000, "--idle-ms", 10000)
        tls_port = port()
        remote = self.relay("receive", rx_path, tls_port)
        self.wait_text(remote, "LISTENING")
        local = self.relay("send", tx_path, tls_port)
        self.wait_text(local, "READY")
        producer = self.start(BIN / "producer", "--shm", input_name, "--slots", 8192,
                              "--count", 1000, "--rate", 10000, "--wait-readers", 1)
        self.wait_ring(input_name, producer)
        sender = self.start(BIN / "sender", "--allow-insecure-udp", "--in-shm", input_name, "--slots", 8192,
                            "--unix-dst", tx_path, "--count", 1000)
        for p in [producer, sender, local, remote, receiver, consumer]:
            self.success(p)
        self.assertIn("published=1000", self.text(receiver))
        self.assertRegex(self.text(consumer), r"received\s*:\s*1000\b")
        self.assertRegex(self.text(consumer), r"dropped\s*:\s*0\b")

    def test_tls_rejects_wrong_client_identity(self):
        self.certificates()
        tls_port = port()
        remote = self.relay("receive", self.root / "unused", tls_port, peer="unauthorized")
        self.wait_text(remote, "LISTENING")
        ctx = ssl.create_default_context(cafile=str(self.root / "ca.pem"))
        ctx.load_cert_chain(self.root / "sender.pem", self.root / "sender.key")
        with socket.create_connection(("127.0.0.1", tls_port), timeout=3) as tcp:
            with ctx.wrap_socket(tcp, server_hostname="receiver") as stream:
                try:
                    self.assertEqual(stream.recv(1), b"")
                except ssl.SSLError:
                    pass
        self.assertNotEqual(remote.wait(timeout=3), 0)
        self.assertIn("unexpected client certificate identity", self.text(remote))


    def test_tls_rejects_wrong_server_identity(self):
        self.certificates()
        tls_port = port()
        remote = self.relay("receive", self.root / "unused", tls_port)
        self.wait_text(remote, "LISTENING")
        local = self.relay("send", self.root / "unused2", tls_port, peer="wrong-server")
        self.assertNotEqual(local.wait(timeout=5), 0)
        self.assertIn("CERTIFICATE_VERIFY_FAILED", self.text(local))

    def test_tls_rejects_missing_client_certificate(self):
        self.certificates()
        tls_port = port()
        remote = self.relay("receive", self.root / "unused", tls_port)
        self.wait_text(remote, "LISTENING")
        ctx = ssl.create_default_context(cafile=str(self.root / "ca.pem"))
        with socket.create_connection(("127.0.0.1", tls_port), timeout=3) as tcp:
            try:
                with ctx.wrap_socket(tcp, server_hostname="receiver") as stream:
                    stream.recv(1)
            except ssl.SSLError:
                pass
        self.assertNotEqual(remote.wait(timeout=3), 0)
        self.assertIn("certificate", self.text(remote).lower())

    def test_supervised_pipeline(self):
        p = self.start(sys.executable, ROOT / "scripts/run_pipeline.py",
                       "--bin-dir", BIN, "--count", 1000, "--port", port(),
                       "--fec-k", 8, "--fec-parity", 3,
                       "--state-file", self.root / "state.json")
        self.success(p)
        self.assertRegex(self.text(p), r"received\s*:\s*1000\b")
        self.assertFalse((self.root / "state.json").exists())


    def test_supervisor_cleans_up_failed_run(self):
        p = self.start(sys.executable, ROOT / "scripts/run_pipeline.py",
                       "--bin-dir", BIN, "--count", 100000, "--rate", 100,
                       "--port", port(), "--timeout", .1,
                       "--state-file", self.root / "state.json")
        self.assertNotEqual(p.wait(timeout=8), 0)
        self.assertIn("deadline exceeded", self.text(p))
        names = set(re.findall(r"/pulse_[0-9a-f]+_(?:in|out)", self.text(p)))
        self.assertEqual(len(names), 2)
        for name in names:
            self.assertFalse(Path("/dev/shm/" + name[1:]).exists())
        self.assertFalse((self.root / "state.json").exists())


    def test_udp_requires_explicit_opt_in(self):
        for app in ("sender","receiver"):
            p=self.start(BIN/app)
            self.assertNotEqual(p.wait(timeout=3),0)
            self.assertIn("allow-insecure-udp",self.text(p))

    def test_two_publishers_with_overlapping_sequences(self):
        output=self.ring();udp_port=port()
        receiver=self.start(BIN/"receiver","--allow-insecure-udp","--out-shm",output,
                            "--slots",8192,"--bind","127.0.0.1","--port",udp_port,"--count",400)
        self.wait_ring(output,receiver)
        publishers=[]
        for stream_id in (11,22):
            name=self.ring()
            producer=self.start(BIN/"producer","--shm",name,"--slots",8192,
                                "--count",200,"--rate",10000,"--wait-readers",1)
            self.wait_ring(name,producer)
            sender=self.start(BIN/"sender","--allow-insecure-udp","--in-shm",name,
                              "--slots",8192,"--count",200,"--dst",f"127.0.0.1:{udp_port}",
                              "--stream-id",stream_id,"--fec-k",8,"--fec-parity",3)
            publishers.extend((producer,sender))
        for p in publishers+[receiver]:
            self.success(p)
        self.assertIn("published=400",self.text(receiver))
        self.assertIn("rejected=0",self.text(receiver))

    def test_slow_destination_does_not_stop_healthy_destination(self):
        output=self.ring();udp_port=port()
        receiver=self.start(BIN/"receiver","--allow-insecure-udp","--out-shm",output,
                            "--slots",8192,"--bind","127.0.0.1","--port",udp_port,"--count",2000,
                            "--idle-ms",10000)
        self.wait_ring(output,receiver)
        slow_path=self.root/"slow.sock"
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as listener:
            listener.bind(str(slow_path));listener.listen()
            name=self.ring()
            producer=self.start(BIN/"producer","--shm",name,"--slots",8192,
                                "--count",2000,"--rate",10000,"--wait-readers",1)
            self.wait_ring(name,producer)
            sender=self.start(BIN/"sender","--allow-insecure-udp","--in-shm",name,
                              "--slots",8192,"--count",2000,"--queue-bytes",65536,
                              "--dst",f"127.0.0.1:{udp_port}","--unix-dst",slow_path)
            connection,_=listener.accept()
            with connection:  # Intentionally do not drain this destination.
                self.success(producer);self.success(receiver)
                self.assertNotEqual(sender.wait(timeout=10),0)
            self.assertIn("published=2000",self.text(receiver))
            self.assertIn("queue full",self.text(sender))

    def test_durable_store_persists_deduplicates_and_bounds(self):
        sys.path.insert(0,str(ROOT/"scripts"))
        from durable_relay import Store
        path=self.root/"store.db"
        store=Store(path,64,"test")
        seq=store.enqueue(b"abc");session=store.session
        store.close()
        store=Store(path,64,"test")
        self.assertEqual(store.session,session)
        self.assertEqual(store.first()[0],seq)
        store.retire(seq)
        self.assertTrue(store.accept("0"*32,1,b"abc"))
        self.assertFalse(store.accept("0"*32,1,b"abc"))
        store.close()
        store=Store(path,64,"test")
        self.assertFalse(store.accept("0"*32,1,b"abc"))
        with self.assertRaises(ValueError):
            store.accept("0"*32,3,b"gap")
        with self.assertRaises(BufferError):
            store.enqueue(b"x"*64)
        self.assertEqual(store.bytes(),3)
        store.close()

    def test_durable_replay_after_sender_relay_crash(self):
        self.certificates()
        output,name=self.ring(),self.ring()
        rx_path,tx_path=self.root/"rx.sock",self.root/"tx.sock"
        tls_port=port()
        receiver=self.start(BIN/"receiver","--out-shm",output,"--slots",8192,
                            "--unix-listen",rx_path,"--ack-publish","--stream-reconnect",
                            "--count",200,"--idle-ms",15000)
        self.wait_ring(output,receiver)
        consumer=self.start(BIN/"consumer","--shm",output,"--slots",8192,
                            "--count",200,"--idle-ms",15000)
        common=["--host","127.0.0.1","--port",tls_port,"--ca",self.root/"ca.pem",
                "--timeout",2,"--retry-seconds",15]
        local=self.start(sys.executable,ROOT/"scripts/tls_relay.py","send",*common,
                         "--unix-path",tx_path,"--cert",self.root/"sender.pem",
                         "--key",self.root/"sender.key","--peer-name","receiver",
                         "--durable-db",self.root/"outbox.db")
        self.wait_text(local,"READY")
        producer=self.start(BIN/"producer","--shm",name,"--slots",8192,
                            "--count",200,"--rate",10000,"--wait-readers",1)
        self.wait_ring(name,producer)
        sender=self.start(BIN/"sender","--in-shm",name,"--slots",8192,
                          "--count",200,"--unix-dst",tx_path,"--durable-acks")
        self.success(producer);self.success(sender)
        # No remote relay exists yet. All locally acknowledged frames must be on disk.
        local.kill();local.wait(timeout=3)
        with sqlite3.connect(self.root/"outbox.db") as db:
            self.assertEqual(db.execute("SELECT count(*) FROM pending").fetchone()[0],200)
        remote=self.start(sys.executable,ROOT/"scripts/tls_relay.py","receive",*common,
                          "--unix-path",rx_path,"--cert",self.root/"receiver.pem",
                          "--key",self.root/"receiver.key","--peer-name","sender",
                          "--durable-db",self.root/"inbox.db")
        self.wait_text(remote,"LISTENING")
        replay=self.start(sys.executable,ROOT/"scripts/tls_relay.py","send",*common,
                          "--unix-path",tx_path,"--cert",self.root/"sender.pem",
                          "--key",self.root/"sender.key","--peer-name","receiver",
                          "--durable-db",self.root/"outbox.db","--replay-only")
        self.success(replay);self.success(receiver);self.success(consumer)
        deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            with sqlite3.connect(self.root/"inbox.db") as db:
                pending=db.execute("SELECT count(*) FROM pending").fetchone()[0]
            if pending==0:
                break
            time.sleep(.01)
        self.assertEqual(pending,0)
        with sqlite3.connect(self.root/"outbox.db") as db:
            self.assertEqual(db.execute("SELECT count(*) FROM pending").fetchone()[0],0)
        self.assertRegex(self.text(consumer),r"received\s*:\s*200\b")
        remote.terminate();remote.wait(timeout=5)


    def test_configured_service_supervisor(self):
        config=self.root/"services.json"
        config.write_text(json.dumps({"services":[
            {"name":"service","argv":[sys.executable,"-u","-c","import time; print('READY'); time.sleep(10)"],"ready":"READY"},
            {"name":"completion","argv":[sys.executable,"-c","import time; time.sleep(.1)"]}
        ],"completion":"completion","endpoints":{"test":"localhost:9443"}}))
        state=self.root/"services-state.json"
        p=self.start(sys.executable,ROOT/"scripts/supervise_services.py",config,
                     "--state-file",state,"--restarts",0)
        self.success(p)
        self.assertIn("service: READY",self.text(p))
        self.assertFalse(state.exists())

    def test_authenticated_discovery_and_stale_registration(self):
        self.certificates()
        state=self.root/"discovery.json"
        value={"epoch":"test","services":[],"endpoints":{"feed":"receiver:9443"}}
        state.write_text(json.dumps(value))
        tls_port=port()
        server=self.start(sys.executable,ROOT/"scripts/discovery.py","serve",
                          "--host","127.0.0.1","--port",tls_port,
                          "--ca",self.root/"ca.pem","--cert",self.root/"receiver.pem",
                          "--key",self.root/"receiver.key","--client-name","sender","--state-file",state)
        self.wait_text(server,"LISTENING")
        ctx=ssl.create_default_context(cafile=str(self.root/"ca.pem"))
        ctx.load_cert_chain(self.root/"sender.pem",self.root/"sender.key")
        def request():
            with socket.create_connection(("127.0.0.1",tls_port),timeout=3) as tcp:
                with ctx.wrap_socket(tcp,server_hostname="receiver") as stream:
                    stream.sendall(b"GET /v1/services HTTP/1.1\r\nHost: receiver\r\nConnection: close\r\n\r\n")
                    response=http.client.HTTPResponse(stream);response.begin()
                    return response.status,response.read()
        status,body=request()
        self.assertEqual(status,200)
        self.assertEqual(json.loads(body),value)
        value["services"]=[{"pid":os.getpid(),"start_ticks":"wrong"}]
        state.write_text(json.dumps(value))
        self.assertEqual(request()[0],503)

if __name__ == "__main__":
    unittest.main()
