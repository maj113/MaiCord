"""Pack fixed-width pages by largest overlap without changing runtime lookup."""

from collections import defaultdict


def overlap_pages(values, shift):
    size = 1 << shift
    if len(values) % size:
        raise ValueError('Expected complete fixed-width pages')
    pages = [tuple(values[at:at + size]) for at in range(0, len(values), size)]
    count = len(pages)
    parents = list(range(count))
    following, incoming, overlaps = [None] * count, [False] * count, [0] * count

    def root(index):
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    for overlap in range(size - 1, 0, -1):
        starts = defaultdict(list)
        for index in reversed(range(count)):
            if not incoming[index]:
                starts[pages[index][:overlap]].append(index)
        for tail in range(count):
            if following[tail] is not None:
                continue
            candidates = starts[pages[tail][-overlap:]]
            for at in range(len(candidates) - 1, -1, -1):
                head = candidates[at]
                if root(tail) != root(head):
                    following[tail], incoming[head], overlaps[head] = head, True, overlap
                    parents[root(head)] = root(tail)
                    candidates.pop(at)
                    break

    flat, offsets = [], [None] * count
    for head in range(count):
        if incoming[head]:
            continue
        at = head
        while at is not None:
            offsets[at] = len(flat) - overlaps[at]
            flat.extend(pages[at][overlaps[at]:])
            at = following[at]
    if any(offset >= 0x8000 for offset in offsets):
        raise ValueError('Direct offset exceeds marker range')
    return offsets, flat


def compact_index(top, middle, leaves, leaf_shift, middle_shift):
    leaf_offsets, leaves = overlap_pages(leaves, leaf_shift)
    middle = [v if v & 0x8000 else leaf_offsets[v] for v in middle]
    middle_offsets, middle = overlap_pages(middle, middle_shift)
    top = [v if v & 0x8000 else middle_offsets[v] for v in top]
    return top, middle, leaves
