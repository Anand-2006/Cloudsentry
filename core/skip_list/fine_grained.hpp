#pragma once
#include <mutex>
#include <vector>
#include <climits>
#include <random>
#include <atomic>
#include <optional>

static const int FG_MAX_LEVEL = 8;
static const float FG_P = 0.5f;

struct FGNode {
    int key, topLevel;
    std::atomic<int>  value;
    std::atomic<bool> marked{false};
    std::mutex        mtx;
    std::vector<FGNode*> next;
    FGNode(int k, int v, int lvl)
        : key(k), topLevel(lvl), value(v), next(lvl+1, nullptr) {}
};

class FineGrainedSkipList {
    FGNode* head;
    FGNode* tail;
    std::mt19937 rng;

    int randomLevel() {
        int lvl = 0;
        std::uniform_real_distribution<float> d(0.f,1.f);
        while (d(rng) < FG_P && lvl < FG_MAX_LEVEL) lvl++;
        return lvl;
    }

    void locate(int key,
                std::vector<FGNode*>& preds,
                std::vector<FGNode*>& succs) const {
        FGNode* pred = head;
        for (int lv = FG_MAX_LEVEL; lv >= 0; lv--) {
            FGNode* curr = pred->next[lv];
            while (curr->key < key) { pred = curr; curr = pred->next[lv]; }
            preds[lv] = pred;
            succs[lv] = curr;
        }
    }

public:
    FineGrainedSkipList() : rng(std::random_device{}()) {
        head = new FGNode(INT_MIN, 0, FG_MAX_LEVEL);
        tail = new FGNode(INT_MAX, 0, FG_MAX_LEVEL);
        for (int i = 0; i <= FG_MAX_LEVEL; i++) head->next[i] = tail;
    }

    ~FineGrainedSkipList() {
        FGNode* c = head;
        while (c) { FGNode* n = c->next[0]; delete c; c = n; }
    }

    bool insert(int key, int value) {
        int newLvl = randomLevel();
        std::vector<FGNode*> preds(FG_MAX_LEVEL+1), succs(FG_MAX_LEVEL+1);

        while (true) {
            locate(key, preds, succs);

            if (succs[0]->key == key) {
                if (!succs[0]->marked.load()) { succs[0]->value.store(value); return false; }
                continue; // being deleted, retry
            }

            // lock preds bottom-up
            std::vector<std::unique_lock<std::mutex>> locks;
            bool valid = true;
            for (int lv = 0; lv <= newLvl && valid; lv++) {
                // only lock if not already locked (same pred at multiple levels)
                if (locks.empty() || preds[lv] != preds[lv-1]) // may differ
                    locks.emplace_back(preds[lv]->mtx);
                valid = !preds[lv]->marked.load() && preds[lv]->next[lv] == succs[lv];
            }
            if (!valid) continue;

            FGNode* node = new FGNode(key, value, newLvl);
            for (int lv = 0; lv <= newLvl; lv++) {
                node->next[lv] = succs[lv];
                preds[lv]->next[lv] = node;
            }
            return true;
        }
    }

    bool remove(int key) {
        std::vector<FGNode*> preds(FG_MAX_LEVEL+1), succs(FG_MAX_LEVEL+1);
        locate(key, preds, succs);
        FGNode* victim = succs[0];
        if (victim->key != key) return false;

        // lock and mark victim
        {
            std::unique_lock<std::mutex> vl(victim->mtx);
            if (victim->marked.load()) return false;
            victim->marked.store(true, std::memory_order_release);
        }

        // unlink at each level
        for (int lv = victim->topLevel; lv >= 0; lv--) {
            while (true) {
                std::unique_lock<std::mutex> pl(preds[lv]->mtx);
                if (preds[lv]->marked.load()) {
                    // pred got deleted, re-locate
                    locate(key, preds, succs);
                    break;
                }
                if (preds[lv]->next[lv] == victim) {
                    preds[lv]->next[lv] = victim->next[lv];
                    break;
                }
                // victim already unlinked at this level by another thread
                break;
            }
        }
        return true;
    }

    std::optional<int> findMin() const {
        FGNode* c = head->next[0];
        while (c != tail && c->marked.load(std::memory_order_acquire))
            c = c->next[0];
        if (c == tail) return std::nullopt;
        return c->value.load();
    }

    std::optional<int> findMinKey() const {
        FGNode* c = head->next[0];
        while (c != tail && c->marked.load(std::memory_order_acquire))
            c = c->next[0];
        if (c == tail) return std::nullopt;
        return c->key;
    }
};
