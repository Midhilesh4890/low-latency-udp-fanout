# Low-latency UDP fan-out transport

The main write-up is in [SUBMISSION.md](SUBMISSION.md). It covers the transport design, test setup, results, limitations, and reproduction steps.

Repository contents:

- [Executed analysis notebook](analysis.ipynb)
- [Measurement data and logs](results/)
- [Transport source and run instructions](harness/)

Build and test:

    make -C harness clean all test
