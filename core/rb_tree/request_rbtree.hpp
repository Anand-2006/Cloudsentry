#pragma once
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

// Red-Black Tree keyed on (deadline_ms, request_id)
// Supports:
//   insert     — O(log n)
//   extractMin — O(log n)  earliest deadline first
//   remove     — O(log n)  cancel a request mid-flight
//   updateKey  — O(log n)  reprioritize (remove + reinsert)

struct Request {
    long long deadlineMs;   // absolute deadline (lower = higher priority)
    int       requestId;
    int       clientId;
    int       priority;     // application-level priority hint (0=highest)
    long long arrivalMs;

    bool operator<(const Request& o) const {
        if (deadlineMs != o.deadlineMs) return deadlineMs < o.deadlineMs;
        return requestId < o.requestId;
    }
    bool operator==(const Request& o) const {
        return requestId == o.requestId;
    }
};

enum class Color { RED, BLACK };

struct RBNode {
    Request     data;
    Color       color;
    RBNode*     left;
    RBNode*     right;
    RBNode*     parent;

    explicit RBNode(const Request& r)
        : data(r), color(Color::RED),
          left(nullptr), right(nullptr), parent(nullptr) {}
};

class RequestRBTree {
    RBNode* root;
    RBNode* NIL;   // sentinel
    int     sz;

    void leftRotate(RBNode* x) {
        RBNode* y = x->right;
        x->right  = y->left;
        if (y->left != NIL) y->left->parent = x;
        y->parent = x->parent;
        if (x->parent == NIL)        root      = y;
        else if (x == x->parent->left) x->parent->left  = y;
        else                           x->parent->right = y;
        y->left   = x;
        x->parent = y;
    }

    void rightRotate(RBNode* x) {
        RBNode* y = x->left;
        x->left   = y->right;
        if (y->right != NIL) y->right->parent = x;
        y->parent = x->parent;
        if (x->parent == NIL)         root      = y;
        else if (x == x->parent->right) x->parent->right = y;
        else                            x->parent->left  = y;
        y->right  = x;
        x->parent = y;
    }

    void insertFixup(RBNode* z) {
        while (z->parent->color == Color::RED) {
            if (z->parent == z->parent->parent->left) {
                RBNode* y = z->parent->parent->right;
                if (y->color == Color::RED) {
                    z->parent->color          = Color::BLACK;
                    y->color                  = Color::BLACK;
                    z->parent->parent->color  = Color::RED;
                    z = z->parent->parent;
                } else {
                    if (z == z->parent->right) {
                        z = z->parent;
                        leftRotate(z);
                    }
                    z->parent->color         = Color::BLACK;
                    z->parent->parent->color = Color::RED;
                    rightRotate(z->parent->parent);
                }
            } else {
                RBNode* y = z->parent->parent->left;
                if (y->color == Color::RED) {
                    z->parent->color         = Color::BLACK;
                    y->color                 = Color::BLACK;
                    z->parent->parent->color = Color::RED;
                    z = z->parent->parent;
                } else {
                    if (z == z->parent->left) {
                        z = z->parent;
                        rightRotate(z);
                    }
                    z->parent->color         = Color::BLACK;
                    z->parent->parent->color = Color::RED;
                    leftRotate(z->parent->parent);
                }
            }
        }
        root->color = Color::BLACK;
    }

    void transplant(RBNode* u, RBNode* v) {
        if (u->parent == NIL)          root            = v;
        else if (u == u->parent->left) u->parent->left  = v;
        else                           u->parent->right = v;
        v->parent = u->parent;
    }

    RBNode* treeMin(RBNode* x) {
        while (x->left != NIL) x = x->left;
        return x;
    }

    void deleteFixup(RBNode* x) {
        while (x != root && x->color == Color::BLACK) {
            if (x == x->parent->left) {
                RBNode* w = x->parent->right;
                if (w->color == Color::RED) {
                    w->color         = Color::BLACK;
                    x->parent->color = Color::RED;
                    leftRotate(x->parent);
                    w = x->parent->right;
                }
                if (w->left->color == Color::BLACK && w->right->color == Color::BLACK) {
                    w->color = Color::RED;
                    x = x->parent;
                } else {
                    if (w->right->color == Color::BLACK) {
                        w->left->color = Color::BLACK;
                        w->color       = Color::RED;
                        rightRotate(w);
                        w = x->parent->right;
                    }
                    w->color         = x->parent->color;
                    x->parent->color = Color::BLACK;
                    w->right->color  = Color::BLACK;
                    leftRotate(x->parent);
                    x = root;
                }
            } else {
                RBNode* w = x->parent->left;
                if (w->color == Color::RED) {
                    w->color         = Color::BLACK;
                    x->parent->color = Color::RED;
                    rightRotate(x->parent);
                    w = x->parent->left;
                }
                if (w->right->color == Color::BLACK && w->left->color == Color::BLACK) {
                    w->color = Color::RED;
                    x = x->parent;
                } else {
                    if (w->left->color == Color::BLACK) {
                        w->right->color = Color::BLACK;
                        w->color        = Color::RED;
                        leftRotate(w);
                        w = x->parent->left;
                    }
                    w->color         = x->parent->color;
                    x->parent->color = Color::BLACK;
                    w->left->color   = Color::BLACK;
                    rightRotate(x->parent);
                    x = root;
                }
            }
        }
        x->color = Color::BLACK;
    }

    RBNode* findNode(const Request& r) {
        RBNode* cur = root;
        while (cur != NIL) {
            if (r < cur->data)       cur = cur->left;
            else if (cur->data < r)  cur = cur->right;
            else                     return cur;
        }
        return NIL;
    }

    // find by requestId only (for cancel/update by ID)
    RBNode* findById(int reqId, RBNode* node = nullptr) {
        if (node == nullptr) node = root;
        if (node == NIL) return NIL;
        if (node->data.requestId == reqId) return node;
        RBNode* l = findById(reqId, node->left);
        if (l != NIL) return l;
        return findById(reqId, node->right);
    }

    void destroyTree(RBNode* node) {
        if (node == NIL || node == nullptr) return;
        destroyTree(node->left);
        destroyTree(node->right);
        delete node;
    }

public:
    RequestRBTree() : root(nullptr), sz(0) {
        NIL         = new RBNode(Request{});
        NIL->color  = Color::BLACK;
        NIL->left   = NIL->right = NIL->parent = NIL;
        root        = NIL;
    }

    ~RequestRBTree() {
        destroyTree(root);
        delete NIL;
    }

    // O(log n) insert
    void insert(const Request& r) {
        RBNode* z = new RBNode(r);
        z->left = z->right = z->parent = NIL;

        RBNode* y = NIL;
        RBNode* x = root;
        while (x != NIL) {
            y = x;
            if (z->data < x->data) x = x->left;
            else                   x = x->right;
        }
        z->parent = y;
        if (y == NIL)              root     = z;
        else if (z->data < y->data) y->left  = z;
        else                        y->right = z;

        insertFixup(z);
        sz++;
    }

    // O(log n) extract minimum (earliest deadline)
    std::optional<Request> extractMin() {
        if (root == NIL) return std::nullopt;
        RBNode* minNode = treeMin(root);
        Request result  = minNode->data;

        // standard RB delete
        RBNode* y = minNode;
        RBNode* x;
        Color   yOrigColor = y->color;

        if (minNode->left == NIL) {
            x = minNode->right;
            transplant(minNode, minNode->right);
        } else if (minNode->right == NIL) {
            x = minNode->left;
            transplant(minNode, minNode->left);
        } else {
            y = treeMin(minNode->right);
            yOrigColor  = y->color;
            x           = y->right;
            if (y->parent == minNode) {
                x->parent = y;
            } else {
                transplant(y, y->right);
                y->right         = minNode->right;
                y->right->parent = y;
            }
            transplant(minNode, y);
            y->left         = minNode->left;
            y->left->parent = y;
            y->color        = minNode->color;
        }
        delete minNode;
        if (yOrigColor == Color::BLACK) deleteFixup(x);
        sz--;
        return result;
    }

    // O(log n) remove by requestId — for cancellations
    bool removeById(int requestId) {
        RBNode* node = findById(requestId);
        if (node == NIL) return false;

        RBNode* y = node;
        RBNode* x;
        Color   yOrigColor = y->color;

        if (node->left == NIL) {
            x = node->right;
            transplant(node, node->right);
        } else if (node->right == NIL) {
            x = node->left;
            transplant(node, node->left);
        } else {
            y = treeMin(node->right);
            yOrigColor  = y->color;
            x           = y->right;
            if (y->parent == node) {
                x->parent = y;
            } else {
                transplant(y, y->right);
                y->right         = node->right;
                y->right->parent = y;
            }
            transplant(node, y);
            y->left         = node->left;
            y->left->parent = y;
            y->color        = node->color;
        }
        delete node;
        if (yOrigColor == Color::BLACK) deleteFixup(x);
        sz--;
        return true;
    }

    // O(log n) reprioritize — update deadline of existing request
    bool updateDeadline(int requestId, long long newDeadlineMs) {
        RBNode* node = findById(requestId);
        if (node == NIL) return false;
        Request updated    = node->data;
        updated.deadlineMs = newDeadlineMs;
        removeById(requestId);
        insert(updated);
        return true;
    }

    // O(log n) peek at min without removing
    std::optional<Request> peekMin() const {
        if (root == NIL) return std::nullopt;
        RBNode* cur = root;
        while (cur->left != NIL) cur = cur->left;
        return cur->data;
    }

    bool   empty() const { return sz == 0; }
    int    size()  const { return sz; }
};
