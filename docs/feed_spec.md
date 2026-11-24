# Market Data Feed — Wire Specification

Version 0.1 · W11·2

The exchange publishes every change to its order book as a sequenced, fixed-layout binary feed. A receiver that starts from an empty book (or a snapshot, once W11·7c–d land) and applies every message in sequence order holds exactly the exchange's book.

## Model

- **Level 3, order by order.** Every book message names an order id.
- **Book events, not requests.** The feed carries what happened to the book (Add, Execute, Delete, Reduce), never what a client asked for. A modify is expressed through these events.
- **Facts, not instructions.** A receiver never re-runs matching. It applies each message as stated.
- **Anonymous.** No message carries an aggressor id or any client identity. Clients learn about their own fills through order entry, not the feed.

## Byte order and encoding

- All multi-byte fields are **little-endian**.
- Every field is packed and unpacked with **`uint64_t`** shifts and masks. A right shift on a negative `int64_t` copies the sign bit into the vacated bits and corrupts the extracted field.
- Words are moved between a `uint64_t` and the byte buffer with **`std::memcpy`**. Implementations guard the native byte order with `static_assert(std::endian::native == std::endian::little)`.
- `reinterpret_cast` of a byte buffer to `uint64_t*` or to a struct pointer is not used. It violates strict aliasing and may be misaligned (both undefined behaviour).

## Transport

- One packet per UDP datagram. The payload is at most **1,472 bytes** (1,500-byte Ethernet MTU − 20-byte IP header − 8-byte UDP header).
- A packet is a 16-byte header followed by 0 to 91 messages of 16 bytes each: 16 + 91 × 16 = 1,472.
- Multicast group, port and the retransmission/snapshot channels are defined in W11·7a, W11·7c and W11·9.

## Packet header (16 bytes)

| Bytes | Field | Contents |
|---|---|---|
| 0–7 | Header word | sequence number (bits 0–55) · message count (bits 56–62) · reserved, always 0 (bit 63) |
| 8–15 | Timestamp | `uint64_t` nanoseconds since the Unix epoch, taken from the exchange's clock when the packet is sealed for sending |

- **Sequence number:** the sequence number of the first message in the packet. Message *n* (0-based) in the packet has sequence number `first + n`.
- **Count:** the number of messages that follow, 0 to 91. The 7-bit field could encode up to 127, but 91 is the most that fits in a datagram.
- **Timestamp:** one per packet, so every message in a packet shares it. Its resolution is bounded by how long the publisher batches before sealing a packet; measure that batching window in W11·7 before relying on the timestamp for per-message latency.

### Sequence rules

- Sequence numbers start at **1** for the first message of a session and increase by exactly 1 per message. 0 is never used.
- The sequence number is the publisher's own counter. It is unrelated to `Order::seq`, which is the engine's queue-priority counter.
- A receiver tracks `expected`, the next sequence number it needs. On each packet:
  - `first == expected`: apply all messages; `expected += count`.
  - `first > expected`: **gap** of `[expected, first − 1]`. Stop trusting the book, buffer this packet, and recover (W11·7a–d).
  - `first + count <= expected`: duplicate, discard.
  - `first < expected < first + count`: partial overlap (for example after a retransmission). Apply only messages with sequence ≥ `expected`.

### Heartbeat

A packet with **count = 0** is a heartbeat. Its sequence field holds the next sequence number the publisher will use, so a receiver detects a gap even when no market activity follows the lost packet. The publisher sends one whenever no packet has gone out for a fixed interval (set in W11·7a).

## Message (16 bytes, every type)

| Bytes | Field | Contents |
|---|---|---|
| 0–7 | Field B | type · side · price · quantity, bit-packed (below) |
| 8–15 | Field A | order id: the engine's `int64_t` id, carried as its 64 bits unchanged |

Field B goes first so that a message's type sits at the same position in every message, including any future message that carries no order id.

### Field B bit layout

| Bits | Width | Field | Values |
|---|---|---|---|
| 0–1 | 2 | type | `00` Add · `01` Execute · `10` Delete · `11` Reduce |
| 2 | 1 | side | `0` Buy · `1` Sell |
| 3–33 | 31 | price | integer ticks, 1 to 2,147,483,647 |
| 34–63 | 30 | quantity | 1 to 1,073,741,823 |

Because type occupies the lowest bits and words are little-endian, the type is in the lowest two bits of **byte 0** of every message.

Fields a message type does not use are written as 0 and ignored by receivers.

### Message types

| Type | Meaning | Uses | Receiver action |
|---|---|---|---|
| **Add** | A new order rests on the book | id, side, price, quantity | Insert at the back of the queue at that side and price |
| **Execute** | A resting order traded | id, price (the trade price, which is the resting order's price), quantity filled | Subtract quantity; remove the order when it reaches 0 |
| **Delete** | A resting order left the book other than by trading | id | Remove the order |
| **Reduce** | A resting order's quantity was reduced in place | id, side, quantity (reduce **by**, not the new quantity) | Subtract quantity; remove the order when it reaches 0 |

- **Removal at zero is implied.** A full fill is one Execute, never an Execute followed by a Delete.
- Execute and Reduce are the same operation for a receiver; they are separate types because a trade and a client reduction mean different things to anyone reading the feed.

### How engine operations map to messages

| Engine operation | Messages |
|---|---|
| Limit order rests (no trade) | Add |
| Incoming order trades against resting orders | one Execute per resting order touched; then Add for any limit remainder that rests |
| Client cancel | Delete |
| Modify: quantity down, same price | Reduce (by old − new); keeps queue priority |
| Modify: price change, or quantity up | Delete, then whatever the replacement produces (Executes if it crosses, Add if it rests); same id |
| Modify: quantity to 0 | Delete |
| Rejected request | nothing |

W11·3 confirms this mapping against every place the engine changes the book.

## Engine limits

The engine's `validate` must reject anything the feed cannot represent:

- limit price: 1 to 2,147,483,647 ticks
- quantity: 1 to 1,073,741,823

## Worked example

An Execute of 30 at price 101 against resting order 7, inside a packet whose first message is sequence 1,000 and which holds 3 messages.

**Header word:** `1000 | (3 << 56)` = `0x03000000000003E8`

```
bytes 0–7:   e8 03 00 00 00 00 00 03
```

**Field B:** type Execute (`01`), side 0 (unused), price 101, quantity 30

```
1 | (0 << 2) | (101 << 3) | (30 << 34)  =  0x0000007800000329
bytes 0–7:   29 03 00 00 78 00 00 00
```

**Field A:** id 7

```
bytes 8–15:  07 00 00 00 00 00 00 00
```

Decoding byte 0 of the message: `0x29 & 0b11 = 01`, so Execute.

## Not yet specified

- Retransmission request and reply format, cache size, serving thread (W11·7a)
- Snapshot format and channel (W11·7c)
- Heartbeat interval (W11·7a)
- Multicast group and port (W11·9)

## Decision record

| Decision | Chosen | Rejected and why |
|---|---|---|
| Feed model | L3 book events | Request log: forces receivers to re-run matching |
| Field order | Field B at bytes 0–7 | Id first: an id-less message would need a sentinel, and id 0 is a real id |
| Fifth message type (heartbeat) | Count-0 packet; type stays 2 bits | 3-bit type: halves the price or quantity cap |
| Bit positions | type, side, price, quantity from bit 0 up | No performance or correctness difference; type at the bottom puts it in byte 0 |
| Byte order | Little-endian | Big-endian: a byte swap per word on every supported machine, for no gain |
| Read/write | `memcpy` with endian `static_assert` | `reinterpret_cast`: strict aliasing and alignment UB · byte-by-byte shifts and `std::bit_cast`: legal but no benefit here |
| Timestamp | Per packet, ns since epoch, in the header | Per message: adds 8 bytes to every message · none: receivers and the backtester have no exchange time |
