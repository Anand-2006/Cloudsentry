#ifndef SEGMENT_TREE_HPP
#define SEGMENT_TREE_HPP

#include <vector>
#include <algorithm>

// Manual Segment Tree Implementation (Unit 5)
// Supporting Range Maximum Queries (RMQ) for peak throughput analytics.
// Allows O(log n) updates and O(log n) range queries.
class PeakSegmentTree {
    int n;
    std::vector<int> tree;

public:
    PeakSegmentTree(int size = 60) {
        n = size;
        tree.assign(4 * n, 0);
    }

    void update(int idx, int val, int node, int start, int end) {
        if (start == end) {
            tree[node] = val;
            return;
        }
        int mid = (start + end) / 2;
        if (idx <= mid) update(idx, val, 2 * node, start, mid);
        else update(idx, val, 2 * node + 1, mid + 1, end);
        tree[node] = std::max(tree[2 * node], tree[2 * node + 1]);
    }

    int query(int l, int r, int node, int start, int end) {
        if (r < start || end < l) return 0;
        if (l <= start && end <= r) return tree[node];
        int mid = (start + end) / 2;
        return std::max(query(l, r, 2 * node, start, mid),
                        query(l, r, 2 * node + 1, mid + 1, end));
    }

    // Wrapper functions
    void set(int idx, int val) { update(idx, val, 1, 0, n - 1); }
    int getMaxRange(int l, int r) { return query(l, r, 1, 0, n - 1); }
};

#endif
