#!/usr/bin/env python3
"""Bounded EA LZH1 codec, independently derived from GN7E69 revision 0.

TERF COMP method 5 uses MSB-first bits, canonical Huffman trees described by
315 four-bit lengths, and a 32 KiB LZ window. It resembles Deflate but has
its own framing and a maximum match length of 227. No game assets live here.
"""
from __future__ import annotations

from bisect import bisect_right
from collections import Counter, defaultdict, deque
import heapq

MAX_OUTPUT = 8 << 20
LENGTH_BASE = (3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227)
LENGTH_BITS = (0,) * 8 + (1,) * 4 + (2,) * 4 + (3,) * 4 + (4,) * 4 + (5,) * 3 + (0,)
DIST_BASE = (1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
             8193, 12289, 16385, 24577)
DIST_BITS = (0,) * 4 + tuple(i for i in range(1, 14) for _ in range(2))


class Bits:
    """A checked MSB-first reader; truncated assets fail without zero fill."""

    def __init__(self, data: bytes):
        self.data, self.position = data, 0

    def get(self, count: int) -> int:
        if self.position + count > len(self.data) * 8:
            raise ValueError('truncated LZH1 stream')
        value = 0
        for _ in range(count):
            value = value * 2 + ((self.data[self.position // 8] >> (7 - self.position % 8)) & 1)
            self.position += 1
        return value


def codes(lengths: list[int]) -> dict[int, tuple[int, int]]:
    """Build canonical codes and reject oversubscribed or empty trees."""
    counts = Counter(lengths)
    counts[0] = 0
    result, code = {}, 0
    for width in range(1, 16):
        code = (code + counts[width - 1]) * 2
        start = code
        for symbol, length in enumerate(lengths):
            if length == width:
                result[symbol] = (code, width)
                code += 1
        if code > 1 << width:
            raise ValueError('oversubscribed LZH1 tree')
        code = start
    if not result:
        raise ValueError('empty LZH1 tree')
    return result


def _tree(reader: Bits, count: int) -> dict[tuple[int, int], int]:
    """Read the game's direct four-bit code-length vector."""
    return {(width, code): symbol for symbol, (code, width) in
            codes([reader.get(4) for _ in range(count)]).items()}


def _symbol(reader: Bits, tree: dict) -> int:
    value = 0
    for width in range(1, 16):
        value = value * 2 + reader.get(1)
        if (width, value) in tree:
            return tree[width, value]
    raise ValueError('invalid LZH1 symbol')


def decompress(data: bytes, size: int) -> bytes:
    """Decode exactly size bytes, including block ends and stream terminator."""
    if not 0 <= size <= MAX_OUTPUT:
        raise ValueError('LZH1 output exceeds limit')
    reader, output = Bits(data), bytearray()
    while reader.get(1) == 0:
        literal, distance = _tree(reader, 285), _tree(reader, 30)
        while True:
            symbol = _symbol(reader, literal)
            if symbol == 256:
                break
            if symbol < 256:
                output.append(symbol)
            else:
                index = symbol - 257
                length = LENGTH_BASE[index] + reader.get(LENGTH_BITS[index])
                index = _symbol(reader, distance)
                offset = DIST_BASE[index] + reader.get(DIST_BITS[index])
                if offset > len(output) or len(output) + length > size:
                    raise ValueError('invalid LZH1 match')
                for _ in range(length):
                    output.append(output[-offset])
            if len(output) > size:
                raise ValueError('LZH1 output exceeds declared size')
    if len(output) != size:
        raise ValueError('LZH1 output size differs')
    return bytes(output)


def _lengths(frequencies: Counter, count: int) -> list[int]:
    """Stable Huffman lengths; bounded uniform fallback handles pathological data."""
    frequencies = frequencies.copy()
    if not frequencies:
        frequencies[0] = 1
    if len(frequencies) == 1:
        frequencies[(next(iter(frequencies)) + 1) % count] = 1
    serial = count
    heap = [(frequency, symbol, symbol) for symbol, frequency in frequencies.items()]
    heapq.heapify(heap)
    while len(heap) > 1:
        af, _, a = heapq.heappop(heap)
        bf, _, b = heapq.heappop(heap)
        heapq.heappush(heap, (af + bf, serial, (a, b)))
        serial += 1
    lengths = [0] * count

    def visit(node, depth):
        if isinstance(node, int):
            lengths[node] = depth
        else:
            visit(node[0], depth + 1)
            visit(node[1], depth + 1)

    visit(heap[0][2], 0)
    if max(lengths) > 15:
        width = (len(frequencies) - 1).bit_length()
        lengths = [width if symbol in frequencies else 0 for symbol in range(count)]
    return lengths


def compress(data: bytes) -> bytes:
    """Deterministic greedy LZ encoder; callers enforce the original slot budget.

    A bounded recent-candidate chain keeps builds inexpensive. Compression ratio
    is deliberately secondary to a small, reproducible implementation. Revisit
    optimal parsing only if validated edits exceed a TERF slot's aligned extent.
    """
    if len(data) > MAX_OUTPUT:
        raise ValueError('LZH1 input exceeds limit')
    chains = defaultdict(lambda: deque(maxlen=64))
    tokens, literal_freq, distance_freq = [], Counter({256: 1}), Counter()
    position = 0
    while position < len(data):
        best, offset = 0, 0
        key = data[position:position + 3]
        if len(key) == 3:
            for candidate in reversed(chains[key]):
                if position - candidate > 32768:
                    break
                length = 3
                while length < min(227, len(data) - position) and data[candidate + length] == data[position + length]:
                    length += 1
                if length > best:
                    best, offset = length, position - candidate
        if best >= 3:
            li, di = bisect_right(LENGTH_BASE, best) - 1, bisect_right(DIST_BASE, offset) - 1
            tokens.append((257 + li, best - LENGTH_BASE[li], di, offset - DIST_BASE[di]))
            literal_freq[257 + li] += 1
            distance_freq[di] += 1
            advance = best
        else:
            tokens.append((data[position],))
            literal_freq[data[position]] += 1
            advance = 1
        for index in range(position, position + advance):
            if index + 3 <= len(data):
                chains[data[index:index + 3]].append(index)
        position += advance
    ll, dl = _lengths(literal_freq, 285), _lengths(distance_freq, 30)
    lc, dc = codes(ll), codes(dl)
    output, accumulator, used = bytearray(), 0, 0

    def put(value, width):
        nonlocal accumulator, used
        accumulator = (accumulator << width) | value
        used += width
        while used >= 8:
            used -= 8
            output.append((accumulator >> used) & 255)
        accumulator &= (1 << used) - 1

    put(0, 1)
    for length in ll + dl:
        put(length, 4)
    for token in tokens:
        symbol = token[0]
        put(*lc[symbol])
        if symbol > 256:
            _, length_extra, di, distance_extra = token
            put(length_extra, LENGTH_BITS[symbol - 257])
            put(*dc[di])
            put(distance_extra, DIST_BITS[di])
    put(*lc[256])
    put(1, 1)
    if used:
        put(0, 8 - used)
    result = bytes(output)
    if decompress(result, len(data)) != data:
        raise ValueError('LZH1 encoder verification failed')
    return result
