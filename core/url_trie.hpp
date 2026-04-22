#ifndef URL_TRIE_HPP
#define URL_TRIE_HPP

#include <iostream>
#include <unordered_map>
#include <string>

// Custom Trie implementation (Unit 3) using std::unordered_map for flexibility.
class TrieNode {
public:
    std::unordered_map<char, TrieNode*> children;
    std::string zone;
    bool isTerminal;

    TrieNode() : zone(""), isTerminal(false) {}
};

class URLRouter {
    TrieNode* root;

public:
    URLRouter() {
        root = new TrieNode();
    }

    void addRoute(const std::string& path, const std::string& zone) {
        TrieNode* curr = root;
        for (char c : path) {
            if (curr->children.find(c) == curr->children.end()) {
                curr->children[c] = new TrieNode();
            }
            curr = curr->children[c];
        }
        curr->isTerminal = true;
        curr->zone = zone;
    }

    std::string route(const std::string& url) {
        TrieNode* curr = root;
        std::string lastFoundZone = "default";
        
        for (char c : url) {
            if (curr->children.find(c) == curr->children.end()) break;
            curr = curr->children[c];
            if (curr->isTerminal) {
                lastFoundZone = curr->zone;
            }
        }
        return lastFoundZone;
    }
};

#endif
