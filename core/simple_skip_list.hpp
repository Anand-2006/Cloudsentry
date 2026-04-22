#pragma once
#include <vector>
#include <climits>
#include <random>
#include <iostream>

// A simple, single-threaded Skip List implementation for a 2nd year project.
// No lock-free or atomic complexity, just the core probabilistic DS.

struct Node {
    int key;
    int value;
    std::vector<Node*> next;

    Node(int k, int v, int level) : key(k), value(v), next(level + 1, nullptr) {}
};

class SimpleSkipList {
    int maxLevel;
    float p;
    Node* head;
    std::mt19937 rng;

    int randomLevel() {
        int lvl = 0;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        while (dist(rng) < p && lvl < maxLevel) lvl++;
        return lvl;
    }

public:
    explicit SimpleSkipList(int maxLvl = 16, float prob = 0.5f)
        : maxLevel(maxLvl), p(prob), rng(std::random_device{}()) {
        head = new Node(INT_MIN, 0, maxLevel);
    }

    ~SimpleSkipList() {
        Node* curr = head;
        while (curr) {
            Node* next = curr->next[0];
            delete curr;
            curr = next;
        }
    }

    void insert(int key, int value) {
        std::vector<Node*> update(maxLevel + 1);
        Node* curr = head;

        for (int i = maxLevel; i >= 0; i--) {
            while (curr->next[i] != nullptr && curr->next[i]->key < key) {
                curr = curr->next[i];
            }
            update[i] = curr;
        }

        curr = curr->next[0];

        if (curr != nullptr && curr->key == key) {
            curr->value = value;
        } else {
            int lvl = randomLevel();
            Node* newNode = new Node(key, value, lvl);
            for (int i = 0; i <= lvl; i++) {
                newNode->next[i] = update[i]->next[i];
                update[i]->next[i] = newNode;
            }
        }
    }

    bool remove(int key) {
        std::vector<Node*> update(maxLevel + 1);
        Node* curr = head;

        for (int i = maxLevel; i >= 0; i--) {
            while (curr->next[i] != nullptr && curr->next[i]->key < key) {
                curr = curr->next[i];
            }
            update[i] = curr;
        }

        curr = curr->next[0];

        if (curr != nullptr && curr->key == key) {
            for (int i = 0; i <= (int)curr->next.size() - 1; i++) {
                if (update[i]->next[i] != curr) break;
                update[i]->next[i] = curr->next[i];
            }
            delete curr;
            return true;
        }
        return false;
    }

    Node* search(int key) {
        Node* curr = head;
        for (int i = maxLevel; i >= 0; i--) {
            while (curr->next[i] != nullptr && curr->next[i]->key < key) {
                curr = curr->next[i];
            }
        }
        curr = curr->next[0];
        if (curr != nullptr && curr->key == key) return curr;
        return nullptr;
    }
};
