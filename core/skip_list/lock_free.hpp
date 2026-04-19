#pragma once
#include <atomic>
#include <vector>
#include <climits>
#include <random>
#include <optional>
#include <cstdint>

static const int LF_MAX_LEVEL = 16;
static const float LF_P = 0.5f;

// Mark pointer trick: lowest bit = logical deletion mark
// We pack mark into the pointer itself using tagged pointer
struct LFNode;

struct MarkedPtr {
    std::atomic<uintptr_t> ptr; // lowest bit = mark

    MarkedPtr() : ptr(0) {}
    MarkedPtr(LFNode* p, bool mark = false)
        : ptr(reinterpret_cast<uintptr_t>(p) | (mark ? 1 : 0)) {}

    LFNode* getPtr() const {
        return reinterpret_cast<LFNode*>(ptr.load(std::memory_order_acquire) & ~1ULL);
    }

    bool getMark() const {
        return ptr.load(std::memory_order_acquire) & 1;
    }

    bool CAS(LFNode* expectedPtr, bool expectedMark,
             LFNode* newPtr, bool newMark) {
        uintptr_t expected = reinterpret_cast<uintptr_t>(expectedPtr) | (expectedMark ? 1 : 0);
        uintptr_t desired  = reinterpret_cast<uintptr_t>(newPtr)      | (newMark      ? 1 : 0);
        return ptr.compare_exchange_strong(expected, desired,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
    }

    void set(LFNode* p, bool mark) {
        ptr.store(reinterpret_cast<uintptr_t>(p) | (mark ? 1 : 0),
                  std::memory_order_release);
    }
};

struct LFNode {
    int key;
    std::atomic<int> value;
    int topLevel;
    std::vector<MarkedPtr> next;

    LFNode(int k, int v, int level)
        : key(k), value(v), topLevel(level), next(level + 1) {}
};

class LockFreeSkipList {
    LFNode* head;
    LFNode* tail;
    std::mt19937 rng;

    int randomLevel() {
        int lvl = 0;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        while (dist(rng) < LF_P && lvl < LF_MAX_LEVEL) lvl++;
        return lvl;
    }

    bool find(int key, std::vector<LFNode*>& preds, std::vector<LFNode*>& succs) {
    retry:
        LFNode* pred = head;
        for (int level = LF_MAX_LEVEL; level >= 0; level--) {
            LFNode* cur = pred->next[level].getPtr();
            while (true) {
                // check at current level
                LFNode* succ = cur->next[level].getPtr();
                bool marked = cur->next[level].getMark();

                while (marked) {
                    // physically remove cur
                    bool snipped = pred->next[level].CAS(cur, false, succ, false);
                    if (!snipped) goto retry;
                    cur = succ;
                    succ = cur->next[level].getPtr();
                    marked = cur->next[level].getMark();
                }

                if (cur->key < key) {
                    pred = cur;
                    cur = succ;
                } else {
                    break;
                }
            }
            preds[level] = pred;
            succs[level] = cur;
        }
        return succs[0]->key == key;
    }

public:
    LockFreeSkipList() : rng(std::random_device{}()) {
        head = new LFNode(INT_MIN, 0, LF_MAX_LEVEL);
        tail = new LFNode(INT_MAX, 0, LF_MAX_LEVEL);
        for (int i = 0; i <= LF_MAX_LEVEL; i++)
            head->next[i].set(tail, false);
    }

    ~LockFreeSkipList() {
        LFNode* cur = head;
        while (cur) {
            LFNode* nxt = cur->next[0].getPtr();
            delete cur;
            cur = nxt;
        }
    }

    bool insert(int key, int value) {
        int topLevel = randomLevel();
        std::vector<LFNode*> preds(LF_MAX_LEVEL + 1);
        std::vector<LFNode*> succs(LF_MAX_LEVEL + 1);

        while (true) {
            bool found = find(key, preds, succs);
            if (found) {
                succs[0]->value.store(value, std::memory_order_release);
                return false;
            }

            LFNode* node = new LFNode(key, value, topLevel);
            for (int level = 0; level <= topLevel; level++) {
                node->next[level].set(succs[level], false);
            }

            // try to splice in at level 0 first
            LFNode* pred = preds[0];
            LFNode* succ = succs[0];
            node->next[0].set(succ, false);

            if (!pred->next[0].CAS(succ, false, node, false)) {
                delete node;
                continue;
            }

            // now link upper levels
            for (int level = 1; level <= topLevel; level++) {
                while (true) {
                    pred = preds[level];
                    succ = succs[level];
                    if (pred->next[level].CAS(succ, false, node, false)) break;
                    find(key, preds, succs); // refresh preds/succs
                }
            }
            return true;
        }
    }

    bool remove(int key) {
        std::vector<LFNode*> preds(LF_MAX_LEVEL + 1);
        std::vector<LFNode*> succs(LF_MAX_LEVEL + 1);

        bool found = find(key, preds, succs);
        if (!found) return false;

        LFNode* victim = succs[0];
        int topLevel = victim->topLevel;

        // logically mark all levels from top down
        for (int level = topLevel; level >= 1; level--) {
            LFNode* succ;
            bool marked;
            do {
                succ   = victim->next[level].getPtr();
                marked = victim->next[level].getMark();
                if (marked) break;
            } while (!victim->next[level].CAS(succ, false, succ, true));
        }

        // mark level 0 — this is the linearization point
        LFNode* succ;
        bool marked;
        do {
            succ   = victim->next[0].getPtr();
            marked = victim->next[0].getMark();
            if (marked) return false; // someone else got it
        } while (!victim->next[0].CAS(succ, false, succ, true));

        // physically remove
        find(key, preds, succs);
        return true;
    }

    std::optional<int> findMinKey() {
        LFNode* cur = head->next[0].getPtr();
        while (cur != tail && cur->next[0].getMark())
            cur = cur->next[0].getPtr();
        if (cur == tail) return std::nullopt;
        return cur->key;
    }

    std::optional<int> findMin() {
        LFNode* cur = head->next[0].getPtr();
        while (cur != tail && cur->next[0].getMark())
            cur = cur->next[0].getPtr();
        if (cur == tail) return std::nullopt;
        return cur->value.load(std::memory_order_acquire);
    }
};
