# Low-latency UDP fan-out transport

The [submission write-up](SUBMISSION.md) describes the transport design, measurement method, results, limitations, and reproduction procedure.

Supporting material:

- [Executed analysis notebook](analysis.ipynb)
- [Compact measurement evidence](results/)
- [Transport source](harness/src/)
- [Benchmark runners](benchmark/)
- [EC2 provisioning and host preparation](infra/)

Build and test:

```bash
make -C harness clean all test
bash benchmark/test_preflight_isolation.sh
