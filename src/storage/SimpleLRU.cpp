#include "SimpleLRU.h"

namespace Afina {
namespace Backend {

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Put(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > _max_size) { return false; }
    auto it = _lru_index.find(std::cref(key));
    if (it == _lru_index.end()) {
        put(key, value);
    } else {
        set(&(it->second.get()), value);
    }
    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::PutIfAbsent(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > _max_size) { return false; }
    if (_lru_index.find(std::cref(key)) != _lru_index.end()) { return false; }
    put(key, value);
    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Set(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > _max_size) { return false; }
    auto it = _lru_index.find(std::cref(key));
    if (it == _lru_index.end()) { return false; }
    set(&(it->second.get()), value);
    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Delete(const std::string &key) {
    auto it = _lru_index.find(std::cref(key));
    if (it == _lru_index.end()) { return false; }
    lru_node *node_ptr = &(it->second.get());
    _lru_index.erase(it);
    if (node_ptr == _lru_head.get()) {
        node_ptr->next->prev = node_ptr->prev;
        _lru_head = std::move(node_ptr->next);
    } else {
        if (node_ptr->next) {
            node_ptr->next->prev = node_ptr->prev;
        } else {
            _lru_head->prev = node_ptr->prev;
        }
        node_ptr->prev->next = std::move(node_ptr->next);
    }
    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Get(const std::string &key, std::string &value) {
    auto it = _lru_index.find(std::cref(key));
    if (it == _lru_index.end()) { return false; }
    value = it->second.get().value;
    to_head(&(it->second.get()));
    return true;
}

void SimpleLRU::to_head(lru_node *node_ptr) {
    if (node_ptr != _lru_head.get()) {
        if (!node_ptr->next) {
            node_ptr->next = std::move(_lru_head);
            _lru_head = std::move(node_ptr->prev->next);
            node_ptr->prev->next = nullptr;
        } else {
            node_ptr->next->prev = node_ptr->prev;
            std::unique_ptr<lru_node> new_head = std::move(node_ptr->prev->next);
            node_ptr->prev->next = std::move(node_ptr->next);
            node_ptr->next = std::move(_lru_head);
            _lru_head = std::move(new_head);
            new_head = nullptr;
            node_ptr->prev = node_ptr->next->prev;
            node_ptr->next->prev = node_ptr;
        }
    } 
}

void SimpleLRU::set(lru_node *node_ptr, const std::string &value) {
    to_head(node_ptr);
    _curr_size -= node_ptr->value.size();
    if (value.size() > _max_size - _curr_size) {
        lru_node *last_deleted = _lru_head.get();
        while (value.size() > _max_size - _curr_size) {
            last_deleted = last_deleted->prev;
            last_deleted->next.reset();
            auto it = _lru_index.find(std::cref(last_deleted->key));
            assert(it != _lru_index.end());
            _lru_index.erase(it);
            _curr_size -= last_deleted->key.size() + last_deleted->value.size();
        }
        _lru_head->prev = last_deleted->prev;
        last_deleted->prev->next.reset();
    }
    _curr_size += value.size();
    node_ptr->value = value;
}

void SimpleLRU::put(const std::string &key, const std::string &value)  {
    if (key.size() + value.size() > _max_size - _curr_size) {
        lru_node *last_deleted = _lru_head.get();
        while (key.size() + value.size() > _max_size - _curr_size) {
            last_deleted = last_deleted->prev;
            last_deleted->next.reset();
            _lru_index.erase(_lru_index.find(std::cref(last_deleted->key)));
            _curr_size -= last_deleted->key.size() + last_deleted->value.size();
        }
        if (last_deleted == _lru_head.get()) { _lru_head = nullptr; }
        else {
            _lru_head->prev = last_deleted->prev;
            last_deleted->prev->next.reset();
        }
    }
    _curr_size += key.size() + value.size();
    if (!_lru_head) {
        _lru_head = std::unique_ptr<lru_node>(new lru_node(key));
        _lru_head->value = value;
        _lru_head->prev = _lru_head.get();
        _lru_head->next = nullptr;
    } else {
        std::unique_ptr<lru_node> new_node = std::unique_ptr<lru_node>(new lru_node(key));
        new_node->value = value;
        new_node->prev = _lru_head->prev;
        _lru_head->prev = new_node.get();
        new_node->next = std::move(_lru_head);
        _lru_head = std::move(new_node);
    }
    _lru_index.emplace(std::cref(_lru_head->key), std::ref(*_lru_head));
}

} // namespace Backend
} // namespace Afina
