#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <bitset>
#include <cassert>
#include <memory>
#include <type_traits>

namespace ecs {
    using Entity = uint32_t;
    using EntityId = uint32_t;

    inline constexpr Entity INVALID_ENTITY = UINT32_MAX;
    inline constexpr std::size_t MAX_COMPONENTS = 32;

    using Signature = std::bitset<MAX_COMPONENTS>;

    struct ComponentRegistry {
        static inline uint32_t counter = 0;

        template<typename T>
        static uint32_t id() {
            static uint32_t cid = counter++;
            return cid;
        }
    };

    class IComponentPool {
    public:
        virtual ~IComponentPool() = default;

        virtual void remove(Entity e) = 0;

        virtual bool has(Entity e) const = 0;
    };

    template<typename T>
    class ComponentPool final : public IComponentPool {
    public:
        static constexpr std::size_t INITIAL_SPARSE = 4096;

        ComponentPool() { sparse_.resize(INITIAL_SPARSE, UINT32_MAX); }

        T &insert(Entity e, T comp) {
            if (e >= sparse_.size())
                sparse_.resize(e * 2 + 1, UINT32_MAX);

            assert(sparse_[e] == UINT32_MAX && "Component exists");

            sparse_[e] = static_cast<uint32_t>(dense_.size());
            dense_.push_back(std::move(comp));
            owners_.push_back(e);
            return dense_.back();
        }

        void remove(Entity e) override {
            if (e >= sparse_.size() || sparse_[e] == UINT32_MAX) return;

            uint32_t idx = sparse_[e];
            uint32_t last = static_cast<uint32_t>(dense_.size()) - 1;

            if (idx != last) {
                dense_[idx] = std::move(dense_[last]);
                owners_[idx] = owners_[last];
                sparse_[owners_[idx]] = idx;
            }

            dense_.pop_back();
            owners_.pop_back();
            sparse_[e] = UINT32_MAX;
        }

        [[nodiscard]] bool has(Entity e) const override {
            return e < sparse_.size() && sparse_[e] != UINT32_MAX;
        }

        T &get(Entity e) {
            assert(has(e));
            return dense_[sparse_[e]];
        }

        const T &get(Entity e) const {
            assert(has(e));
            return dense_[sparse_[e]];
        }

        std::vector<T> &data() { return dense_; }
        std::vector<Entity> &owners() { return owners_; }

    private:
        std::vector<uint32_t> sparse_;
        std::vector<T> dense_;
        std::vector<Entity> owners_;
    };

    class Registry {
    public:
        Entity create() {
            Entity e;
            if (!free_list_.empty()) {
                e = free_list_.back();
                free_list_.pop_back();
            } else {
                e = next_id_++;
                signatures_.resize(next_id_);
            }
            signatures_[e].reset();
            return e;
        }

        void destroy(Entity e) {
            for (auto &[_, pool]: pools_)
                pool->remove(e);
            signatures_[e].reset();
            free_list_.push_back(e);
        }

        bool alive(Entity e) const {
            return e < next_id_ && signatures_[e].any();
        }

        template<typename T, typename... Args>
        T &emplace(Entity e, Args &&... args) {
            auto &pool = get_or_create_pool<T>();
            signatures_[e].set(ComponentRegistry::id<T>());
            return pool.insert(e, T{std::forward<Args>(args)...});
        }

        template<typename T>
        void remove(Entity e) {
            get_pool<T>().remove(e);
            signatures_[e].reset(ComponentRegistry::id<T>());
        }

        template<typename T>
        T &get(Entity e) { return get_pool<T>().get(e); }

        template<typename T>
        const T &get(Entity e) const { return get_pool<T>().get(e); }

        template<typename T>
        bool has(Entity e) const {
            auto it = pools_.find(ComponentRegistry::id<T>());
            if (it == pools_.end()) return false;
            return it->second->has(e);
        }

        const Signature &signature(Entity e) const { return signatures_[e]; }

        template<typename... Ts, typename Func>
        void view(Func &&fn) {
            std::vector<Entity> *owners_ptr = smallest_pool<Ts...>();
            if (!owners_ptr) return;

            Signature required;
            (required.set(ComponentRegistry::id<Ts>()), ...);

            std::vector<Entity> snapshot = *owners_ptr;
            for (Entity eid: snapshot) {
                if (eid >= signatures_.size()) continue;
                if ((signatures_[eid] & required) == required)
                    fn(eid, get<Ts>(eid)...);
            }
        }

        EntityId entity_count() const { return next_id_ - static_cast<uint32_t>(free_list_.size()); }

    private:
        template<typename T>
        ComponentPool<T> &get_or_create_pool() {
            uint32_t cid = ComponentRegistry::id<T>();
            auto it = pools_.find(cid);
            if (it == pools_.end()) {
                auto pool = std::make_unique<ComponentPool<T> >();
                auto *ptr = pool.get();
                pools_[cid] = std::move(pool);
                return *ptr;
            }
            return *static_cast<ComponentPool<T> *>(it->second.get());
        }

        template<typename T>
        ComponentPool<T> &get_pool() {
            return *static_cast<ComponentPool<T> *>(pools_.at(ComponentRegistry::id<T>()).get());
        }

        template<typename T>
        const ComponentPool<T> &get_pool() const {
            return *static_cast<const ComponentPool<T> *>(pools_.at(ComponentRegistry::id<T>()).get());
        }

        template<typename... Ts>
        std::vector<Entity> *smallest_pool() {
            std::vector<Entity> *result = nullptr;
            std::size_t min_size = SIZE_MAX;
            auto check = [&]<typename T>() {
                auto it = pools_.find(ComponentRegistry::id<T>());
                if (it == pools_.end()) return;
                auto &pool = *static_cast<ComponentPool<T> *>(it->second.get());
                if (pool.owners().size() < min_size) {
                    min_size = pool.owners().size();
                    result = &pool.owners();
                }
            };
            (check.template operator()<Ts>(), ...);
            return result;
        }

        Entity next_id_ = 0;
        std::vector<Entity> free_list_;
        std::vector<Signature> signatures_;
        std::unordered_map<uint32_t, std::unique_ptr<IComponentPool> > pools_;
    };
}
