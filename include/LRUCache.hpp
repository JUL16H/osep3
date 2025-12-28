#pragma once
#include <list>
#include <memory>
#include <unordered_map>

template <typename Key, typename Val>
struct ICacheBackend {
    virtual ~ICacheBackend() = default;
    virtual Val load(Key key) = 0;
    virtual void save(Key, const Val &val) = 0;
};

template <typename Key, typename Val>
class LRUCache {
    struct CacheItem {
        Key key;
        std::shared_ptr<Val> val;
        bool dirty = false;
    };

public:
    LRUCache(size_t _capacity, std::shared_ptr<ICacheBackend<Key, Val>> _backend)
        : capacity(_capacity), backend(_backend) {}
    ~LRUCache() { flush_all(); }

    std::shared_ptr<const Val> get(Key key) { return access(key)->val; }
    std::shared_ptr<Val> get_mut(Key key) {
        auto it = access(key);
        it->dirty = true;
        return it->val;
    }
    void flush_all() {
        for (auto &item : cache_list) {
            if (!item.dirty)
                continue;
            backend->save(item.key, *item.val);
            item.dirty = false;
        }
    }

    void clear() {
        flush_all();
        cache_list.clear();
        cache_map.clear();
    }

    void remove(Key key) {
        auto it = cache_map.find(key);
        if (it == cache_map.end())
            return;
        cache_list.erase(it->second);
        cache_map.erase(it);
    }

private:
    typename std::list<CacheItem>::iterator access(Key key) {
        if (auto it = cache_map.find(key); it != cache_map.end()) {
            cache_list.splice(cache_list.begin(), cache_list, it->second);
            return it->second;
        }

        if (cache_list.size() >= capacity)
            evict();

        auto val = std::make_shared<Val>(backend->load(key));
        cache_list.push_front(CacheItem{.key = key, .val = val, .dirty = false});
        cache_map[key] = cache_list.begin();
        return cache_list.begin();
    }

    void evict() {
        for (auto it = cache_list.rbegin(); it != cache_list.rend(); it++) {
            if (it->val.use_count() != 1)
                continue;

            if (it->dirty)
                backend->save(it->key, *it->val);

            cache_map.erase(it->key);
            cache_list.erase(std::next(it).base());
            return;
        }
    }

private:
    size_t capacity;
    std::shared_ptr<ICacheBackend<Key, Val>> backend;
    std::list<CacheItem> cache_list;
    std::unordered_map<Key, typename std::list<CacheItem>::iterator> cache_map;
};
