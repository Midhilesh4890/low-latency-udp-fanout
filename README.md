# Low-latency UDP fan-out transport

The [submission write-up](SUBMISSION.md) describes the transport design, measurement method, results, limitations, and reproduction procedure.

Submission material:

- [Executed analysis notebook](analysis.ipynb)
- [Compact measurement evidence](results/)
- [Transport source and run instructions](harness/)

Build and test:

    make -C harness clean all test
