#ifndef SPLAY_TREE_HPP
#define SPLAY_TREE_HPP

#include <iostream>

// Manual Splay Tree Implementation (Unit 1)
// Used for "Session Affinity" - caching client-to-server assignments.
// Recently used client lookups are amortized O(1).
struct SplayNode {
    int clientId;
    int serverId;
    SplayNode *left, *right, *parent;

    SplayNode(int cId, int sId) : clientId(cId), serverId(sId), left(nullptr), right(nullptr), parent(nullptr) {}
};

class SessionCache {
    SplayNode* root;

    void rightRotate(SplayNode* x) {
        SplayNode* y = x->left;
        x->left = y->right;
        if (y->right) y->right->parent = x;
        y->parent = x->parent;
        if (!x->parent) root = y;
        else if (x == x->parent->right) x->parent->right = y;
        else x->parent->left = y;
        y->right = x;
        x->parent = y;
    }

    void leftRotate(SplayNode* x) {
        SplayNode* y = x->right;
        x->right = y->left;
        if (y->left) y->left->parent = x;
        y->parent = x->parent;
        if (!x->parent) root = y;
        else if (x == x->parent->left) x->parent->left = y;
        else x->parent->right = y;
        y->left = x;
        x->parent = y;
    }

    void splay(SplayNode* x) {
        while (x->parent) {
            if (!x->parent->parent) {
                if (x == x->parent->left) rightRotate(x->parent);
                else leftRotate(x->parent);
            } else if (x == x->parent->left && x->parent == x->parent->parent->left) {
                rightRotate(x->parent->parent);
                rightRotate(x->parent);
            } else if (x == x->parent->right && x->parent == x->parent->parent->right) {
                leftRotate(x->parent->parent);
                leftRotate(x->parent);
            } else if (x == x->parent->left && x->parent == x->parent->parent->right) {
                rightRotate(x->parent);
                leftRotate(x->parent);
            } else {
                leftRotate(x->parent);
                rightRotate(x->parent);
            }
        }
    }

public:
    SessionCache() : root(nullptr) {}

    void insert(int clientId, int serverId) {
        if (!root) {
            root = new SplayNode(clientId, serverId);
            return;
        }
        SplayNode* curr = root;
        SplayNode* last = nullptr;
        while (curr) {
            last = curr;
            if (clientId == curr->clientId) {
                curr->serverId = serverId;
                splay(curr);
                return;
            }
            if (clientId < curr->clientId) curr = curr->left;
            else curr = curr->right;
        }
        SplayNode* newNode = new SplayNode(clientId, serverId);
        newNode->parent = last;
        if (clientId < last->clientId) last->left = newNode;
        else last->right = newNode;
        splay(newNode);
    }

    int find(int clientId) {
        SplayNode* curr = root;
        while (curr) {
            if (clientId == curr->clientId) {
                splay(curr);
                return curr->serverId;
            }
            if (clientId < curr->clientId) curr = curr->left;
            else curr = curr->right;
        }
        return -1; // Not found
    }
};

#endif
