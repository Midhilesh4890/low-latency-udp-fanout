# Low-latency data transfer

🇷🇺 [Русская версия](README.md)

Hi! Welcome to the **Low-latency data transfer** challenge from the HFT fund Spectral::Technologies. In this challenge you'll be solving one of the problems we've faced in our own work — building a system that transfers data from one source server to other servers with minimal latency.

The key requirement: optimal latency not just on average, but at high quantiles and under load. The system has to hold latency down when the data flow is heavy and when there are many receivers.


**Prizes:**
- 1st place – 6,000 USD gross
- 2nd place – 3,600 USD gross
- 3rd place – 2,400 USD gross

We'll be glad to offer the authors of the best submissions (and not only the top 3) a shortened hiring track for C++ Software Engineer or AI Software Engineer positions. We've also prepared some merch prizes.

## Task details

The baseline configuration is **one source → one receiver**, but in the general case one source fans a stream out to **many receivers**.
That can be one of the qualitative differentiators in your solution.

### What we provide

So that we can interpret the numbers properly, **we provide the data source and the data sink** — those are the fixed ends of the system.
Your job is to build the middle: the transport that delivers the stream from the source to the receivers.

Included:

1. **Producer (binary)** — generates a stream of events in a **defined format** and **stamps every event with a timestamp** at the moment it is created. This is the input to your system.
2. **Consumer (binary)** — accepts delivered events, **measures latency** from the timestamps, and reports the measurements/metrics. This is the output of your system and the point of measurement.
3. **Starter ipython notebook** — reads the measurements and draws a couple of basic plots. Use it as a starting point for your own analysis.

You need to implement 2 binaries that carry out the data transfer (sender and receiver, basically), interacting with the producer and consumer through a shared-memory transport.

### The channel between sender and receiver

On the channel between the sending and receiving points, you should assume a small amount of packet loss — 0.01–1%. How latency changes as a function of packet loss is a potential direction for qualitative improvement in your solution.

### What you can change in the harness

You may improve and modify the transport itself, as well as the specific data format, within reason.

It's important to keep the delivered data realistic. In the current format, some duplicated or irrelevant fields can be optimized away. Thinking those changes through and justifying them in your final write-up is part of the task too. That said, a good solution doesn't necessarily focus on this part of the problem.

One observation you may make is that the current format holds up rather poorly under packet loss.

The framing part of the header that the consumer relies on for measurement must stay unchanged (`seq_id`, `send_ts_ns`).

### Data / payload

Qualitatively, the data and the load patterns can vary — for example:

- a stream of messages in the shape of market data;
- arbitrary messages (fixed or variable size) — sizes anywhere from a few bytes to a few hundred are worth considering.

If you pick a particular data format, justify the format and the sizes (they affect the result).

The producer generates several types of market messages (trade, BBO, and a 5-level-per-side order book).

### Metrics

In your solutions we'll pay particular attention to end-to-end "producer → consumer" delivery latency, at minimum by percentiles:

```
p50, p99, p99.9, p99.99
```

It's worth looking at these latencies as a function of:

- **message rate** (as the rate grows — all the way to saturation),
- **number of receivers** (as fan-out grows).

Looking at the whole distribution, not only the percentiles, can also be useful.

The main **differentiation** comes from the high percentiles (p99, p99.9, p99.99) and from how the tail of the distribution holds up under load.

## Rules of the game

- **This is a networking problem.** You can and should use several machines (two laptops, a home network, your own or cloud servers) and a real network. The transport is your choice and part of the task (TCP, UDP, multicast, kernel bypass, and so on).
- **Any tools are allowed.** Any languages, libraries, libraries.
- **Reproducibility is mandatory.** The environment, the topology, how to run it, what hardware/network you measured on — all of that has to be recorded in your write-up.
- **Deadline:** we accept solutions until 23:59 GMT+3 on 30 August 2026.
- **Submission format:** described in [this document](how_to_submit_solution_en.md).

By taking part in the competition you agree to the rules: https://spectral.tech/polozheniya-o-konkurse. If you have questions, write to the bot and we'll answer. https://t.me/spectral_challenge_bot

## How we evaluate solutions

We start by reading the code, the notebook, and the write-up. After that we run the solution in the following environment:

**Hardware and OS**

- **m7i / m8a** instances on AWS. If your solution needs one specific type out of those two, say so in your write-up and we'll pin it. In specific cases, we can try other AWS EC2 instances.
- Distribution: **Ubuntu 24.04** or **Ubuntu 20.04**.
- The NICs on these machines are **ENA** adapters.
- You're welcome to go into the kernel. Include the patches and the kernel version with your solution and we'll build a kernel with them for the test environment.

**Topology**

- The baseline run is **two servers in the same subnet** — so, L2 connectivity.
- After that, depending on the solution and the numbers it produces, we test over real **L2 and L3** links between several servers, with varying latency and packet loss.
- **1 to 3 receivers** to start with, but possibly more — depending on what the solution supports and what results it shows.

**Reproducibility**

Your solution really does need clear instructions on how to run it. If we can't reproduce your results even approximately, that can be grounds for disqualification.

At the same time, we understand that the numbers on our bench and on yours may differ noticeably — the setups aren't the same. What we look at is whether the behavior reproduces and whether the conclusions hold up, not whether the digits match exactly.
