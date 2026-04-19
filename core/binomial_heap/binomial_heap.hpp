#pragma once
#include <vector>
#include <optional>
#include <stdexcept>
#include <functional>

struct BinomialNode {
    int key;
    int serverId;
    int degree;
    BinomialNode* parent;
    BinomialNode* child;
    BinomialNode* sibling;

    BinomialNode(int k, int sid)
        : key(k), serverId(sid), degree(0),
          parent(nullptr), child(nullptr), sibling(nullptr) {}
};

class BinomialHeap {
    BinomialNode* head; // head of root list

    BinomialNode* mergeTrees(BinomialNode* a, BinomialNode* b) {
        if (a->key > b->key) std::swap(a, b);
        b->parent = a;
        b->sibling = a->child;
        a->child = b;
        a->degree++;
        return a;
    }

    BinomialNode* mergeRootLists(BinomialNode* h1, BinomialNode* h2) {
        if (!h1) return h2;
        if (!h2) return h1;

        BinomialNode* result = nullptr;
        BinomialNode** tail = &result;

        while (h1 && h2) {
            if (h1->degree <= h2->degree) {
                *tail = h1; h1 = h1->sibling;
            } else {
                *tail = h2; h2 = h2->sibling;
            }
            (*tail)->sibling = nullptr;
            tail = &(*tail)->sibling;
        }
        *tail = h1 ? h1 : h2;
        return result;
    }

    BinomialNode* consolidate(BinomialNode* h) {
        if (!h) return nullptr;

        BinomialNode* prev = nullptr;
        BinomialNode* cur = h;
        BinomialNode* next = cur->sibling;

        while (next) {
            if (cur->degree != next->degree ||
                (next->sibling && next->sibling->degree == cur->degree)) {
                prev = cur;
                cur = next;
            } else {
                if (cur->key <= next->key) {
                    cur->sibling = next->sibling;
                    mergeTrees(cur, next);
                } else {
                    if (!prev) h = next;
                    else prev->sibling = next;
                    mergeTrees(next, cur);
                    cur = next;
                }
            }
            next = cur->sibling;
        }
        return h;
    }

    void destroyTree(BinomialNode* node) {
        if (!node) return;
        destroyTree(node->child);
        destroyTree(node->sibling);
        delete node;
    }

public:
    BinomialHeap() : head(nullptr) {}

    ~BinomialHeap() { destroyTree(head); }

    // O(log n) insert
    void insert(int key, int serverId) {
        BinomialNode* node = new BinomialNode(key, serverId);
        BinomialHeap tmp;
        tmp.head = node;
        head = consolidate(mergeRootLists(head, tmp.head));
        tmp.head = nullptr;
    }

    // O(log n) merge — THE key operation for failover
    void merge(BinomialHeap& other) {
        head = consolidate(mergeRootLists(head, other.head));
        other.head = nullptr;
    }

    // O(log n) find min
    std::optional<std::pair<int,int>> findMin() const {
        if (!head) return std::nullopt;
        BinomialNode* minNode = head;
        BinomialNode* cur = head->sibling;
        while (cur) {
            if (cur->key < minNode->key) minNode = cur;
            cur = cur->sibling;
        }
        return std::make_pair(minNode->key, minNode->serverId);
    }

    // O(log n) extract min
    std::optional<std::pair<int,int>> extractMin() {
        if (!head) return std::nullopt;

        // find min root
        BinomialNode* minPrev = nullptr;
        BinomialNode* minNode = head;
        BinomialNode* prev = nullptr;
        BinomialNode* cur = head;

        while (cur) {
            if (cur->key < minNode->key) {
                minNode = cur;
                minPrev = prev;
            }
            prev = cur;
            cur = cur->sibling;
        }

        // remove minNode from root list
        if (!minPrev) head = minNode->sibling;
        else minPrev->sibling = minNode->sibling;

        // reverse children of minNode
        BinomialNode* child = minNode->child;
        BinomialNode* reversedChild = nullptr;
        while (child) {
            BinomialNode* next = child->sibling;
            child->sibling = reversedChild;
            child->parent = nullptr;
            reversedChild = child;
            child = next;
        }

        head = consolidate(mergeRootLists(head, reversedChild));

        auto result = std::make_pair(minNode->key, minNode->serverId);
        delete minNode;
        return result;
    }

    bool empty() const { return head == nullptr; }

    int size() const {
        int count = 0;
        std::function<void(BinomialNode*)> countNodes = [&](BinomialNode* n) {
            if (!n) return;
            count++;
            countNodes(n->child);
            countNodes(n->sibling);
        };
        countNodes(head);
        return count;
    }
};
