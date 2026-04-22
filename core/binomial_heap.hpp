#ifndef BINOMIAL_HEAP_HPP
#define BINOMIAL_HEAP_HPP

#include <iostream>
#include <vector>
#include <list>
#include <algorithm>

// Custom Binomial Heap implementation (Unit 2)
// This version uses std::list for the root list for cleaner code.
template <typename T>
struct BinomialNode {
    T data;
    int degree;
    BinomialNode *child, *sibling, *parent;

    BinomialNode(T val) : data(val), degree(0), child(nullptr), sibling(nullptr), parent(nullptr) {}
};

template <typename T>
class BinomialHeap {
    std::list<BinomialNode<T>*> roots;
    int count = 0;

    void linkNodes(BinomialNode<T>* y, BinomialNode<T>* z) {
        y->parent = z;
        y->sibling = z->child;
        z->child = y;
        z->degree++;
    }

    std::list<BinomialNode<T>*> mergeRoots(BinomialHeap<T>& other) {
        std::list<BinomialNode<T>*> res;
        auto it1 = roots.begin();
        auto it2 = other.roots.begin();

        while (it1 != roots.end() && it2 != other.roots.end()) {
            if ((*it1)->degree <= (*it2)->degree) {
                res.push_back(*it1);
                it1++;
            } else {
                res.push_back(*it2);
                it2++;
            }
        }
        while (it1 != roots.end()) res.push_back(*it1++);
        while (it2 != other.roots.end()) res.push_back(*it2++);
        return res;
    }

public:
    BinomialHeap() {}

    bool empty() const { return roots.empty(); }
    int size() const { return count; }

    void insert(T val) {
        BinomialHeap<T> temp;
        temp.roots.push_back(new BinomialNode<T>(val));
        temp.count = 1;
        merge(temp);
    }

    void merge(BinomialHeap<T>& other) {
        count += other.count;
        other.count = 0;
        std::list<BinomialNode<T>*> newRoots = mergeRoots(other);
        roots.clear();
        other.roots.clear();

        if (newRoots.empty()) return;

        auto curr = newRoots.begin();
        auto next = std::next(curr);

        while (next != newRoots.end()) {
            if ((*curr)->degree != (*next)->degree || 
                (std::next(next) != newRoots.end() && (*std::next(next))->degree == (*curr)->degree)) {
                curr = next;
            } else {
                if ((*curr)->data <= (*next)->data) {
                    (*curr)->sibling = (*next)->sibling;
                    linkNodes(*next, *curr);
                    newRoots.erase(next);
                } else {
                    linkNodes(*curr, *next);
                    curr = newRoots.erase(curr);
                }
            }
            next = std::next(curr);
            if (curr == newRoots.end()) break;
        }
        roots = newRoots;
    }

    T extractMin() {
        if (empty()) return T();
        
        auto minIt = roots.begin();
        T minVal = (*minIt)->data;
        for (auto it = roots.begin(); it != roots.end(); ++it) {
            if ((*it)->data < minVal) {
                minVal = (*it)->data;
                minIt = it;
            }
        }

        BinomialNode<T>* minNode = *minIt;
        roots.erase(minIt);

        BinomialHeap<T> childrenHeap;
        BinomialNode<T>* child = minNode->child;
        while (child) {
            BinomialNode<T>* next = child->sibling;
            child->parent = nullptr;
            child->sibling = nullptr;
            childrenHeap.roots.push_front(child);
            child = next;
        }

        merge(childrenHeap);
        delete minNode;
        count--;
        return minVal;
    }
};

#endif
